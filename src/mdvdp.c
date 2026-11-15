#include "mdvdp.h"

#include <string.h>

enum {
	STATUS_PAL = 0x0001,
	STATUS_DMA = 0x0002,
	STATUS_HBLANK = 0x0004,
	STATUS_VBLANK = 0x0008,
	STATUS_ODD = 0x0010,
	STATUS_COLLISION = 0x0020,
	STATUS_OVERFLOW = 0x0040,
	STATUS_VINT = 0x0080,
	STATUS_FIFO_FULL = 0x0100,
	STATUS_FIFO_EMPTY = 0x0200,
	PIXEL_PRIORITY = 0x40,
	LINE_MARGIN = 16
};

static uint16_t nibbles[256];
static uint16_t nibbles_flipped[256];
static bool nibbles_ready;

static void build_nibbles(void) {
	unsigned index;

	for (index = 0; index < 256; index++) {
		nibbles[index] = (uint16_t) ((index >> 4) | (index & 15) << 8);
		nibbles_flipped[index] = (uint16_t) ((index & 15) | (index >> 4) << 8);
	}
	nibbles_ready = true;
}

static bool h40(const struct karashi_mdvdp *vdp) {
	return (vdp->registers[12] & 0x01) != 0;
}

static const uint16_t active_slots_h32[] = {
	50, 210, 370, 690, 850, 1010, 1330, 1490, 1650, 1970, 2130, 2290, 2590, 2610, 2890, 3170
};

static const uint16_t active_slots_h40[] = {
	28, 156, 284, 540, 668, 796, 1052, 1180, 1308, 1564, 1692, 1820, 2076, 2204, 2332, 2572, 2588, 2980
};

static bool interlace_double(const struct karashi_mdvdp *vdp) {
	return (vdp->registers[12] & 0x06) == 0x06;
}

unsigned karashi_mdvdp_width(const struct karashi_mdvdp *vdp) {
	return h40(vdp) ? 320 : 256;
}

unsigned karashi_mdvdp_height(const struct karashi_mdvdp *vdp) {
	return vdp->pal && (vdp->registers[1] & 0x08) ? 240 : 224;
}

static void update_colour(struct karashi_mdvdp *vdp, unsigned index) {
	uint16_t value = vdp->cram[index];
	unsigned red = (value >> 1) & 7;
	unsigned green = (value >> 5) & 7;
	unsigned blue = (value >> 9) & 7;

	vdp->palette[index] = KARASHI_PIXEL(red * 255 / 7, green * 255 / 7, blue * 255 / 7);
	vdp->palette[64 + index] = KARASHI_PIXEL(red * 255 / 14, green * 255 / 14, blue * 255 / 14);
	vdp->palette[128 + index] = KARASHI_PIXEL(
		(red + 7) * 255 / 14, (green + 7) * 255 / 14, (blue + 7) * 255 / 14
	);
}

void karashi_mdvdp_set_pal(struct karashi_mdvdp *vdp, bool pal) {
	vdp->pal = pal;
	vdp->lines = pal ? 313 : 262;
}

void karashi_mdvdp_reset(struct karashi_mdvdp *vdp) {
	karashi_mdvdp_dma_read_t dma_read = vdp->dma_read;
	void *dma_context = vdp->dma_context;
	bool pal = vdp->pal;
	unsigned index;

	if (!nibbles_ready) build_nibbles();
	memset(vdp, 0, sizeof(*vdp));
	vdp->dma_read = dma_read;
	vdp->dma_context = dma_context;
	karashi_mdvdp_set_pal(vdp, pal);
	for (index = 0; index < KARASHI_MD_CRAM_SIZE; index++) update_colour(vdp, index);
}

static unsigned increment(const struct karashi_mdvdp *vdp) {
	return vdp->registers[15];
}

static void store(struct karashi_mdvdp *vdp, uint16_t value) {
	unsigned index;

	switch (vdp->code & 0x0f) {
		case 1:
			vdp->vram[vdp->address & 0xffff] = (uint8_t) (value >> 8);
			vdp->vram[(vdp->address ^ 1) & 0xffff] = (uint8_t) value;
			break;
		case 3:
			index = (vdp->address >> 1) & 0x3f;
			vdp->cram[index] = value & 0x0eee;
			update_colour(vdp, index);
			break;
		case 5:
			index = (vdp->address >> 1) & 0x3f;
			if (index < KARASHI_MD_VSRAM_SIZE) vdp->vsram[index] = value & 0x07ff;
			break;
		default: break;
	}
	vdp->address = (vdp->address + increment(vdp)) & 0xffff;
}

static unsigned dma_length(const struct karashi_mdvdp *vdp) {
	unsigned length = vdp->registers[19] | (unsigned) vdp->registers[20] << 8;
	return length ? length : 0x10000;
}

static bool line_active(const struct karashi_mdvdp *vdp, unsigned line) {
	return (vdp->registers[1] & 0x40) && line < karashi_mdvdp_height(vdp);
}

static unsigned line_capacity(const struct karashi_mdvdp *vdp, unsigned line, unsigned active, unsigned blank) {
	return line_active(vdp, line) ? active : blank;
}

