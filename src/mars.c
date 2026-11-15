#include "mars.h"

#include <string.h>

/*
 * The SEGA 32X adapter: two SH-2 CPUs, SDRAM, two frame buffers, a 256 colour palette,
 * communication and interrupt registers, the 68000 to SH-2 FIFO and PWM sound.
 * Register layouts follow SEGA's 32X hardware manual; see docs/32X.md for the open questions.
 */

enum {
	ADAPTER_ADEN = 0x0001,
	ADAPTER_RES = 0x0002,
	ADAPTER_REN = 0x0080,
	ADAPTER_FM = 0x8000,
	DREQ_RV = 0x0001,
	DREQ_68S = 0x0004,
	DREQ_FULL = 0x0080,
	PENDING_PWM = 0x01,
	PENDING_CMD = 0x02,
	PENDING_H = 0x04,
	PENDING_V = 0x08,
	MASK_HEN = 0x0080,
	MODE_PRIORITY = 0x0080,
	MODE_240 = 0x0040,
	PWM_RTP = 0x0080,
	QUANTUM = 24
};

static bool running(const struct karashi_mars *mars) {
	return (mars->adapter & (ADAPTER_ADEN | ADAPTER_RES)) == (ADAPTER_ADEN | ADAPTER_RES);
}

static uint16_t get16(const uint8_t *data, uint32_t offset) {
	return (uint16_t) ((uint16_t) data[offset] << 8 | data[offset + 1]);
}

static void put16(uint8_t *data, uint32_t offset, uint16_t value) {
	data[offset] = (uint8_t) (value >> 8);
	data[offset + 1] = (uint8_t) value;
}

static karashi_pixel convert(uint16_t value) {
	unsigned red = value & 31;
	unsigned green = (value >> 5) & 31;
	unsigned blue = (value >> 10) & 31;

	return KARASHI_PIXEL(red << 3 | red >> 2, green << 3 | green >> 2, blue << 3 | blue >> 2);
}

static void update_irl(struct karashi_mars *mars, unsigned index) {
	uint8_t active = mars->pending[index] & (uint8_t) (mars->interrupt_mask[index] & 0x0f);
	unsigned level = 0;

	if (active & PENDING_V) level = 12;
	else if (active & PENDING_H) level = 10;
	else if (active & PENDING_CMD) level = 8;
	else if (active & PENDING_PWM) level = 6;
	karashi_sh2_set_irl(&mars->cpu[index], level);
}

static void raise(struct karashi_mars *mars, uint8_t source) {
	unsigned index;

	for (index = 0; index < 2; index++) {
		if (mars->interrupt_mask[index] & source) {
			mars->pending[index] |= source;
			update_irl(mars, index);
		}
	}
}

static void update_command(struct karashi_mars *mars) {
	unsigned index;

	for (index = 0; index < 2; index++) {
		if (mars->interrupt_control & (1u << index)) mars->pending[index] |= PENDING_CMD;
		else mars->pending[index] &= (uint8_t) ~PENDING_CMD;
		update_irl(mars, index);
	}
}

static unsigned pwm_period(const struct karashi_mars *mars) {
	unsigned period = (mars->pwm_cycle - 1u) & 0xfff;
	return period ? period : 0xfff;
}

static void pwm_push(struct karashi_mars *mars, unsigned channel, uint16_t value) {
	if (mars->pwm_count[channel] == 3) {
		mars->pwm_fifo[channel][0] = mars->pwm_fifo[channel][1];
		mars->pwm_fifo[channel][1] = mars->pwm_fifo[channel][2];
		mars->pwm_count[channel] = 2;
	}
	mars->pwm_fifo[channel][mars->pwm_count[channel]++] = value & 0xfff;
}

static uint16_t pwm_status(const struct karashi_mars *mars, unsigned channel) {
	uint16_t value = 0;

	if (mars->pwm_count[channel] == 3) value |= 0x8000;
	if (!mars->pwm_count[channel]) value |= 0x4000;
	return value;
}

static int16_t pwm_level(const struct karashi_mars *mars, unsigned source) {
	int32_t period = (int32_t) pwm_period(mars);
	int32_t level = ((int32_t) mars->pwm_width[source] * 2 - period) * 24000 / period;

	if (!mars->pwm_width[source]) return 0;
	return (int16_t) (level > 30000 ? 30000 : level < -30000 ? -30000 : level);
}

static void pwm_tick(struct karashi_mars *mars) {
	unsigned left_mode = mars->pwm_control & 3;
	unsigned right_mode = (mars->pwm_control >> 2) & 3;
	unsigned interval = (mars->pwm_control >> 8) & 15;
	unsigned channel;

	for (channel = 0; channel < 2; channel++) {
		if (!mars->pwm_count[channel]) continue;
		mars->pwm_width[channel] = mars->pwm_fifo[channel][0];
		mars->pwm_fifo[channel][0] = mars->pwm_fifo[channel][1];
		mars->pwm_fifo[channel][1] = mars->pwm_fifo[channel][2];
		mars->pwm_count[channel]--;
	}
	if (mars->pwm_frames < KARASHI_MARS_PWM_CAPACITY) {
		int16_t *sample = mars->pwm_audio + mars->pwm_frames * 2;
		sample[0] = left_mode == 1 ? pwm_level(mars, 0) : left_mode == 2 ? pwm_level(mars, 1) : 0;
		sample[1] = right_mode == 1 ? pwm_level(mars, 1) : right_mode == 2 ? pwm_level(mars, 0) : 0;
		mars->pwm_frames++;
	}
	if (++mars->pwm_timer >= (interval ? interval : 16)) {
		mars->pwm_timer = 0;
		raise(mars, PENDING_PWM);
		if (mars->pwm_control & PWM_RTP) {
			if (!karashi_sh2_dreq(&mars->cpu[0], 1)) karashi_sh2_dreq(&mars->cpu[1], 1);
		}
	}
}