static unsigned transfer_time(
	const struct karashi_mdvdp *vdp, unsigned words, unsigned slots_per_word, unsigned active, unsigned blank,
	unsigned line_mclk
) {
	unsigned line = vdp->line;
	unsigned cycles = 0;
	unsigned credit = 0;

	while (words) {
		unsigned capacity = line_capacity(vdp, line, active, blank);
		unsigned duration = KARASHI_MD_LINE_MCLK;
		unsigned available;
		unsigned transferable;

		if (line_mclk) {
			duration -= line_mclk;
			capacity = capacity * duration / KARASHI_MD_LINE_MCLK;
			line_mclk = 0;
		}
		available = capacity + credit;
		transferable = available / slots_per_word;
		if (words > transferable) {
			cycles += (duration + 6) / 7;
			words -= transferable;
			credit = available % slots_per_word;
			line = (line + 1) % vdp->lines;
		} else {
			unsigned needed = words * slots_per_word - credit;
			unsigned master_clocks = (needed * duration + capacity - 1) / capacity;
			cycles += (master_clocks + 6) / 7;
			words = 0;
		}
	}
	return cycles;
}

static unsigned dma_transfer(
	struct karashi_mdvdp *vdp, unsigned line_mclk, uint64_t master_clock
) {
	unsigned length = dma_length(vdp);
	uint32_t source = ((uint32_t) vdp->registers[21] | (uint32_t) vdp->registers[22] << 8
	                   | (uint32_t) (vdp->registers[23] & 0x7f) << 16) << 1;
	unsigned slots_per_word = (vdp->code & 0x0f) == 1 ? 2 : 1;
	unsigned cycles = transfer_time(
		vdp, length, slots_per_word, h40(vdp) ? 18 : 16, h40(vdp) ? 204 : 166, line_mclk
	);

	while (length--) {
		store(vdp, vdp->dma_read ? vdp->dma_read(vdp->dma_context, source) : 0xffff);
		source = (source & 0xfe0000) | ((source + 2) & 0x1ffff);
	}
	vdp->registers[19] = 0;
	vdp->registers[20] = 0;
	vdp->registers[21] = (uint8_t) (source >> 1);
	vdp->registers[22] = (uint8_t) (source >> 9);
	vdp->dma_finish = master_clock + (uint64_t) cycles * 7;
	return cycles;
}

static void dma_copy(struct karashi_mdvdp *vdp, unsigned line_mclk, uint64_t master_clock) {
	unsigned length = dma_length(vdp);
	unsigned cycles = transfer_time(
		vdp, length, 2, h40(vdp) ? 18 : 16, h40(vdp) ? 204 : 166, line_mclk
	);
	unsigned source = vdp->registers[21] | (unsigned) vdp->registers[22] << 8;

	while (length--) {
		vdp->vram[(vdp->address ^ 1) & 0xffff] = vdp->vram[(source ^ 1) & 0xffff];
		source = (source + 1) & 0xffff;
		vdp->address = (vdp->address + increment(vdp)) & 0xffff;
	}
	vdp->registers[21] = (uint8_t) source;
	vdp->registers[22] = (uint8_t) (source >> 8);
	vdp->registers[19] = vdp->registers[20] = 0;
	vdp->dma_finish = master_clock + (uint64_t) cycles * 7;
}

static void dma_fill(
	struct karashi_mdvdp *vdp, uint16_t value, unsigned line_mclk, uint64_t master_clock
) {
	unsigned length = dma_length(vdp);
	unsigned cycles = transfer_time(
		vdp, length, 1, h40(vdp) ? 18 : 16, h40(vdp) ? 204 : 166, line_mclk
	);

	while (length--) {
		if ((vdp->code & 0x0f) == 1) vdp->vram[(vdp->address ^ 1) & 0xffff] = (uint8_t) (value >> 8);
		else {
			unsigned saved = vdp->address;
			store(vdp, value);
			vdp->address = saved;
		}
		vdp->address = (vdp->address + increment(vdp)) & 0xffff;
	}
	vdp->registers[19] = vdp->registers[20] = 0;
	vdp->dma_finish = master_clock + (uint64_t) cycles * 7;
}