static void pwm_advance(struct karashi_mars *mars, uint64_t time) {
	unsigned period;
	unsigned limit = KARASHI_MARS_PWM_CAPACITY;

	if (mars->in_pwm || time <= mars->pwm_time) return;
	if (!(mars->pwm_control & 0x0f)) {
		mars->pwm_time = time;
		return;
	}
	mars->in_pwm = true;
	period = pwm_period(mars);
	while (mars->pwm_time + period <= time && limit--) {
		mars->pwm_time += period;
		pwm_tick(mars);
	}
	if (mars->pwm_time + period <= time) mars->pwm_time = time;
	mars->in_pwm = false;
}

static void service_fifo(struct karashi_mars *mars) {
	unsigned guard = KARASHI_MARS_FIFO_SIZE * 2;

	while (mars->fifo_count && guard--) {
		if (!karashi_sh2_dreq(&mars->cpu[0], 0) && !karashi_sh2_dreq(&mars->cpu[1], 0)) break;
	}
}

static uint16_t fifo_pop(struct karashi_mars *mars) {
	uint16_t value;

	if (!mars->fifo_count) return mars->fifo[(mars->fifo_read + KARASHI_MARS_FIFO_SIZE - 1) % KARASHI_MARS_FIFO_SIZE];
	value = mars->fifo[mars->fifo_read];
	mars->fifo_read = (uint8_t) ((mars->fifo_read + 1) % KARASHI_MARS_FIFO_SIZE);
	mars->fifo_count--;
	if (mars->dreq_length && !--mars->dreq_length) mars->dreq_control &= (uint16_t) ~DREQ_68S;
	return value;
}

static void fifo_push(struct karashi_mars *mars, uint16_t value) {
	if (mars->fifo_count == KARASHI_MARS_FIFO_SIZE) return;
	mars->fifo[(mars->fifo_read + mars->fifo_count) % KARASHI_MARS_FIFO_SIZE] = value;
	mars->fifo_count++;
	service_fifo(mars);
}

static uint16_t dreq_status(const struct karashi_mars *mars) {
	return (uint16_t) ((mars->dreq_control & 7) | (mars->fifo_count == KARASHI_MARS_FIFO_SIZE ? DREQ_FULL : 0));
}

static bool vblank(const struct karashi_mars *mars) {
	return mars->line >= mars->height;
}

static bool hblank(const struct karashi_mars *mars, uint64_t cycles) {
	uint64_t mclk = cycles * 7 / 3;
	uint64_t offset = mclk > mars->line_start ? mclk - mars->line_start : 0;
	return offset >= 2640 && offset < 3420;
}

static uint16_t vdp_read(const struct karashi_mars *mars, unsigned offset, uint64_t cycles) {
	uint16_t value;

	switch (offset & 0x0e) {
		case 0x00: return (uint16_t) ((mars->bitmap_mode & 0x00c3) | (mars->pal ? 0 : 0x8000));
		case 0x02: return mars->shift & 1;
		case 0x04: return mars->fill_length & 0xff;
		case 0x06: return mars->fill_address;
		case 0x08: return mars->fill_data;
		case 0x0a:
			value = mars->display & 1;
			if (vblank(mars)) value |= 0x8000;
			if (hblank(mars, cycles)) value |= 0x4000;
			if ((value & 0xc000) || !(mars->bitmap_mode & 3)) value |= 0x2000;
			return value;
		default: return 0;
	}
}

static void bind_frame_reads(struct karashi_mars *mars) {
	unsigned index;

	for (index = 0; index < 2; index++) {
		mars->cpu[index].bus.direct[0x04] = mars->frame[mars->display ^ 1];
		mars->cpu[index].bus.direct_mask[0x04] = KARASHI_MARS_FRAME_SIZE - 1;
	}
}

static void vdp_write(struct karashi_mars *mars, unsigned offset, uint16_t value) {
	uint8_t *frame = mars->frame[mars->display ^ 1];
	unsigned count;

	switch (offset & 0x0e) {
		case 0x00: mars->bitmap_mode = value & 0x00c3; break;
		case 0x02: mars->shift = value & 1; break;
		case 0x04: mars->fill_length = value & 0xff; break;
		case 0x06: mars->fill_address = value; break;
		case 0x08:
			mars->fill_data = value;
			for (count = 0; count <= mars->fill_length; count++) {
				put16(frame, (uint32_t) mars->fill_address * 2 & (KARASHI_MARS_FRAME_SIZE - 2), value);
				mars->fill_address = (uint16_t) ((mars->fill_address & 0xff00) | ((mars->fill_address + 1) & 0xff));
			}
			break;
		case 0x0a:
			mars->select = value & 1;
			if (vblank(mars) || !(mars->bitmap_mode & 3)) {
				mars->display = mars->select;
				mars->swap_pending = false;
				bind_frame_reads(mars);
			} else mars->swap_pending = mars->select != mars->display;
			break;
		default: break;
	}
}

static void palette_write(struct karashi_mars *mars, unsigned index, uint16_t value) {
	mars->palette[index & 0xff] = value;
	mars->colours[index & 0xff] = convert(value);
}

static uint16_t pwm_read(struct karashi_mars *mars, unsigned offset) {
	switch (offset & 0x0e) {
		case 0x00: return mars->pwm_control;
		case 0x02: return mars->pwm_cycle;
		case 0x04: return pwm_status(mars, 0);
		case 0x06: return pwm_status(mars, 1);
		case 0x08: return (uint16_t) (pwm_status(mars, 0) | pwm_status(mars, 1));
		default: return 0;
	}
}

static void pwm_write(struct karashi_mars *mars, unsigned offset, uint16_t value) {
	switch (offset & 0x0e) {
		case 0x00: mars->pwm_control = value & 0x0f8f; break;
		case 0x02: mars->pwm_cycle = value & 0x0fff; break;
		case 0x04: pwm_push(mars, 0, value); break;
		case 0x06: pwm_push(mars, 1, value); break;
		case 0x08:
			pwm_push(mars, 0, value);
			pwm_push(mars, 1, value);
			break;
		default: break;
	}
}