unsigned karashi_mdvdp_write_control_at(
	struct karashi_mdvdp *vdp, uint16_t value, unsigned line_mclk, uint64_t master_clock
) {
	unsigned cycles = 0;

	if (!vdp->control_pending) {
		if ((value & 0xc000) == 0x8000) {
			unsigned reg = (value >> 8) & 0x1f;
			if (reg < 24 && ((vdp->registers[1] & 0x04) || reg <= 10)) {
				if (reg == 0 && (value & 0x02) && !(vdp->registers[0] & 0x02)) {
					vdp->hv_latch = karashi_mdvdp_hv_counter(vdp);
					vdp->hv_latched = true;
				}
				vdp->registers[reg] = (uint8_t) value;
				if (reg == 0 && !(value & 0x02)) vdp->hv_latched = false;
			}
			vdp->code = 0;
			return 0;
		}
		vdp->code = (uint8_t) ((vdp->code & 0x3c) | (value >> 14));
		vdp->address = (vdp->address & 0xc000) | (value & 0x3fff);
		vdp->control_pending = true;
		return 0;
	}
	vdp->control_pending = false;
	vdp->code = (uint8_t) ((vdp->code & 0x03) | ((value >> 2) & 0x3c));
	vdp->address = (vdp->address & 0x3fff) | ((uint32_t) (value & 3) << 14);
	if ((vdp->code & 0x20) && (vdp->registers[1] & 0x10)) {
		switch (vdp->registers[23] >> 6) {
			case 2: vdp->fill_pending = true; break;
			case 3:
				dma_copy(vdp, line_mclk, master_clock);
				vdp->code &= 0x1f;
				break;
			default:
				cycles = dma_transfer(vdp, line_mclk, master_clock);
				vdp->code &= 0x1f;
				break;
		}
	}
	return cycles;
}

unsigned karashi_mdvdp_write_control(struct karashi_mdvdp *vdp, uint16_t value) {
	return karashi_mdvdp_write_control_at(vdp, value, 0, 0);
}

void karashi_mdvdp_write_data(struct karashi_mdvdp *vdp, uint16_t value) {
	vdp->control_pending = false;
	vdp->last_data = value;
	store(vdp, value);
	if (vdp->fill_pending) {
		vdp->fill_pending = false;
		dma_fill(vdp, value, 0, 0);
		vdp->code &= 0x1f;
	}
}

static void fifo_discard(struct karashi_mdvdp *vdp, uint64_t master_clock) {
	while (vdp->fifo_count && vdp->fifo_finish[vdp->fifo_head] <= master_clock) {
		vdp->fifo_head = (uint8_t) ((vdp->fifo_head + 1) & 3);
		vdp->fifo_count--;
	}
}

static uint64_t fifo_finish_after(
	const struct karashi_mdvdp *vdp, uint64_t master_clock, unsigned line_mclk,
	uint64_t start, unsigned slots
) {
	uint64_t offset = line_mclk + start - master_clock;
	unsigned line = (unsigned) ((vdp->line + offset / KARASHI_MD_LINE_MCLK) % vdp->lines);
	unsigned position = (unsigned) (offset % KARASHI_MD_LINE_MCLK);
	uint64_t line_start = start - position;

	while (slots) {
		unsigned capacity = line_capacity(vdp, line, h40(vdp) ? 18 : 16, h40(vdp) ? 204 : 166);
		const uint16_t *active_slots = h40(vdp) ? active_slots_h40 : active_slots_h32;
		bool active = line_active(vdp, line);
		unsigned elapsed = 0;
		unsigned available = capacity - elapsed;

		if (active) {
			while (elapsed < capacity && active_slots[elapsed] <= position) elapsed++;
		} else elapsed = position * capacity / KARASHI_MD_LINE_MCLK;
		available = capacity - elapsed;

		if (slots <= available) {
			unsigned target = elapsed + slots - 1;
			unsigned target_position = active
			                             ? active_slots[target]
			                             : ((target + 1) * KARASHI_MD_LINE_MCLK + capacity - 1) / capacity;
			return line_start + target_position;
		}
		slots -= available;
		line_start += KARASHI_MD_LINE_MCLK;
		line = (line + 1) % vdp->lines;
		position = 0;
	}
	return start;
}

unsigned karashi_mdvdp_write_data_at(
	struct karashi_mdvdp *vdp, uint16_t value, unsigned line_mclk, uint64_t master_clock
) {
	uint64_t access_clock = master_clock + 28;
	uint64_t effective_clock;
	uint64_t previous_finish;
	unsigned cycles = 0;
	unsigned slots;
	unsigned tail;

	if (line_mclk >= KARASHI_MD_LINE_MCLK) line_mclk = KARASHI_MD_LINE_MCLK - 1;
	if (access_clock <= vdp->port_clock) access_clock = vdp->port_clock + 28;
	effective_clock = access_clock;
	fifo_discard(vdp, access_clock);
	if (line_active(vdp, vdp->line)) {
		if (vdp->fifo_count == 4) {
			uint64_t delay = vdp->fifo_finish[vdp->fifo_head] - access_clock;
			cycles = (unsigned) ((delay + 6) / 7);
			effective_clock += (uint64_t) cycles * 7;
			fifo_discard(vdp, effective_clock);
		}
		previous_finish = vdp->fifo_count
		                    ? vdp->fifo_finish[(vdp->fifo_head + vdp->fifo_count - 1) & 3]
		                    : effective_clock;
		slots = (vdp->code & 0x06) ? 1 : 2;
		tail = (vdp->fifo_head + vdp->fifo_count) & 3;
		vdp->fifo_finish[tail] = fifo_finish_after(
			vdp, master_clock, line_mclk, previous_finish, slots
		);
		vdp->fifo_count++;
	}
	vdp->port_clock = effective_clock;

	vdp->control_pending = false;
	vdp->last_data = value;
	store(vdp, value);
	if (vdp->fill_pending) {
		vdp->fill_pending = false;
		dma_fill(vdp, value, line_mclk, effective_clock);
		vdp->code &= 0x1f;
	}
	return cycles;
}