static uint16_t comm_read(const struct karashi_mars *mars, unsigned offset) {
	return mars->comm[(offset >> 1) & 7];
}

static void comm_write(struct karashi_mars *mars, unsigned offset, uint16_t value, uint16_t lanes) {
	uint16_t *word = &mars->comm[(offset >> 1) & 7];
	*word = (uint16_t) ((*word & ~lanes) | (value & lanes));
}

static void sh2_reset(struct karashi_mars *mars) {
	unsigned index;
	uint64_t cycles = mars->clock * 3 / 7;

	for (index = 0; index < 2; index++) {
		karashi_sh2_reset(&mars->cpu[index]);
		mars->cpu[index].cycles = cycles;
		mars->pending[index] = 0;
		mars->interrupt_mask[index] = 0;
		update_irl(mars, index);
	}
}

static uint16_t system_read(struct karashi_mars *mars, unsigned index, unsigned offset) {
	struct karashi_sh2 *cpu = &mars->cpu[index];

	if (offset >= 0x200) return mars->palette[(offset >> 1) & 0xff];
	if (offset >= 0x100) return vdp_read(mars, offset, cpu->cycles);
	if (offset >= 0x30 && offset < 0x3a) {
		pwm_advance(mars, cpu->cycles);
		return pwm_read(mars, offset);
	}
	if (offset >= 0x20 && offset < 0x30) return comm_read(mars, offset);
	switch (offset & 0xfe) {
		case 0x00:
			return (uint16_t) ((mars->interrupt_mask[index] & 0x008f) | (mars->adapter & ADAPTER_FM)
			                   | ((mars->adapter & ADAPTER_ADEN) ? 0x0200 : 0));
		case 0x04: return mars->hcount;
		case 0x06: return dreq_status(mars);
		case 0x08: return (uint16_t) (mars->dreq_source >> 16);
		case 0x0a: return (uint16_t) mars->dreq_source;
		case 0x0c: return (uint16_t) (mars->dreq_destination >> 16);
		case 0x0e: return (uint16_t) mars->dreq_destination;
		case 0x10: return mars->dreq_length;
		case 0x12: return fifo_pop(mars);
		default: return 0;
	}
}

static void system_write(struct karashi_mars *mars, unsigned index, unsigned offset, uint16_t value, uint16_t lanes) {
	struct karashi_sh2 *cpu = &mars->cpu[index];

	if (offset >= 0x200) {
		unsigned entry = (offset >> 1) & 0xff;
		palette_write(mars, entry, (uint16_t) ((mars->palette[entry] & ~lanes) | (value & lanes)));
		return;
	}
	if (offset >= 0x100) {
		vdp_write(mars, offset, (uint16_t) ((vdp_read(mars, offset, cpu->cycles) & ~lanes) | (value & lanes)));
		return;
	}
	if (offset >= 0x30 && offset < 0x3a) {
		pwm_advance(mars, cpu->cycles);
		pwm_write(mars, offset, (uint16_t) ((pwm_read(mars, offset) & ~lanes) | (value & lanes)));
		return;
	}
	if (offset >= 0x20 && offset < 0x30) {
		comm_write(mars, offset, value, lanes);
		return;
	}
	switch (offset & 0xfe) {
		case 0x00:
			if (lanes & 0x00ff) mars->interrupt_mask[index] = (uint16_t) ((mars->interrupt_mask[index] & 0xff00) | (value & 0x8f));
			if (lanes & 0xff00) mars->adapter = (uint16_t) ((mars->adapter & ~ADAPTER_FM) | (value & ADAPTER_FM));
			mars->pending[index] &= (uint8_t) (mars->interrupt_mask[index] | PENDING_CMD);
			update_irl(mars, index);
			break;
		case 0x04: mars->hcount = value & 0xff; break;
		case 0x14: break;
		case 0x16: mars->pending[index] &= (uint8_t) ~PENDING_V; update_irl(mars, index); break;
		case 0x18: mars->pending[index] &= (uint8_t) ~PENDING_H; update_irl(mars, index); break;
		case 0x1a:
			mars->interrupt_control &= (uint16_t) ~(1u << index);
			mars->pending[index] &= (uint8_t) ~PENDING_CMD;
			update_irl(mars, index);
			break;
		case 0x1c: mars->pending[index] &= (uint8_t) ~PENDING_PWM; update_irl(mars, index); break;
		default: break;
	}
}

static uint32_t frame_offset(uint32_t address) {
	return address & (KARASHI_MARS_FRAME_SIZE - 1);
}

static void frame_write8(struct karashi_mars *mars, uint32_t address, uint8_t value) {
	if (!value) return;
	mars->frame[mars->display ^ 1][frame_offset(address)] = value;
}

static uint8_t sh2_read8(void *context, uint32_t address) {
	struct karashi_mars_port *port = context;
	struct karashi_mars *mars = port->mars;

	switch (address >> 24) {
		case 0x00:
			if (address < 0x4000) {
				size_t size = mars->boot_sh2_size[port->index];
				return mars->boot_sh2[port->index] && size ? mars->boot_sh2[port->index][address % size] : 0;
			}
			if (address < 0x4400) {
				uint16_t value = system_read(mars, port->index, address & 0x3fe);
				return (uint8_t) (address & 1 ? value : value >> 8);
			}
			return 0;
		case 0x02:
			address &= 0x3fffff;
			return address < mars->rom_size ? mars->rom[address] : 0xff;
		case 0x04: return mars->frame[mars->display ^ 1][frame_offset(address)];
		case 0x06: return mars->sdram[address & (KARASHI_MARS_SDRAM_SIZE - 1)];
		default: return 0;
	}
}

static uint16_t sh2_read16(void *context, uint32_t address) {
	struct karashi_mars_port *port = context;
	struct karashi_mars *mars = port->mars;

	switch (address >> 24) {
		case 0x00:
			if (address >= 0x4000 && address < 0x4400) return system_read(mars, port->index, address & 0x3fe);
			break;
		case 0x02:
			address &= 0x3ffffe;
			return address + 1 < mars->rom_size ? get16(mars->rom, address) : 0xffff;
		case 0x04: return get16(mars->frame[mars->display ^ 1], frame_offset(address));
		case 0x06: return get16(mars->sdram, address & (KARASHI_MARS_SDRAM_SIZE - 2));
		default: break;
	}
	return (uint16_t) ((uint16_t) sh2_read8(context, address) << 8 | sh2_read8(context, address + 1));
}

static uint32_t sh2_read32(void *context, uint32_t address) {
	struct karashi_mars_port *port = context;
	struct karashi_mars *mars = port->mars;

	if ((address >> 24) == 0x06) {
		const uint8_t *data = mars->sdram + (address & (KARASHI_MARS_SDRAM_SIZE - 4));
		return (uint32_t) data[0] << 24 | (uint32_t) data[1] << 16 | (uint32_t) data[2] << 8 | data[3];
	}
	return (uint32_t) sh2_read16(context, address) << 16 | sh2_read16(context, address + 2);
}

static void sh2_write8(void *context, uint32_t address, uint8_t value) {
	struct karashi_mars_port *port = context;
	struct karashi_mars *mars = port->mars;

	switch (address >> 24) {
		case 0x00:
			if (address >= 0x4000 && address < 0x4400) {
				system_write(mars, port->index, address & 0x3fe, (uint16_t) (value << 8 | value),
				             address & 1 ? 0x00ff : 0xff00);
			}
			break;
		case 0x04: frame_write8(mars, address, value); break;
		case 0x06: mars->sdram[address & (KARASHI_MARS_SDRAM_SIZE - 1)] = value; break;
		default: break;
	}
}

static void sh2_write16(void *context, uint32_t address, uint16_t value) {
	struct karashi_mars_port *port = context;
	struct karashi_mars *mars = port->mars;

	switch (address >> 24) {
		case 0x00:
			if (address >= 0x4000 && address < 0x4400) system_write(mars, port->index, address & 0x3fe, value, 0xffff);
			break;
		case 0x04:
			if (address & 0x20000) {
				frame_write8(mars, address, (uint8_t) (value >> 8));
				frame_write8(mars, address + 1, (uint8_t) value);
			} else put16(mars->frame[mars->display ^ 1], frame_offset(address) & ~1u, value);
			break;
		case 0x06: put16(mars->sdram, address & (KARASHI_MARS_SDRAM_SIZE - 2), value); break;
		default: break;
	}
}

static void sh2_write32(void *context, uint32_t address, uint32_t value) {
	struct karashi_mars_port *port = context;
	struct karashi_mars *mars = port->mars;

	if ((address >> 24) == 0x06) {
		uint8_t *data = mars->sdram + (address & (KARASHI_MARS_SDRAM_SIZE - 4));
		data[0] = (uint8_t) (value >> 24);
		data[1] = (uint8_t) (value >> 16);
		data[2] = (uint8_t) (value >> 8);
		data[3] = (uint8_t) value;
		return;
	}
	sh2_write16(context, address, (uint16_t) (value >> 16));
	sh2_write16(context, address + 2, (uint16_t) value);
}

static uint16_t boot_rom_word(const struct karashi_mars *mars, uint32_t address) {
	uint32_t vector;

	if (mars->boot_68k) return get16(mars->boot_68k, address & 0xfe);
	if (address >= 0x100) return 0xffff;
	vector = address < 4 ? 0 : 0x880200 + ((address >> 2) - 1) * 6;
	return (uint16_t) (address & 2 ? vector : vector >> 16);
}

static uint16_t boot_vector_word(const struct karashi_mars *mars, uint32_t address) {
	if (address == 0x70) return (uint16_t) (mars->hint_vector >> 16);
	if (address == 0x72) return (uint16_t) mars->hint_vector;
	return boot_rom_word(mars, address);
}

static void serial_link(void *context, uint8_t value) {
	struct karashi_mars_port *port = context;
	karashi_sh2_serial_receive(&port->mars->cpu[port->index ^ 1], value);
}

void karashi_mars_init(struct karashi_mars *mars, const uint8_t *rom, size_t rom_size) {
	memset(mars, 0, sizeof(*mars));
	mars->rom = rom;
	mars->rom_size = rom_size;
}

void karashi_mars_set_boot(
	struct karashi_mars *mars, const uint8_t *boot_68k, const uint8_t *master, size_t master_size,
	const uint8_t *slave, size_t slave_size
) {
	mars->boot_68k = boot_68k;
	mars->boot_sh2[0] = master;
	mars->boot_sh2_size[0] = master_size;
	mars->boot_sh2[1] = slave;
	mars->boot_sh2_size[1] = slave_size;
}

void karashi_mars_set_rom_capacity(struct karashi_mars *mars, size_t capacity) {
	unsigned index;

	if (capacity < mars->rom_size || capacity > 0x400000 || (capacity & (capacity - 1))) capacity = 0;
	mars->rom_capacity = capacity;
	for (index = 0; index < 2; index++) {
		mars->cpu[index].bus.direct[0x02] = capacity ? (uint8_t *) (uintptr_t) mars->rom : NULL;
		mars->cpu[index].bus.direct_mask[0x02] = capacity ? (uint32_t) capacity - 1 : 0;
	}
}