uint16_t karashi_mdvdp_read_data(struct karashi_mdvdp *vdp) {
	uint16_t value;
	unsigned index = (vdp->address >> 1) & 0x3f;

	vdp->control_pending = false;
	switch (vdp->code & 0x0f) {
		case 0:
			value = (uint16_t) ((uint16_t) vdp->vram[vdp->address & 0xfffe] << 8 | vdp->vram[vdp->address | 1]);
			break;
		case 4: value = index < KARASHI_MD_VSRAM_SIZE ? vdp->vsram[index] : vdp->vsram[0]; break;
		case 8: value = vdp->cram[index]; break;
		default: value = vdp->last_data; break;
	}
	vdp->address = (vdp->address + increment(vdp)) & 0xffff;
	return value;
}

uint16_t karashi_mdvdp_read_status_at(
	struct karashi_mdvdp *vdp, unsigned line_mclk, uint64_t master_clock
) {
	uint16_t value = (uint16_t) (0x3400 | vdp->status);
	bool blank_edge = h40(vdp) ? line_mclk >= 2850 || line_mclk < 80 : line_mclk >= 2940 || line_mclk < 100;

	fifo_discard(vdp, master_clock);
	if (vdp->pal) value |= STATUS_PAL;
	if (master_clock < vdp->dma_finish) value |= STATUS_DMA;
	if (vdp->fifo_count == 4) value |= STATUS_FIFO_FULL;
	else if (!vdp->fifo_count) value |= STATUS_FIFO_EMPTY;
	if (vdp->vint_flag) value |= STATUS_VINT;
	vdp->vint_flag = false;
	if (blank_edge) value |= STATUS_HBLANK;
	if (!(vdp->registers[1] & 0x40)) value |= STATUS_VBLANK;
	if (vdp->odd_frame) value |= STATUS_ODD;
	vdp->status &= (uint16_t) ~(STATUS_COLLISION | STATUS_OVERFLOW);
	vdp->control_pending = false;
	return value;
}

uint16_t karashi_mdvdp_read_status(struct karashi_mdvdp *vdp, unsigned line_mclk) {
	return karashi_mdvdp_read_status_at(vdp, line_mclk, 0);
}

uint16_t karashi_mdvdp_read_control(struct karashi_mdvdp *vdp) {
	return karashi_mdvdp_read_status_at(vdp, 1000, 0);
}

uint16_t karashi_mdvdp_hv_counter_at(const struct karashi_mdvdp *vdp, unsigned line_mclk) {
	unsigned height = karashi_mdvdp_height(vdp);
	unsigned v = vdp->line + (line_mclk >= KARASHI_MD_HBLANK_MCLK ? 1 : 0);
	unsigned h;

	if (vdp->hv_latched) return vdp->hv_latch;
	if (v >= vdp->lines) v -= vdp->lines;
	if (!vdp->pal) {
		if (v > 0xea) v -= 6;
	} else if (height == 240) {
		if (v > 0x10a) v -= 0x39;
	} else if (v > 0x102) v -= 0x39;
	if (interlace_double(vdp)) v = (v << 1 & 0xfe) | (v >> 7 & 1);
	if (h40(vdp)) {
		h = line_mclk / 16;
		if (h > 0xb6) h += 0xe4 - 0xb7;
	} else {
		h = line_mclk / 20;
		if (h > 0x93) h += 0xe9 - 0x94;
	}
	return (uint16_t) ((v & 0xff) << 8 | (h & 0xff));
}

uint16_t karashi_mdvdp_hv_counter(const struct karashi_mdvdp *vdp) {
	return karashi_mdvdp_hv_counter_at(vdp, 0);
}

unsigned karashi_mdvdp_irq_level(const struct karashi_mdvdp *vdp) {
	if (vdp->vint_pending && (vdp->registers[1] & 0x20)) return 6;
	if (vdp->hint_pending && (vdp->registers[0] & 0x10)) return 4;
	return 0;
}

void karashi_mdvdp_raise_vint(struct karashi_mdvdp *vdp) {
	vdp->vint_pending = true;
}

void karashi_mdvdp_acknowledge(struct karashi_mdvdp *vdp, unsigned level) {
	if (level >= 6) vdp->vint_pending = false;
	else vdp->hint_pending = false;
}

unsigned karashi_mdvdp_begin_line(struct karashi_mdvdp *vdp, unsigned line) {
	unsigned height = karashi_mdvdp_height(vdp);

	vdp->line = (uint16_t) line;
	if (line == 0) {
		vdp->status &= (uint16_t) ~STATUS_VBLANK;
		if (vdp->registers[12] & 0x02) vdp->odd_frame = !vdp->odd_frame;
		else vdp->odd_frame = false;
	}
	if (line == height) {
		vdp->status |= STATUS_VBLANK;
		vdp->vint_flag = true;
	}
	return karashi_mdvdp_irq_level(vdp);
}