void karashi_mars_rebind(struct karashi_mars *mars) {
	unsigned index;

	for (index = 0; index < 2; index++) {
		mars->port[index].mars = mars;
		mars->port[index].index = index;
		mars->cpu[index].bus.context = &mars->port[index];
		mars->cpu[index].bus.boot = mars->boot_sh2_size[index] && !(mars->boot_sh2_size[index] & (mars->boot_sh2_size[index] - 1))
		                                ? mars->boot_sh2[index]
		                                : NULL;
		mars->cpu[index].bus.boot_mask = mars->boot_sh2_size[index] ? (uint32_t) mars->boot_sh2_size[index] - 1 : 0;
		mars->cpu[index].bus.direct[0x02] = mars->rom_capacity ? (uint8_t *) (uintptr_t) mars->rom : NULL;
		mars->cpu[index].bus.direct_mask[0x02] = mars->rom_capacity ? (uint32_t) mars->rom_capacity - 1 : 0;
		mars->cpu[index].bus.direct[0x04] = mars->frame[mars->display ^ 1];
		mars->cpu[index].bus.direct_mask[0x04] = KARASHI_MARS_FRAME_SIZE - 1;
		mars->cpu[index].bus.direct[0x06] = mars->sdram;
		mars->cpu[index].bus.direct_mask[0x06] = KARASHI_MARS_SDRAM_SIZE - 1;
		mars->cpu[index].bus.direct_write[0x06] = true;
	}
}

void karashi_mars_reset(struct karashi_mars *mars, bool pal) {
	unsigned index;

	memset(mars->sdram, 0, sizeof(mars->sdram));
	memset(mars->frame, 0, sizeof(mars->frame));
	memset(mars->palette, 0, sizeof(mars->palette));
	memset(mars->colours, 0, sizeof(mars->colours));
	memset(mars->comm, 0, sizeof(mars->comm));
	memset(mars->pending, 0, sizeof(mars->pending));
	memset(mars->interrupt_mask, 0, sizeof(mars->interrupt_mask));
	memset(mars->pwm_fifo, 0, sizeof(mars->pwm_fifo));
	memset(mars->pwm_width, 0, sizeof(mars->pwm_width));
	memset(mars->pwm_count, 0, sizeof(mars->pwm_count));
	mars->adapter = ADAPTER_REN;
	mars->interrupt_control = mars->bank = mars->dreq_control = mars->dreq_length = mars->sega_tv = 0;
	mars->dreq_source = mars->dreq_destination = 0;
	mars->hint_vector = (uint32_t) boot_rom_word(mars, 0x70) << 16 | boot_rom_word(mars, 0x72);
	mars->hcount = mars->hint_counter = 0;
	mars->bitmap_mode = mars->shift = mars->fill_length = mars->fill_address = mars->fill_data = 0;
	mars->pwm_control = mars->pwm_cycle = 0;
	mars->pwm_timer = mars->fifo_read = mars->fifo_count = mars->display = mars->select = 0;
	mars->swap_pending = mars->in_pwm = false;
	mars->pal = pal;
	mars->line = 0;
	mars->height = 224;
	mars->line_start = mars->clock = mars->pwm_time = 0;
	mars->pwm_frames = 0;
	for (index = 0; index < 2; index++) {
		struct karashi_sh2_bus bus;
		mars->port[index].mars = mars;
		mars->port[index].index = index;
		bus.context = &mars->port[index];
		bus.read = sh2_read8;
		bus.write = sh2_write8;
		bus.read16 = sh2_read16;
		bus.write16 = sh2_write16;
		bus.read32 = sh2_read32;
		bus.write32 = sh2_write32;
		bus.serial = serial_link;
		bus.boot = mars->boot_sh2_size[index] && !(mars->boot_sh2_size[index] & (mars->boot_sh2_size[index] - 1))
		               ? mars->boot_sh2[index]
		               : NULL;
		bus.boot_mask = mars->boot_sh2_size[index] ? (uint32_t) mars->boot_sh2_size[index] - 1 : 0;
		memset(bus.direct, 0, sizeof(bus.direct));
		memset(bus.direct_mask, 0, sizeof(bus.direct_mask));
		memset(bus.direct_write, 0, sizeof(bus.direct_write));
		if (mars->rom_capacity) {
			bus.direct[0x02] = (uint8_t *) (uintptr_t) mars->rom;
			bus.direct_mask[0x02] = (uint32_t) mars->rom_capacity - 1;
		}
		bus.direct[0x04] = mars->frame[mars->display ^ 1];
		bus.direct_mask[0x04] = KARASHI_MARS_FRAME_SIZE - 1;
		bus.direct[0x06] = mars->sdram;
		bus.direct_mask[0x06] = KARASHI_MARS_SDRAM_SIZE - 1;
		bus.direct_write[0x06] = true;
		karashi_sh2_init(&mars->cpu[index], &bus);
	}
}

bool karashi_mars_owns(const struct karashi_mars *mars, uint32_t address) {
	address &= 0xffffff;
	if (address >= 0xa15100 && address < 0xa15400) return true;
	if (address >= 0xa130ec && address < 0xa130f0) return true;
	if (!(mars->adapter & ADAPTER_ADEN)) return false;
	return address < 0x100 || (address >= 0x840000 && address < 0xa00000);
}

static uint16_t register_read(struct karashi_mars *mars, uint32_t address) {
	unsigned offset = address & 0x3fe;
	uint64_t cycles = mars->clock * 3 / 7;

	if (offset >= 0x200) return mars->palette[(offset >> 1) & 0xff];
	if (offset >= 0x180) return vdp_read(mars, offset, cycles);
	if (offset >= 0x130 && offset < 0x13a) {
		pwm_advance(mars, cycles);
		return pwm_read(mars, offset);
	}
	if (offset >= 0x120 && offset < 0x130) return comm_read(mars, offset);
	switch (offset) {
		case 0x100: return mars->adapter;
		case 0x102: return mars->interrupt_control;
		case 0x104: return mars->bank;
		case 0x106: return dreq_status(mars);
		case 0x108: return (uint16_t) (mars->dreq_source >> 16);
		case 0x10a: return (uint16_t) mars->dreq_source;
		case 0x10c: return (uint16_t) (mars->dreq_destination >> 16);
		case 0x10e: return (uint16_t) mars->dreq_destination;
		case 0x110: return mars->dreq_length;
		case 0x11a: return mars->sega_tv;
		default: return 0;
	}
}

static void register_write(struct karashi_mars *mars, uint32_t address, uint16_t value, uint16_t lanes) {
	unsigned offset = address & 0x3fe;
	uint64_t cycles = mars->clock * 3 / 7;
	uint16_t merged;

	if (offset >= 0x200) {
		unsigned entry = (offset >> 1) & 0xff;
		palette_write(mars, entry, (uint16_t) ((mars->palette[entry] & ~lanes) | (value & lanes)));
		return;
	}
	if (offset >= 0x180) {
		vdp_write(mars, offset, (uint16_t) ((vdp_read(mars, offset, cycles) & ~lanes) | (value & lanes)));
		return;
	}
	if (offset >= 0x130 && offset < 0x13a) {
		pwm_advance(mars, cycles);
		pwm_write(mars, offset, (uint16_t) ((pwm_read(mars, offset) & ~lanes) | (value & lanes)));
		return;
	}
	if (offset >= 0x120 && offset < 0x130) {
		comm_write(mars, offset, value, lanes);
		return;
	}
	merged = (uint16_t) ((register_read(mars, address) & ~lanes) | (value & lanes));
	switch (offset) {
		case 0x100: {
			bool was_running = running(mars);
			mars->adapter = (uint16_t) ((merged & (ADAPTER_ADEN | ADAPTER_RES | ADAPTER_FM)) | ADAPTER_REN);
			if (running(mars) && !was_running) sh2_reset(mars);
			break;
		}
		case 0x102:
			mars->interrupt_control = merged & 3;
			update_command(mars);
			break;
		case 0x104: mars->bank = merged & 3; break;
		case 0x106:
			if ((merged ^ mars->dreq_control) & DREQ_68S) mars->fifo_count = mars->fifo_read = 0;
			mars->dreq_control = merged & 7;
			break;
		case 0x108: mars->dreq_source = (mars->dreq_source & 0xffff) | (uint32_t) (merged & 0xff) << 16; break;
		case 0x10a: mars->dreq_source = (mars->dreq_source & 0xff0000) | (merged & 0xfffe); break;
		case 0x10c: mars->dreq_destination = (mars->dreq_destination & 0xffff) | (uint32_t) (merged & 0xff) << 16; break;
		case 0x10e: mars->dreq_destination = (mars->dreq_destination & 0xff0000) | merged; break;
		case 0x110: mars->dreq_length = merged & 0xfffc; break;
		case 0x112: fifo_push(mars, value); break;
		case 0x11a: mars->sega_tv = merged & 1; break;
		default: break;
	}
}

static void note_write(struct karashi_mars *mars) {
	mars->writes++;
}

uint8_t karashi_mars_read8(struct karashi_mars *mars, uint32_t address) {
	uint16_t value = karashi_mars_read16(mars, address & ~1u);
	return (uint8_t) (address & 1 ? value : value >> 8);
}

uint16_t karashi_mars_read16(struct karashi_mars *mars, uint32_t address) {
	address &= 0xfffffe;
	if (address < 0x100) return boot_vector_word(mars, address);
	if (address >= 0xa130ec && address < 0xa130f0) return address == 0xa130ec ? 0x4d41 : 0x5253;
	if (address >= 0xa15100) return register_read(mars, address);
	if (address < 0x880000) return get16(mars->frame[mars->display ^ 1], frame_offset(address));
	if (address < 0x900000) address -= 0x880000;
	else address = (uint32_t) (mars->bank & 3) * 0x100000 + (address - 0x900000);
	return address + 1 < mars->rom_size ? get16(mars->rom, address) : 0xffff;
}

static void write_hint_vector(struct karashi_mars *mars, uint32_t address, uint8_t value) {
	unsigned shift = (3 - (address & 3)) * 8;
	mars->hint_vector = (mars->hint_vector & ~(UINT32_C(0xff) << shift)) | (uint32_t) value << shift;
}

void karashi_mars_write8(struct karashi_mars *mars, uint32_t address, uint8_t value) {
	address &= 0xffffff;
	note_write(mars);
	if (address >= 0x70 && address < 0x74) {
		write_hint_vector(mars, address, value);
		return;
	}
	if (address >= 0xa15100 && address < 0xa15400) {
		register_write(mars, address, (uint16_t) (value << 8 | value), address & 1 ? 0x00ff : 0xff00);
	} else if (address >= 0x840000 && address < 0x880000) {
		frame_write8(mars, address, value);
	}
}

void karashi_mars_write16(struct karashi_mars *mars, uint32_t address, uint16_t value) {
	address &= 0xfffffe;
	note_write(mars);
	if (address == 0x70 || address == 0x72) {
		write_hint_vector(mars, address, (uint8_t) (value >> 8));
		write_hint_vector(mars, address + 1, (uint8_t) value);
		return;
	}
	if (address >= 0xa15100 && address < 0xa15400) register_write(mars, address, value, 0xffff);
	else if (address >= 0x860000 && address < 0x880000) {
		frame_write8(mars, address, (uint8_t) (value >> 8));
		frame_write8(mars, address + 1, (uint8_t) value);
	} else if (address >= 0x840000 && address < 0x860000) put16(mars->frame[mars->display ^ 1], frame_offset(address), value);
}

static bool woken(struct karashi_mars *mars, unsigned index) {
	struct karashi_mars_port *port = &mars->port[index];

	return mars->cpu[index ^ 1].stores != port->wake_stores || mars->writes != port->wake_writes
	       || karashi_sh2_interrupt_ready(&mars->cpu[index]);
}