void karashi_mdvdp_end_active(struct karashi_mdvdp *vdp) {
	unsigned height = karashi_mdvdp_height(vdp);

	if (vdp->line < height || vdp->line == vdp->lines - 1u) {
		if (vdp->hint_counter == 0) {
			vdp->hint_counter = vdp->registers[10];
			vdp->hint_pending = true;
		} else vdp->hint_counter--;
	} else vdp->hint_counter = vdp->registers[10];
}

static void draw_tile(
	const struct karashi_mdvdp *vdp, uint16_t name, unsigned row, unsigned cell_height, uint8_t *output
) {
	uint64_t attribute = (((name >> 9) & 0x30) | (name & 0x8000 ? PIXEL_PRIORITY : 0)) * UINT64_C(0x0101010101010101);
	unsigned address;
	const uint8_t *pattern;
	uint64_t pixels;

	if (name & 0x1000) row = cell_height - 1 - row;
	address = cell_height == 16 ? (name & 0x03ff) << 6 : (name & 0x07ff) << 5;
	pattern = vdp->vram + ((address + row * 4) & 0xfffc);
	if (name & 0x0800) {
		pixels = (uint64_t) nibbles_flipped[pattern[3]] | (uint64_t) nibbles_flipped[pattern[2]] << 16
		         | (uint64_t) nibbles_flipped[pattern[1]] << 32 | (uint64_t) nibbles_flipped[pattern[0]] << 48;
	} else {
		pixels = (uint64_t) nibbles[pattern[0]] | (uint64_t) nibbles[pattern[1]] << 16
		         | (uint64_t) nibbles[pattern[2]] << 32 | (uint64_t) nibbles[pattern[3]] << 48;
	}
	pixels |= attribute;
	output[0] = (uint8_t) pixels;
	output[1] = (uint8_t) (pixels >> 8);
	output[2] = (uint8_t) (pixels >> 16);
	output[3] = (uint8_t) (pixels >> 24);
	output[4] = (uint8_t) (pixels >> 32);
	output[5] = (uint8_t) (pixels >> 40);
	output[6] = (uint8_t) (pixels >> 48);
	output[7] = (uint8_t) (pixels >> 56);
}

static uint64_t load64(const uint8_t *data) {
	return (uint64_t) data[0] | (uint64_t) data[1] << 8 | (uint64_t) data[2] << 16 | (uint64_t) data[3] << 24
	       | (uint64_t) data[4] << 32 | (uint64_t) data[5] << 40 | (uint64_t) data[6] << 48 | (uint64_t) data[7] << 56;
}

static uint16_t vram_word(const struct karashi_mdvdp *vdp, unsigned address) {
	address &= 0xfffe;
	return (uint16_t) ((uint16_t) vdp->vram[address] << 8 | vdp->vram[address + 1]);
}

static unsigned plane_cells(unsigned code) {
	if (code == 1) return 64;
	if (code == 3) return 128;
	return 32;
}

static void render_plane(
	const struct karashi_mdvdp *vdp, unsigned plane, unsigned line, unsigned field_line, unsigned width,
	uint8_t *output
) {
	unsigned cell_height = interlace_double(vdp) ? 16 : 8;
	unsigned cell_shift = cell_height == 16 ? 4 : 3;
	unsigned plane_width = plane_cells(vdp->registers[16] & 3);
	unsigned plane_height = plane_cells((vdp->registers[16] >> 4) & 3);
	unsigned table = plane ? (vdp->registers[4] & 0x07) << 13 : (vdp->registers[2] & 0x38) << 10;
	unsigned scroll_table = (vdp->registers[13] & 0x3f) << 10;
	unsigned scroll_line;
	unsigned scroll_x;
	unsigned fine;
	int unit;
	int units = (int) width / 16;

	if (plane_width == 32 && (vdp->registers[16] & 3) == 2) plane_height = 1;
	else if (plane_width == 64 && plane_height == 128) plane_height = 64;
	else if (plane_width == 128) plane_height = 32;
	switch (vdp->registers[11] & 3) {
		case 0: scroll_line = 0; break;
		case 1: scroll_line = line & 7; break;
		case 2: scroll_line = line & ~7u; break;
		default: scroll_line = line; break;
	}
	scroll_x = vram_word(vdp, scroll_table + scroll_line * 4 + plane * 2) & 0x03ff;
	fine = scroll_x & 15;
	for (unit = fine ? -1 : 0; unit < units; unit++) {
		unsigned column = unit < 0 ? 0 : (unsigned) unit;
		unsigned scroll_y = vdp->registers[11] & 0x04 ? vdp->vsram[(column * 2 + plane) % KARASHI_MD_VSRAM_SIZE]
		                                            : vdp->vsram[plane];
		unsigned y = (field_line + (scroll_y & (cell_height == 16 ? 0x7ff : 0x3ff))) & (plane_height * cell_height - 1);
		unsigned row = y >> cell_shift;
		int screen_x = unit * 16 + (int) fine;
		unsigned plane_x = ((unsigned) (screen_x + 0x400) - scroll_x) & (plane_width * 8 - 1);
		unsigned tile;
		for (tile = 0; tile < 2; tile++) {
			unsigned cell_x = ((plane_x >> 3) + tile) & (plane_width - 1);
			uint16_t name = vram_word(vdp, table + (row * plane_width + cell_x) * 2);
			draw_tile(vdp, name, y & (cell_height - 1), cell_height, output + LINE_MARGIN + screen_x + (int) tile * 8);
		}
	}
}

static void render_window(
	const struct karashi_mdvdp *vdp, unsigned line, unsigned field_line, unsigned width, uint8_t *output
) {
	unsigned cell_height = interlace_double(vdp) ? 16 : 8;
	unsigned vertical = (vdp->registers[18] & 0x1f) * 8;
	unsigned horizontal = (vdp->registers[17] & 0x1f) * 16;
	bool down = (vdp->registers[18] & 0x80) != 0;
	bool right = (vdp->registers[17] & 0x80) != 0;
	unsigned table = h40(vdp) ? (vdp->registers[3] & 0x3c) << 10 : (vdp->registers[3] & 0x3e) << 10;
	unsigned table_width = h40(vdp) ? 64 : 32;
	unsigned start = 0;
	unsigned end = width;
	unsigned row = line >> 3;
	unsigned pixel_row = cell_height == 16 ? (field_line & 15) : (line & 7);
	unsigned x;

	if (down ? line < vertical : line >= vertical) {
		if (right) start = horizontal;
		else end = horizontal;
	}
	if (end > width) end = width;
	for (x = start; x < end; x += 8) {
		uint16_t name = vram_word(vdp, table + (row * table_width + (x >> 3)) * 2);
		draw_tile(vdp, name, pixel_row, cell_height, output + LINE_MARGIN + x);
	}
}

static void render_sprites(struct karashi_mdvdp *vdp, unsigned field_line, unsigned width, uint8_t *output) {
	bool tall = interlace_double(vdp);
	unsigned cell_height = tall ? 16 : 8;
	unsigned table = (vdp->registers[5] & (width == 320 ? 0x7e : 0x7f)) << 9;
	unsigned limit = width == 320 ? 80 : 64;
	unsigned line_limit = width == 320 ? 20 : 16;
	unsigned budget = width;
	unsigned on_line = 0;
	unsigned index = 0;
	unsigned count;
	bool masked = false;
	bool seen = vdp->dot_overflow;

	vdp->dot_overflow = false;
	for (count = 0; count < limit; count++) {
		unsigned address = table + index * 8;
		unsigned y_word = vram_word(vdp, address);
		unsigned size_link = vram_word(vdp, address + 2);
		unsigned columns = ((size_link >> 10) & 3) + 1;
		unsigned rows = ((size_link >> 8) & 3) + 1;
		int top = tall ? (int) (y_word & 0x3ff) - 256 : (int) (y_word & 0x1ff) - 128;
		int row = (int) field_line - top;

		if (row >= 0 && row < (int) (rows * cell_height)) {
			unsigned attributes = vram_word(vdp, address + 4);
			unsigned x_word = vram_word(vdp, address + 6) & 0x1ff;
			unsigned priority = attributes & 0x8000 ? PIXEL_PRIORITY : 0;
			unsigned palette = (attributes >> 9) & 0x30;
			unsigned pixels = columns * 8;
			unsigned column;

			if (++on_line > line_limit) {
				vdp->status |= STATUS_OVERFLOW;
				break;
			}
			if (!x_word) {
				if (seen) masked = true;
			} else seen = true;
			if (pixels > budget) pixels = budget;
			if (attributes & 0x1000) row = (int) (rows * cell_height) - 1 - row;
			for (column = 0; column * 8 < pixels && !masked; column++) {
				unsigned source_column = attributes & 0x0800 ? columns - 1 - column : column;
				unsigned tile = (attributes & (tall ? 0x03ff : 0x07ff)) + source_column * rows
				                + (unsigned) row / cell_height;
				unsigned pattern_address = ((tile << (tall ? 6 : 5)) + ((unsigned) row % cell_height) * 4) & 0xfffc;
				const uint8_t *pattern = vdp->vram + pattern_address;
				int screen_x = (int) x_word - 128 + (int) column * 8;
				unsigned pixel;
				if (screen_x <= -8 || screen_x >= (int) width) continue;
				if (!(pattern[0] | pattern[1] | pattern[2] | pattern[3])) continue;
				for (pixel = 0; pixel < 8; pixel++) {
					unsigned source = attributes & 0x0800 ? 7 - pixel : pixel;
					unsigned colour = source & 1 ? pattern[source >> 1] & 15u : pattern[source >> 1] >> 4;
					uint8_t *target = output + LINE_MARGIN + screen_x + (int) pixel;
					if (!colour) continue;
					if (*target & 15) vdp->status |= STATUS_COLLISION;
					else *target = (uint8_t) (priority | palette | colour);
				}
			}
			budget -= pixels;
			if (!budget) {
				vdp->dot_overflow = true;
				vdp->status |= STATUS_OVERFLOW;
				break;
			}
		}
		index = size_link & 0x7f;
		if (!index || index >= limit) break;
	}
}