void karashi_mars_run(struct karashi_mars *mars, uint64_t target) {
	uint64_t goal;
	unsigned index;

	if (target <= mars->clock) return;
	goal = target * 3 / 7;
	mars->clock = target;
	if (!running(mars)) {
		mars->cpu[0].cycles = mars->cpu[1].cycles = goal;
		pwm_advance(mars, goal);
		return;
	}
	service_fifo(mars);
	for (index = 0; index < 2; index++) {
		mars->cpu[index].budget = goal;
		mars->cpu[index].polling = false;
		mars->port[index].sleeping = false;
	}
	for (;;) {
		uint64_t lowest = mars->cpu[0].cycles < mars->cpu[1].cycles ? mars->cpu[0].cycles : mars->cpu[1].cycles;
		uint64_t quantum;

		if (lowest >= goal) break;
		quantum = lowest + QUANTUM < goal ? lowest + QUANTUM : goal;
		for (index = 0; index < 2; index++) {
			struct karashi_sh2 *cpu = &mars->cpu[index];
			struct karashi_mars_port *port = &mars->port[index];
			while (cpu->cycles < quantum) {
				if (port->sleeping) {
					if (!woken(mars, index)) {
						karashi_sh2_wait(cpu, quantum);
						break;
					}
					port->sleeping = false;
					cpu->polling = false;
				}
				karashi_sh2_run(cpu, quantum);
				if (cpu->fault != KARASHI_SH2_FAULT_NONE) {
					cpu->cycles = quantum;
					break;
				}
				if (cpu->idle) karashi_sh2_skip_idle(cpu, quantum);
				if (cpu->polling && !port->sleeping) {
					port->sleeping = true;
					port->wake_stores = mars->cpu[index ^ 1].stores;
					port->wake_writes = mars->writes;
				}
			}
		}
		pwm_advance(mars, quantum);
	}
}

void karashi_mars_begin_line(struct karashi_mars *mars, unsigned line, unsigned height, uint64_t line_start) {
	mars->line = (uint16_t) line;
	mars->height = (uint16_t) height;
	mars->line_start = line_start;
	if (line == height) {
		if (mars->swap_pending) {
			mars->display = mars->select;
			mars->swap_pending = false;
			bind_frame_reads(mars);
		}
		raise(mars, PENDING_V);
	}
}

void karashi_mars_end_frame(struct karashi_mars *mars) {
	unsigned index;

	for (index = 0; index < 2; index++) {
		karashi_sh2_forget_loops(&mars->cpu[index]);
		mars->port[index].sleeping = false;
	}
}

void karashi_mars_end_active(struct karashi_mars *mars) {
	if (mars->line < mars->height || ((mars->interrupt_mask[0] | mars->interrupt_mask[1]) & MASK_HEN)) {
		if (!mars->hint_counter) {
			mars->hint_counter = mars->hcount;
			raise(mars, PENDING_H);
		} else mars->hint_counter--;
	} else mars->hint_counter = mars->hcount;
}

unsigned karashi_mars_height(const struct karashi_mars *mars) {
	return mars->pal && (mars->bitmap_mode & MODE_240) ? 240 : 224;
}

void karashi_mars_render_line(
	const struct karashi_mars *mars, unsigned line, karashi_pixel *output, const uint8_t *backdrop
) {
	const uint8_t *frame = mars->frame[mars->display];
	unsigned mode = mars->bitmap_mode & 3;
	bool priority = (mars->bitmap_mode & MODE_PRIORITY) != 0;
	uint16_t start;
	uint16_t pixels[KARASHI_MARS_WIDTH];
	unsigned x;

	if (!mode || line >= 256) return;
	start = get16(frame, line * 2);
	if (mode == 1) {
		uint32_t base = (uint32_t) start * 2 + (mars->shift & 1);
		for (x = 0; x < KARASHI_MARS_WIDTH; x++) pixels[x] = mars->palette[frame[(base + x) & (KARASHI_MARS_FRAME_SIZE - 1)]];
	} else if (mode == 2) {
		for (x = 0; x < KARASHI_MARS_WIDTH; x++) pixels[x] = get16(frame, (uint32_t) ((start + x) & 0xffff) * 2);
	} else {
		uint16_t address = start;
		x = 0;
		while (x < KARASHI_MARS_WIDTH) {
			uint16_t run = get16(frame, (uint32_t) address * 2);
			unsigned length = (run >> 8) + 1u;
			uint16_t colour = mars->palette[run & 0xff];
			address++;
			while (length-- && x < KARASHI_MARS_WIDTH) pixels[x++] = colour;
		}
	}
	for (x = 0; x < KARASHI_MARS_WIDTH; x++) {
		uint16_t colour = pixels[x];
		bool front = priority != ((colour & 0x8000) != 0);
		if (front || !backdrop || backdrop[x]) output[x] = convert(colour);
	}
}

void karashi_mars_mix_audio(struct karashi_mars *mars, int16_t *audio, unsigned frames) {
	unsigned available = mars->pwm_frames;
	unsigned index;

	if (available) {
		for (index = 0; index < frames; index++) {
			unsigned source = (unsigned) ((uint64_t) index * available / frames);
			unsigned channel;
			for (channel = 0; channel < 2; channel++) {
				int32_t value = audio[index * 2 + channel] + mars->pwm_audio[source * 2 + channel];
				audio[index * 2 + channel] = (int16_t) (value > 32767 ? 32767 : value < -32768 ? -32768 : value);
			}
		}
	}
	mars->pwm_frames = 0;
}