void karashi_mdvdp_render_line(struct karashi_mdvdp *vdp, unsigned line, karashi_pixel *output) {
	karashi_mdvdp_render_line_mask(vdp, line, output, NULL);
}

void karashi_mdvdp_render_line_mask(
	struct karashi_mdvdp *vdp, unsigned line, karashi_pixel *output, uint8_t *backdrop
) {
	uint8_t plane_a[KARASHI_MD_WIDTH + LINE_MARGIN * 2];
	uint8_t plane_b[KARASHI_MD_WIDTH + LINE_MARGIN * 2];
	uint8_t sprites[KARASHI_MD_WIDTH + LINE_MARGIN * 2];
	unsigned width = karashi_mdvdp_width(vdp);
	unsigned field_line = interlace_double(vdp) ? line * 2 + (vdp->odd_frame ? 1 : 0) : line;
	unsigned background = vdp->registers[7] & 0x3f;
	const uint8_t *a = plane_a + LINE_MARGIN;
	const uint8_t *b = plane_b + LINE_MARGIN;
	const uint8_t *s = sprites + LINE_MARGIN;
	const karashi_pixel *palette = vdp->palette;
	bool shade = (vdp->registers[12] & 0x08) != 0;
	unsigned x;

	if (!(vdp->registers[1] & 0x40)) {
		for (x = 0; x < KARASHI_MD_WIDTH; x++) output[x] = palette[background];
		if (backdrop) memset(backdrop, 1, KARASHI_MD_WIDTH);
		return;
	}
	memset(sprites, 0, sizeof(sprites));
	render_plane(vdp, 1, line, field_line, width, plane_b);
	render_plane(vdp, 0, line, field_line, width, plane_a);
	render_window(vdp, line, field_line, width, plane_a);
	render_sprites(vdp, field_line, width, sprites);
	if (!shade) {
		const uint64_t ones = UINT64_C(0x0101010101010101);
		const uint64_t low = UINT64_C(0x0f0f0f0f0f0f0f0f);
		for (x = 0; x < width; x += 8) {
			uint64_t pa = load64(a + x);
			uint64_t pb = load64(b + x);
			uint64_t ps = load64(s + x);
			uint64_t oa = ((((pa & low) + low) >> 4) & ones) * 0xff;
			uint64_t ob = ((((pb & low) + low) >> 4) & ones) * 0xff;
			uint64_t os = ((((ps & low) + low) >> 4) & ones) * 0xff;
			uint64_t ha = oa & (((pa >> 6) & ones) * 0xff);
			uint64_t hb = ob & (((pb >> 6) & ones) * 0xff);
			uint64_t hs = os & (((ps >> 6) & ones) * 0xff);
			uint64_t pixels = background * ones;
			unsigned index;
			pixels = (pixels & ~ob) | (pb & ob);
			pixels = (pixels & ~oa) | (pa & oa);
			pixels = (pixels & ~os) | (ps & os);
			pixels = (pixels & ~hb) | (pb & hb);
			pixels = (pixels & ~ha) | (pa & ha);
			pixels = (pixels & ~hs) | (ps & hs);
			output[x] = palette[pixels & 0x3f];
			output[x + 1] = palette[(pixels >> 8) & 0x3f];
			output[x + 2] = palette[(pixels >> 16) & 0x3f];
			output[x + 3] = palette[(pixels >> 24) & 0x3f];
			output[x + 4] = palette[(pixels >> 32) & 0x3f];
			output[x + 5] = palette[(pixels >> 40) & 0x3f];
			output[x + 6] = palette[(pixels >> 48) & 0x3f];
			output[x + 7] = palette[(pixels >> 56) & 0x3f];
			if (backdrop) {
				uint64_t opaque = oa | ob | os;
				for (index = 0; index < 8; index++) backdrop[x + index] = !((opaque >> (index * 8)) & 1);
			}
		}
	} else {
		for (x = 0; x < width; x++) {
			unsigned pa = a[x];
			unsigned pb = b[x];
			unsigned ps = s[x];
			unsigned level = (pa | pb) & PIXEL_PRIORITY ? 0 : 64;
			unsigned pixel = background;
			bool plane_front = false;
			if ((pa & 15) && (pa & PIXEL_PRIORITY)) {
				pixel = pa;
				plane_front = true;
			} else if ((pb & 15) && (pb & PIXEL_PRIORITY)) {
				pixel = pb;
				plane_front = true;
			} else if (pa & 15) pixel = pa;
			else if (pb & 15) pixel = pb;
			if ((ps & 15) && ((ps & PIXEL_PRIORITY) || !plane_front)) {
				unsigned colour = ps & 0x3f;
				if (colour == 0x3e) level = level ? 0 : 128;
				else if (colour == 0x3f) level = 64;
				else {
					pixel = ps;
					if ((ps & PIXEL_PRIORITY) || (ps & 15) == 14) level = 0;
				}
			}
			output[x] = palette[level + (pixel & 0x3f)];
			if (backdrop) backdrop[x] = !((pa | pb | ps) & 15);
		}
	}
	if (vdp->registers[0] & 0x20) {
		for (x = 0; x < 8; x++) output[x] = palette[background];
		if (backdrop) memset(backdrop, 1, 8);
	}
	for (x = width; x < KARASHI_MD_WIDTH; x++) output[x] = palette[background];
	if (backdrop && width < KARASHI_MD_WIDTH) memset(backdrop + width, 1, KARASHI_MD_WIDTH - width);
}