bool karashi_mars_save(const struct karashi_mars *mars, struct karashi_state_writer *writer) {
	unsigned index;

	karashi_sh2_save(&mars->cpu[0], writer);
	karashi_sh2_save(&mars->cpu[1], writer);
	karashi_state_write_bytes(writer, mars->sdram, sizeof(mars->sdram));
	karashi_state_write_bytes(writer, mars->frame, sizeof(mars->frame));
	for (index = 0; index < 256; index++) karashi_state_write_u16(writer, mars->palette[index]);
	for (index = 0; index < 8; index++) karashi_state_write_u16(writer, mars->comm[index]);
	for (index = 0; index < KARASHI_MARS_FIFO_SIZE; index++) karashi_state_write_u16(writer, mars->fifo[index]);
	karashi_state_write_u16(writer, mars->adapter);
	karashi_state_write_u16(writer, mars->interrupt_control);
	karashi_state_write_u16(writer, mars->bank);
	karashi_state_write_u16(writer, mars->dreq_control);
	karashi_state_write_u32(writer, mars->dreq_source);
	karashi_state_write_u32(writer, mars->dreq_destination);
	karashi_state_write_u16(writer, mars->dreq_length);
	karashi_state_write_u16(writer, mars->sega_tv);
	karashi_state_write_u32(writer, mars->hint_vector);
	karashi_state_write_u16(writer, mars->interrupt_mask[0]);
	karashi_state_write_u16(writer, mars->interrupt_mask[1]);
	karashi_state_write_bytes(writer, mars->pending, sizeof(mars->pending));
	karashi_state_write_u16(writer, mars->hcount);
	karashi_state_write_u16(writer, mars->hint_counter);
	karashi_state_write_u16(writer, mars->bitmap_mode);
	karashi_state_write_u16(writer, mars->shift);
	karashi_state_write_u16(writer, mars->fill_length);
	karashi_state_write_u16(writer, mars->fill_address);
	karashi_state_write_u16(writer, mars->fill_data);
	karashi_state_write_u16(writer, mars->pwm_control);
	karashi_state_write_u16(writer, mars->pwm_cycle);
	for (index = 0; index < 6; index++) karashi_state_write_u16(writer, mars->pwm_fifo[index / 3][index % 3]);
	karashi_state_write_u16(writer, mars->pwm_width[0]);
	karashi_state_write_u16(writer, mars->pwm_width[1]);
	karashi_state_write_bytes(writer, mars->pwm_count, sizeof(mars->pwm_count));
	karashi_state_write_u8(writer, mars->pwm_timer);
	karashi_state_write_u8(writer, mars->fifo_read);
	karashi_state_write_u8(writer, mars->fifo_count);
	karashi_state_write_u8(writer, mars->display);
	karashi_state_write_u8(writer, mars->select);
	karashi_state_write_bool(writer, mars->swap_pending);
	karashi_state_write_u16(writer, mars->line);
	karashi_state_write_u16(writer, mars->height);
	karashi_state_write_u64(writer, mars->line_start);
	karashi_state_write_u64(writer, mars->clock);
	return karashi_state_write_u64(writer, mars->pwm_time);
}

bool karashi_mars_load(struct karashi_mars *mars, struct karashi_state_reader *reader) {
	bool loaded = karashi_sh2_load(&mars->cpu[0], reader) && karashi_sh2_load(&mars->cpu[1], reader);
	unsigned index;

	loaded = karashi_state_read_bytes(reader, mars->sdram, sizeof(mars->sdram)) && loaded;
	loaded = karashi_state_read_bytes(reader, mars->frame, sizeof(mars->frame)) && loaded;
	for (index = 0; index < 256; index++) loaded = karashi_state_read_u16(reader, &mars->palette[index]) && loaded;
	for (index = 0; index < 8; index++) loaded = karashi_state_read_u16(reader, &mars->comm[index]) && loaded;
	for (index = 0; index < KARASHI_MARS_FIFO_SIZE; index++) loaded = karashi_state_read_u16(reader, &mars->fifo[index]) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->adapter) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->interrupt_control) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->bank) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->dreq_control) && loaded;
	loaded = karashi_state_read_u32(reader, &mars->dreq_source) && loaded;
	loaded = karashi_state_read_u32(reader, &mars->dreq_destination) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->dreq_length) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->sega_tv) && loaded;
	loaded = karashi_state_read_u32(reader, &mars->hint_vector) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->interrupt_mask[0]) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->interrupt_mask[1]) && loaded;
	loaded = karashi_state_read_bytes(reader, mars->pending, sizeof(mars->pending)) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->hcount) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->hint_counter) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->bitmap_mode) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->shift) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->fill_length) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->fill_address) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->fill_data) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->pwm_control) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->pwm_cycle) && loaded;
	for (index = 0; index < 6; index++) loaded = karashi_state_read_u16(reader, &mars->pwm_fifo[index / 3][index % 3]) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->pwm_width[0]) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->pwm_width[1]) && loaded;
	loaded = karashi_state_read_bytes(reader, mars->pwm_count, sizeof(mars->pwm_count)) && loaded;
	loaded = karashi_state_read_u8(reader, &mars->pwm_timer) && loaded;
	loaded = karashi_state_read_u8(reader, &mars->fifo_read) && loaded;
	loaded = karashi_state_read_u8(reader, &mars->fifo_count) && loaded;
	loaded = karashi_state_read_u8(reader, &mars->display) && loaded;
	loaded = karashi_state_read_u8(reader, &mars->select) && loaded;
	loaded = karashi_state_read_bool(reader, &mars->swap_pending) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->line) && loaded;
	loaded = karashi_state_read_u16(reader, &mars->height) && loaded;
	loaded = karashi_state_read_u64(reader, &mars->line_start) && loaded;
	loaded = karashi_state_read_u64(reader, &mars->clock) && loaded;
	loaded = karashi_state_read_u64(reader, &mars->pwm_time) && loaded;
	if (!loaded) return false;
	mars->fifo_read %= KARASHI_MARS_FIFO_SIZE;
	if (mars->fifo_count > KARASHI_MARS_FIFO_SIZE) mars->fifo_count = KARASHI_MARS_FIFO_SIZE;
	for (index = 0; index < 2; index++) if (mars->pwm_count[index] > 3) mars->pwm_count[index] = 3;
	mars->display &= 1;
	mars->select &= 1;
	mars->pwm_frames = 0;
	mars->in_pwm = false;
	for (index = 0; index < 256; index++) mars->colours[index] = convert(mars->palette[index]);
	for (index = 0; index < 2; index++) update_irl(mars, index);
	bind_frame_reads(mars);
	return true;
}