bool karashi_mdvdp_save(const struct karashi_mdvdp *vdp, struct karashi_state_writer *writer) {
	unsigned index;

	karashi_state_write_bytes(writer, vdp->vram, sizeof(vdp->vram));
	for (index = 0; index < KARASHI_MD_CRAM_SIZE; index++) karashi_state_write_u16(writer, vdp->cram[index]);
	for (index = 0; index < KARASHI_MD_VSRAM_SIZE; index++) karashi_state_write_u16(writer, vdp->vsram[index]);
	karashi_state_write_bytes(writer, vdp->registers, sizeof(vdp->registers));
	karashi_state_write_u32(writer, vdp->address);
	for (index = 0; index < 4; index++) karashi_state_write_u64(writer, vdp->fifo_finish[index]);
	karashi_state_write_u64(writer, vdp->dma_finish);
	karashi_state_write_u64(writer, vdp->port_clock);
	karashi_state_write_u16(writer, vdp->status);
	karashi_state_write_u16(writer, vdp->line);
	karashi_state_write_u16(writer, vdp->hv_latch);
	karashi_state_write_u16(writer, vdp->last_data);
	karashi_state_write_u8(writer, vdp->code);
	karashi_state_write_u8(writer, vdp->fifo_head);
	karashi_state_write_u8(writer, vdp->fifo_count);
	karashi_state_write_u8(writer, vdp->hint_counter);
	karashi_state_write_bool(writer, vdp->control_pending);
	karashi_state_write_bool(writer, vdp->vint_pending);
	karashi_state_write_bool(writer, vdp->hint_pending);
	karashi_state_write_bool(writer, vdp->fill_pending);
	karashi_state_write_bool(writer, vdp->odd_frame);
	karashi_state_write_bool(writer, vdp->hv_latched);
	karashi_state_write_bool(writer, vdp->vint_flag);
	return karashi_state_write_bool(writer, vdp->dot_overflow);
}

bool karashi_mdvdp_load(struct karashi_mdvdp *vdp, struct karashi_state_reader *reader) {
	bool loaded = true;
	unsigned index;

	loaded = karashi_state_read_bytes(reader, vdp->vram, sizeof(vdp->vram)) && loaded;
	for (index = 0; index < KARASHI_MD_CRAM_SIZE; index++) {
		loaded = karashi_state_read_u16(reader, &vdp->cram[index]) && loaded;
	}
	for (index = 0; index < KARASHI_MD_VSRAM_SIZE; index++) {
		loaded = karashi_state_read_u16(reader, &vdp->vsram[index]) && loaded;
	}
	loaded = karashi_state_read_bytes(reader, vdp->registers, sizeof(vdp->registers)) && loaded;
	loaded = karashi_state_read_u32(reader, &vdp->address) && loaded;
	for (index = 0; index < 4; index++) {
		loaded = karashi_state_read_u64(reader, &vdp->fifo_finish[index]) && loaded;
	}
	loaded = karashi_state_read_u64(reader, &vdp->dma_finish) && loaded;
	loaded = karashi_state_read_u64(reader, &vdp->port_clock) && loaded;
	loaded = karashi_state_read_u16(reader, &vdp->status) && loaded;
	loaded = karashi_state_read_u16(reader, &vdp->line) && loaded;
	loaded = karashi_state_read_u16(reader, &vdp->hv_latch) && loaded;
	loaded = karashi_state_read_u16(reader, &vdp->last_data) && loaded;
	loaded = karashi_state_read_u8(reader, &vdp->code) && loaded;
	loaded = karashi_state_read_u8(reader, &vdp->fifo_head) && loaded;
	loaded = karashi_state_read_u8(reader, &vdp->fifo_count) && loaded;
	loaded = karashi_state_read_u8(reader, &vdp->hint_counter) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->control_pending) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->vint_pending) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->hint_pending) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->fill_pending) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->odd_frame) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->hv_latched) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->vint_flag) && loaded;
	loaded = karashi_state_read_bool(reader, &vdp->dot_overflow) && loaded;
	if (!loaded) return false;
	vdp->address &= 0xffff;
	if (vdp->fifo_head > 3 || vdp->fifo_count > 4) return false;
	vdp->line %= vdp->lines;
	for (index = 0; index < KARASHI_MD_CRAM_SIZE; index++) {
		vdp->cram[index] &= 0x0eee;
		update_colour(vdp, index);
	}
	for (index = 0; index < KARASHI_MD_VSRAM_SIZE; index++) vdp->vsram[index] &= 0x07ff;
	return true;
}
