#include "sms.h"

#include <stdint.h>
#include <string.h>

#define PAGE_SIZE 0x4000
#define BLOCK_SIZE 0x0400
#define RAM_ENABLE 0x08
#define RAM_BANK 0x04
#define EEPROM_ENABLE 0x08
#define EEPROM_RESET 0x80
#define FM_GAIN 2
#define TH_ONE_INPUT 0x02
#define TH_TWO_INPUT 0x08
#define TH_ONE_LEVEL 0x20
#define TH_TWO_LEVEL 0x80

#ifndef KARASHI_SMS_LINE_EVENT_CYCLE
#define KARASHI_SMS_LINE_EVENT_CYCLE 171
#endif
#define IO_READ_CYCLE 9
#define BIOS_STACK 0xdff0
#define MEMORY_BIOS_OFF 0x08
#define MEMORY_CART_OFF 0x40
#define GG_BIOS_SIZE 0x0400

static void update_irq(struct karashi_sms *machine) {
	karashi_z80_irq(&machine->cpu, karashi_smsvdp_interrupt(&machine->vdp));
}

static void line_event(struct karashi_sms *machine) {
	if (!machine->line_event_pending) return;
	machine->line_event_pending = false;
	karashi_smsvdp_end_line(&machine->vdp, machine->line);
	update_irq(machine);
}

static void run_until(struct karashi_sms *machine, uint64_t target) {
	while (machine->cpu.cycles < target) karashi_z80_step(&machine->cpu);
}

static void sound_sync(struct karashi_sms *machine, uint64_t target);

static bool bios_active(const struct karashi_sms *machine) {
	return machine->bios && !machine->game_gear && !(machine->memory_control & MEMORY_BIOS_OFF)
	       && (machine->memory_control & MEMORY_CART_OFF);
}

static bool gg_bios_active(const struct karashi_sms *machine) {
	return machine->bios && machine->game_gear && !(machine->memory_control & MEMORY_BIOS_OFF);
}

static bool sega_paging(const struct karashi_sms *machine) {
	return bios_active(machine) || machine->mapper == KARASHI_SMS_MAPPER_SEGA;
}

static const uint8_t *slot_source(const struct karashi_sms *machine, size_t *size) {
	if (bios_active(machine)) {
		*size = machine->bios_size;
		return machine->bios;
	}
	if (machine->bios && !machine->game_gear && (machine->memory_control & MEMORY_CART_OFF)) {
		*size = 0;
		return NULL;
	}
	*size = machine->rom_size;
	return machine->rom;
}

static size_t source_offset(size_t size, unsigned page, uint16_t address) {
	size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	size_t offset = (size_t) (page % pages) * PAGE_SIZE + (address & (PAGE_SIZE - 1));

	return size < PAGE_SIZE ? offset % size : offset;
}

static size_t cart_offset(const struct karashi_sms *machine, size_t size, uint16_t address) {
	static const uint8_t msx_register[4] = {2, 3, 0, 1};
	unsigned slot = address >> 14;
	unsigned page = machine->pages[slot];

	if (machine->mapper == KARASHI_SMS_MAPPER_MSX && !bios_active(machine)) {
		size_t banks = (size + 0x1fff) / 0x2000;
		if (address < 0x4000) return address < size ? address : SIZE_MAX;
		return (size_t) (machine->banks8[msx_register[(address - 0x4000) >> 13]] % banks) * 0x2000 + (address & 0x1fff);
	}
	if (machine->mapper == KARASHI_SMS_MAPPER_KOREAN && !bios_active(machine)) page = slot < 2 ? slot : machine->pages[2];
	else if (sega_paging(machine) && address < 0x0400) page = 0;
	return source_offset(size, page, address);
}

static uint8_t rom_read(const struct karashi_sms *machine, uint16_t address) {
	size_t size;
	const uint8_t *data = slot_source(machine, &size);
	size_t offset;

	if (!data || !size) return 0xff;
	offset = cart_offset(machine, size, address);
	return offset < size ? data[offset] : 0xff;
}

static void map_rom(struct karashi_sms *machine) {
	size_t size;
	const uint8_t *data = slot_source(machine, &size);
	unsigned block;

	for (block = 0; block < KARASHI_SMS_ROM_BLOCKS; block++) {
		size_t offset;

		machine->rom_map[block] = NULL;
		if (!data || !size || ((block >> 4) == 2 && (machine->ram_control & RAM_ENABLE))) continue;
		if (block == 0 && gg_bios_active(machine)) continue;
		offset = cart_offset(machine, size, (uint16_t) (block << 10));
		if (offset != SIZE_MAX && offset + BLOCK_SIZE <= size) machine->rom_map[block] = data + offset;
	}
}

static uint8_t *cart_ram(struct karashi_sms *machine, uint16_t address) {
	return machine->cart_ram + ((machine->ram_control & RAM_BANK) ? PAGE_SIZE : 0) + (address & (PAGE_SIZE - 1));
}

static uint8_t memory_read(void *context, uint16_t address) {
	struct karashi_sms *machine = context;
	const uint8_t *block;

	if (address >= 0xc000) return machine->ram[address & (KARASHI_SMS_RAM_SIZE - 1)];
	if (machine->eeprom_enabled && address == 0x8000) return karashi_eeprom93_read(&machine->eeprom);
	block = machine->rom_map[address >> 10];
	if (block) return block[address & (BLOCK_SIZE - 1)];
	if (address >= 0x8000 && (machine->ram_control & RAM_ENABLE)) return *cart_ram(machine, address);
	if (address < GG_BIOS_SIZE && gg_bios_active(machine)) return machine->bios[address % machine->bios_size];
	return rom_read(machine, address);
}

static void memory_write(void *context, uint16_t address, uint8_t value) {
	struct karashi_sms *machine = context;

	if (machine->eeprom_enabled && address == 0x8000) {
		karashi_eeprom93_write(&machine->eeprom, value, machine->cart_ram);
		return;
	}
	if (machine->eeprom_present && address == 0xfffc) {
		machine->eeprom_enabled = (value & EEPROM_ENABLE) != 0;
		if (value & EEPROM_RESET) {
			karashi_eeprom93_reset(&machine->eeprom);
			machine->eeprom_enabled = false;
		}
		value &= (uint8_t) ~(RAM_ENABLE | EEPROM_RESET);
	}

	if (machine->mapper == KARASHI_SMS_MAPPER_SEGA && !machine->sega_paging_seen && !bios_active(machine)
	    && !machine->licensed && machine->rom_size > 0x8000 && machine->cpu.pc < 0xc000) {
		if (address < 4) machine->mapper = KARASHI_SMS_MAPPER_MSX;
		else if (address == 0xa000) machine->mapper = KARASHI_SMS_MAPPER_KOREAN;
	}
	if (machine->mapper == KARASHI_SMS_MAPPER_MSX && !bios_active(machine) && address < 4) {
		machine->banks8[address] = value;
		map_rom(machine);
	} else if (machine->mapper == KARASHI_SMS_MAPPER_KOREAN && !bios_active(machine) && address == 0xa000) {
		machine->pages[2] = value;
		map_rom(machine);
	} else if (machine->mapper == KARASHI_SMS_MAPPER_CODEMASTERS && !bios_active(machine)) {
		if (address == 0x0000 || address == 0x4000 || address == 0x8000) {
			machine->pages[address >> 14] = value;
			map_rom(machine);
		}
	} else if (address >= 0xfffc) {
		if (!bios_active(machine)) machine->sega_paging_seen = true;
		if (address == 0xfffc) machine->ram_control = value;
		else machine->pages[address - 0xfffd] = value;
		map_rom(machine);
	} else if (address >= 0x8000 && address < 0xc000 && (machine->ram_control & RAM_ENABLE)) {
		*cart_ram(machine, address) = value;
	}
	if (address >= 0xc000) machine->ram[address & (KARASHI_SMS_RAM_SIZE - 1)] = value;
}

#define PHASER_LINES 4
#define PHASER_OFFSET 44
#define PADDLE_HALF_PERIOD 224
#define SPORTS_PAD_PHASE 224
#define SPORTS_PAD_TIMEOUT 2048

static bool bright(const karashi_pixel *pixel) {
	unsigned red = (*pixel >> 11) & 31;
	unsigned green = (*pixel >> 6) & 31;
	unsigned blue = *pixel & 31;

	return red + green + blue >= 48;
}

static unsigned line_pixel(const struct karashi_sms *machine) {
	return (unsigned) ((machine->cpu.cycles - machine->line_start) * 3 / 2);
}

static bool phaser_sees(const struct karashi_sms *machine, unsigned port, bool whole_line) {
	int x = machine->phaser_x[port];
	int y = machine->phaser_y[port];
	int line = (int) machine->line;

	if (machine->device[port] != KARASHI_SMS_DEVICE_PHASER || x < 0 || y < 0 || x >= KARASHI_SMSVDP_WIDTH || y >= (int) machine->vdp.height) return false;
	if (line < y - PHASER_LINES || line > y + PHASER_LINES) return false;
	if (!whole_line && line_pixel(machine) < (unsigned) x) return false;
	return bright(machine->video + y * KARASHI_SMSVDP_WIDTH + x);
}

static void phaser_latch(struct karashi_sms *machine, unsigned port) {
	if (machine->phaser_latched == machine->line + 1) return;
	machine->phaser_latched = (uint16_t) (machine->line + 1);
	karashi_smsvdp_latch_hcounter(&machine->vdp, (unsigned) (machine->phaser_x[port] + PHASER_OFFSET) * 2 / 3);
}

static bool th_output(const struct karashi_sms *machine, unsigned port) {
	return !machine->japanese && !(machine->io_control & (port ? TH_TWO_INPUT : TH_ONE_INPUT));
}

static bool th_line(uint8_t io_control, unsigned port) {
	if (io_control & (port ? TH_TWO_INPUT : TH_ONE_INPUT)) return true;
	return (io_control & (port ? TH_TWO_LEVEL : TH_ONE_LEVEL)) != 0;
}

static bool th_level(const struct karashi_sms *machine, unsigned port) {
	return (machine->io_control & (port ? TH_TWO_LEVEL : TH_ONE_LEVEL)) != 0;
}

static uint8_t paddle_bits(const struct karashi_sms *machine, unsigned port) {
	bool high = th_output(machine, port) ? th_level(machine, port)
	                                     : (machine->cpu.cycles / PADDLE_HALF_PERIOD) & 1;
	uint8_t nibble = (uint8_t) (high ? machine->paddle[port] >> 4 : machine->paddle[port] & 0x0f);

	return (uint8_t) (nibble | (machine->paddle_button[port] ? 0 : 0x10) | (high ? 0x20 : 0));
}

static uint8_t sports_nibble(const uint8_t *value, unsigned index) {
	return (uint8_t) (index & 1 ? value[index >> 1] & 0x0f : value[index >> 1] >> 4);
}

static uint8_t sports_bits(struct karashi_sms *machine, unsigned port) {
	uint8_t buttons = (uint8_t) ((uint8_t) ~machine->sports_buttons[port] << 4 & 0x30);
	unsigned phase;

	if (!machine->japanese) {
		if (!machine->sports_index[port]) return (uint8_t) (buttons | 0x0f);
		return (uint8_t) (buttons | sports_nibble(machine->sports_latch[port], (machine->sports_index[port] - 1u) & 3));
	}
	phase = (unsigned) (machine->cpu.cycles / SPORTS_PAD_PHASE % 6);
	if (phase == 1) {
		machine->sports_latch[port][0] = machine->sports_position[port][0];
		machine->sports_latch[port][1] = machine->sports_position[port][1];
		return (uint8_t) (0x30 | (~machine->sports_buttons[port] & 0x0f));
	}
	if (phase == 0) return 0x1f;
	return (uint8_t) (((phase & 1) ? 0x10 : 0) | sports_nibble(machine->sports_latch[port], phase - 2));
}

static uint8_t port_bits(struct karashi_sms *machine, unsigned port) {
	switch (machine->device[port]) {
		case KARASHI_SMS_DEVICE_PHASER: return machine->phaser_trigger[port] ? 0x2f : 0x3f;
		case KARASHI_SMS_DEVICE_PADDLE: return paddle_bits(machine, port);
		case KARASHI_SMS_DEVICE_SPORTS_PAD: return sports_bits(machine, port);
		default: return (uint8_t) (~machine->controller[port] & 0x3f);
	}
}

static void sports_edge(struct karashi_sms *machine, unsigned port) {
	if (machine->cpu.cycles - machine->sports_edge[port] > SPORTS_PAD_TIMEOUT) machine->sports_index[port] = 0;
	machine->sports_edge[port] = machine->cpu.cycles;
	if (!machine->sports_index[port]) {
		machine->sports_latch[port][0] = (uint8_t) -machine->sports_move[port][0];
		machine->sports_latch[port][1] = (uint8_t) -machine->sports_move[port][1];
		machine->sports_move[port][0] = 0;
		machine->sports_move[port][1] = 0;
	}
	machine->sports_index[port]++;
}

static uint8_t pad_low(struct karashi_sms *machine) {
	return (uint8_t) (port_bits(machine, 0) | (port_bits(machine, 1) & 0x03) << 6);
}

static uint8_t pad_high(struct karashi_sms *machine) {
	uint8_t value = (uint8_t) (0xf0 | port_bits(machine, 1) >> 2);
	unsigned port;

	for (port = 0; port < 2; port++) {
		if (phaser_sees(machine, port, false)) {
			value &= (uint8_t) ~(0x40 << port);
			phaser_latch(machine, port);
		}
	}
	if (machine->japanese) return value;
	if (!(machine->io_control & TH_ONE_INPUT) && !(machine->io_control & TH_ONE_LEVEL)) value &= (uint8_t) ~0x40;
	if (!(machine->io_control & TH_TWO_INPUT) && !(machine->io_control & TH_TWO_LEVEL)) value &= (uint8_t) ~0x80;
	return value;
}

static uint8_t game_gear_read(const struct karashi_sms *machine, uint8_t port) {
	static const uint8_t link[5] = {0x7f, 0xff, 0x00, 0xff, 0x00};

	if (port == 0) return (uint8_t) ((machine->pause ? 0x00 : 0x80) | (machine->japanese ? 0x00 : 0x40));
	if (port < 6) return link[port - 1];
	return 0xff;
}

static uint8_t io_read(void *context, uint16_t port) {
	struct karashi_sms *machine = context;
	uint8_t decoded = (uint8_t) port;

	if (machine->game_gear && decoded < 7) return game_gear_read(machine, decoded);
	if (machine->fm_present && decoded == 0xf2) return machine->fm_control;
	switch (decoded & 0xc1) {
		case 0x40: return karashi_smsvdp_read_vcounter(&machine->vdp);
		case 0x41: return karashi_smsvdp_read_hcounter(&machine->vdp);
		case 0x80: return karashi_smsvdp_read_data(&machine->vdp);
		case 0x81: {
			uint8_t status;
			if (machine->cpu.cycles + IO_READ_CYCLE >= machine->line_event) line_event(machine);
			status = karashi_smsvdp_read_status(&machine->vdp);
			update_irq(machine);
			return status;
		}
		case 0xc0: return pad_low(machine);
		case 0xc1: return pad_high(machine);
		default: return 0xff;
	}
}

static void io_control_write(struct karashi_sms *machine, uint8_t value) {
	uint8_t rising = (uint8_t) (~machine->io_control & value);
	unsigned port;

	for (port = 0; port < 2; port++) {
		if (machine->device[port] == KARASHI_SMS_DEVICE_SPORTS_PAD && th_line(machine->io_control, port) != th_line(value, port)) {
			sports_edge(machine, port);
		}
	}

	if (rising & (TH_ONE_LEVEL | TH_TWO_LEVEL)) {
		karashi_smsvdp_latch_hcounter(&machine->vdp, (unsigned) (machine->cpu.cycles - machine->line_start));
	}
	machine->io_control = value;
}

static void io_write(void *context, uint16_t port, uint8_t value) {
	struct karashi_sms *machine = context;
	uint8_t decoded = (uint8_t) port;

	if (machine->game_gear && decoded < 7) {
		if (decoded == 6) {
			sound_sync(machine, machine->cpu.cycles);
			karashi_sn76489_set_stereo(&machine->psg, value);
		}
		return;
	}
	if (machine->fm_present && decoded >= 0xf0 && decoded <= 0xf2) {
		if (decoded == 0xf2) {
			sound_sync(machine, machine->cpu.cycles);
			machine->fm_control = value & 3;
		} else {
			if (decoded == 0xf1) sound_sync(machine, machine->cpu.cycles);
			karashi_ym2413_write(&machine->fm, decoded == 0xf1, value);
		}
		return;
	}
	switch (decoded & 0xc1) {
		case 0x00:
			machine->memory_control = value;
			map_rom(machine);
			break;
		case 0x01: io_control_write(machine, value); break;
		case 0x40:
		case 0x41:
			sound_sync(machine, machine->cpu.cycles);
			karashi_sn76489_write(&machine->psg, value);
			break;
		case 0x80: karashi_smsvdp_write_data(&machine->vdp, value); break;
		case 0x81:
			karashi_smsvdp_write_control(&machine->vdp, value);
			update_irq(machine);
			break;
		default: break;
	}
}

static bool has_header(const uint8_t *rom, size_t rom_size) {
	static const size_t offsets[3] = {0x1ff0, 0x3ff0, 0x7ff0};
	unsigned index;

	for (index = 0; index < 3; index++) {
		if (offsets[index] + 8 <= rom_size && memcmp(rom + offsets[index], "TMR SEGA", 8) == 0) return true;
	}
	return false;
}

static bool has_eeprom(const uint8_t *rom, size_t rom_size) {
	static const uint32_t checksums[] = {0x36ebcd6d, 0x2da8e943, 0x3d8d0dd6, 0xbb38cfd7, 0x578a8a38};
	uint32_t checksum = karashi_state_crc32(rom, rom_size);
	unsigned index;

	for (index = 0; index < sizeof(checksums) / sizeof(checksums[0]); index++) {
		if (checksums[index] == checksum) return true;
	}
	return false;
}

static uint8_t detect_mapper(const uint8_t *rom, size_t rom_size) {
	unsigned checksum;
	unsigned inverse;

	if (rom_size < 0x8000) return KARASHI_SMS_MAPPER_SEGA;
	checksum = rom[0x7fe6] | (unsigned) rom[0x7fe7] << 8;
	inverse = rom[0x7fe8] | (unsigned) rom[0x7fe9] << 8;
	if (checksum && ((checksum + inverse) & 0xffff) == 0) return KARASHI_SMS_MAPPER_CODEMASTERS;
	return KARASHI_SMS_MAPPER_SEGA;
}

static uint32_t cpu_clock(const struct karashi_sms *machine) {
	return machine->pal ? KARASHI_SEGA8_PAL_CPU_CLOCK : KARASHI_SEGA8_CPU_CLOCK;
}

static unsigned frame_lines(const struct karashi_sms *machine) {
	return machine->pal ? KARASHI_SEGA8_PAL_LINES : KARASHI_SEGA8_LINES;
}

unsigned karashi_sms_height(const struct karashi_sms *machine) {
	return machine->vdp.height;
}

double karashi_sms_frame_rate(const struct karashi_sms *machine) {
	return (double) cpu_clock(machine) / (KARASHI_SEGA8_LINE_CYCLES * frame_lines(machine));
}

void karashi_sms_init(
	struct karashi_sms *machine, const uint8_t *rom, size_t rom_size, bool game_gear, bool japanese, bool pal
) {
	memset(machine, 0, sizeof(*machine));
	machine->rom = rom;
	machine->rom_size = rom_size;
	machine->game_gear = game_gear;
	machine->japanese = japanese;
	machine->pal = pal && !game_gear;
	machine->base_mapper = detect_mapper(rom, rom_size);
	machine->licensed = has_header(rom, rom_size);
	machine->eeprom_present = has_eeprom(rom, rom_size);
	machine->fm_present = japanese && !game_gear;
	if (machine->eeprom_present) memset(machine->cart_ram, 0xff, KARASHI_EEPROM93_SIZE);
	karashi_sms_reset(machine);
}

void karashi_sms_set_bios(struct karashi_sms *machine, const uint8_t *bios, size_t bios_size) {
	machine->bios = bios_size ? bios : NULL;
	machine->bios_size = bios ? bios_size : 0;
	karashi_sms_reset(machine);
}

void karashi_sms_set_region(struct karashi_sms *machine, bool japanese, bool pal) {
	uint32_t old_clock = cpu_clock(machine);
	bool fm_present = japanese && !machine->game_gear;

	machine->japanese = japanese;
	machine->pal = pal && !machine->game_gear;
	machine->vdp.pal = machine->pal;
	if (old_clock != cpu_clock(machine)) {
		machine->audio_clock = (uint32_t) ((uint64_t) machine->audio_clock * cpu_clock(machine) / old_clock);
		machine->psg.clock_rate = cpu_clock(machine);
	}
	if (fm_present && !machine->fm_present) machine->fm_clock = machine->cpu.cycles;
	machine->fm_present = fm_present;
}

void karashi_sms_reset(struct karashi_sms *machine) {
	struct karashi_z80_bus bus;

	memset(machine->ram, 0, sizeof(machine->ram));
	memset(machine->controller, 0, sizeof(machine->controller));
	machine->mapper = machine->base_mapper;
	machine->sega_paging_seen = false;
	machine->eeprom_enabled = false;
	karashi_eeprom93_reset(&machine->eeprom);
	machine->fm_control = 0;
	karashi_ym2413_reset(&machine->fm);
	machine->banks8[0] = 4;
	machine->banks8[1] = 5;
	machine->banks8[2] = 2;
	machine->banks8[3] = 3;
	machine->pages[0] = 0;
	machine->pages[1] = 1;
	machine->pages[2] = machine->mapper == KARASHI_SMS_MAPPER_CODEMASTERS ? 0 : 2;
	machine->ram_control = 0;
	machine->memory_control = machine->bios ? (machine->game_gear ? 0xa0 : 0xe3) : 0xab;
	map_rom(machine);
	machine->io_control = 0xff;
	machine->pause = false;
	machine->pause_seen = false;
	machine->line_start = 0;
	machine->line_event = 0;
	machine->line = 0;
	machine->line_event_pending = false;
	machine->sound_clock = 0;
	machine->fm_clock = 0;
	machine->audio_clock = 0;
	machine->audio_frames = 0;
	machine->fm_frames = 0;
	bus.context = machine;
	bus.read = memory_read;
	bus.write = memory_write;
	bus.io_read = io_read;
	bus.io_write = io_write;
	karashi_z80_init(&machine->cpu, &bus);
	if (!machine->bios) {
		machine->cpu.sp = BIOS_STACK;
		machine->ram[0x1ffe] = 1;
		machine->ram[0x1fff] = 2;
	}
	karashi_smsvdp_reset(&machine->vdp, machine->game_gear, machine->pal);
	karashi_sn76489_reset(&machine->psg, cpu_clock(machine));
}

void karashi_sms_set_device(struct karashi_sms *machine, unsigned port, enum karashi_sms_device device) {
	if (port > 1) return;
	machine->device[port] = (uint8_t) (machine->game_gear ? KARASHI_SMS_DEVICE_PAD : device);
}

void karashi_sms_set_paddle(struct karashi_sms *machine, unsigned port, uint8_t position, bool button) {
	if (port > 1) return;
	machine->paddle[port] = position;
	machine->paddle_button[port] = button;
}

void karashi_sms_set_sports_pad(struct karashi_sms *machine, unsigned port, int x, int y, uint8_t buttons) {
	if (port > 1) return;
	machine->sports_move[port][0] = (int16_t) (machine->sports_move[port][0] + x);
	machine->sports_move[port][1] = (int16_t) (machine->sports_move[port][1] + y);
	machine->sports_position[port][0] = (uint8_t) (machine->sports_position[port][0] + x);
	machine->sports_position[port][1] = (uint8_t) (machine->sports_position[port][1] + y);
	machine->sports_buttons[port] = buttons;
}

void karashi_sms_set_phaser(struct karashi_sms *machine, unsigned port, int x, int y, bool trigger) {
	if (port > 1) return;
	machine->phaser_x[port] = (int16_t) x;
	machine->phaser_y[port] = (int16_t) y;
	machine->phaser_trigger[port] = trigger;
}

void karashi_sms_set_controller(struct karashi_sms *machine, unsigned port, uint8_t state) {
	if (port < 2) machine->controller[port] = state & 0x3f;
}

void karashi_sms_set_pause(struct karashi_sms *machine, bool pressed) {
	machine->pause = pressed;
}

static void fm_sync(struct karashi_sms *machine, uint64_t target) {
	unsigned frames;
	unsigned kept;
	unsigned index;

	if (target <= machine->fm_clock) return;
	frames = (unsigned) ((target - machine->fm_clock) / KARASHI_YM2413_CLOCK_DIVIDER);
	if (!frames) return;
	machine->fm_clock += (uint64_t) frames * KARASHI_YM2413_CLOCK_DIVIDER;
	kept = frames;
	if (kept > KARASHI_SEGA8_AUDIO_CAPACITY - machine->fm_frames) {
		kept = KARASHI_SEGA8_AUDIO_CAPACITY - machine->fm_frames;
	}
	for (index = 0; index < kept; index++) {
		int32_t value = karashi_ym2413_sample(&machine->fm);
		machine->fm_audio[machine->fm_frames++] = machine->fm_control & 1 ? value * FM_GAIN : 0;
	}
	for (; index < frames; index++) (void) karashi_ym2413_sample(&machine->fm);
}

static void psg_sync(struct karashi_sms *machine, uint64_t target) {
	uint64_t elapsed;
	uint64_t scaled;
	unsigned frames;
	unsigned index;
	bool enabled;

	if (target <= machine->sound_clock) return;
	elapsed = target - machine->sound_clock;
	scaled = machine->audio_clock + elapsed * KARASHI_SEGA8_SAMPLE_RATE;
	frames = (unsigned) (scaled / cpu_clock(machine));
	machine->audio_clock = (uint32_t) (scaled % cpu_clock(machine));
	machine->sound_clock = target;
	if (frames > KARASHI_SEGA8_AUDIO_CAPACITY - machine->audio_frames) {
		frames = KARASHI_SEGA8_AUDIO_CAPACITY - machine->audio_frames;
	}
	karashi_sn76489_generate(
		&machine->psg, machine->audio + machine->audio_frames * 2, frames, KARASHI_SEGA8_SAMPLE_RATE
	);
	enabled = !((machine->fm_control ^ (machine->fm_control >> 1)) & 1);
	if (!enabled) {
		for (index = 0; index < frames * 2; index++) machine->audio[machine->audio_frames * 2 + index] = 0;
	}
	machine->audio_frames += frames;
}

static void sound_sync(struct karashi_sms *machine, uint64_t target) {
	psg_sync(machine, target);
	if (machine->fm_present) fm_sync(machine, target);
}

static void mix_fm(struct karashi_sms *machine) {
	unsigned frames = machine->audio_frames;
	unsigned available = machine->fm_frames;
	uint64_t position = 0;
	uint64_t advance = frames ? (uint64_t) available * 65536 / frames : 0;
	unsigned advance_remainder = frames ? (unsigned) ((uint64_t) available * 65536 % frames) : 0;
	unsigned remainder = 0;
	unsigned index;

	for (index = 0; index < frames; index++) {
		int32_t value = 0;
		if (available) {
			unsigned first = (unsigned) (position >> 16);
			unsigned second = first + 1 < available ? first + 1 : first;
			int32_t weight = (int32_t) (position & 0xffff);
			value = machine->fm_audio[first]
			        + (((machine->fm_audio[second] - machine->fm_audio[first]) * weight) >> 16);
		}
		value += machine->audio[index * 2];
		machine->audio[index * 2] = (int16_t) (value > 32767 ? 32767 : value < -32768 ? -32768 : value);
		machine->audio[index * 2 + 1] = machine->audio[index * 2];
		position += advance;
		remainder += advance_remainder;
		if (remainder >= frames) {
			position++;
			remainder -= frames;
		}
	}
}

void karashi_sms_run_frame(struct karashi_sms *machine) {
	uint64_t target = machine->cpu.cycles;
	unsigned line;

	if (!machine->game_gear && machine->pause && !machine->pause_seen) karashi_z80_nmi(&machine->cpu);
	machine->pause_seen = machine->pause;

	machine->audio_frames = 0;
	machine->fm_frames = 0;
	machine->phaser_latched = 0;
	for (line = 0; line < frame_lines(machine); line++) {
		machine->line_start = target;
		karashi_smsvdp_begin_line(
			&machine->vdp, line,
			line < KARASHI_SMSVDP_MAX_HEIGHT ? machine->video + line * KARASHI_SMSVDP_WIDTH : NULL
		);

		machine->line = line;
		machine->line_event = target + KARASHI_SMS_LINE_EVENT_CYCLE;
		machine->line_event_pending = true;
		run_until(machine, machine->line_event);
		line_event(machine);
		target += KARASHI_SEGA8_LINE_CYCLES;
		run_until(machine, target);
		if (phaser_sees(machine, 0, true)) phaser_latch(machine, 0);
		else if (phaser_sees(machine, 1, true)) phaser_latch(machine, 1);

		sound_sync(machine, target);
	}
	if (machine->fm_present) mix_fm(machine);
}

bool karashi_sms_save(const struct karashi_sms *machine, struct karashi_state_writer *writer) {
	unsigned port;

	karashi_z80_save(&machine->cpu, writer);
	karashi_smsvdp_save(&machine->vdp, writer);
	karashi_sn76489_save(&machine->psg, writer);
	karashi_state_write_bytes(writer, machine->ram, sizeof(machine->ram));
	karashi_state_write_bytes(writer, machine->cart_ram, sizeof(machine->cart_ram));
	karashi_state_write_bytes(writer, machine->pages, sizeof(machine->pages));
	karashi_state_write_u8(writer, machine->ram_control);
	karashi_state_write_bool(writer, machine->eeprom_enabled);
	karashi_eeprom93_save(&machine->eeprom, writer);
	karashi_state_write_u8(writer, machine->fm_control);
	karashi_ym2413_save(&machine->fm, writer);
	karashi_state_write_u8(writer, machine->memory_control);
	karashi_state_write_u8(writer, machine->mapper);
	karashi_state_write_bytes(writer, machine->banks8, sizeof(machine->banks8));
	karashi_state_write_bool(writer, machine->sega_paging_seen);
	karashi_state_write_u8(writer, machine->io_control);
	karashi_state_write_bool(writer, machine->pause_seen);
	for (port = 0; port < 2; port++) {
		karashi_state_write_u16(writer, (uint16_t) machine->sports_move[port][0]);
		karashi_state_write_u16(writer, (uint16_t) machine->sports_move[port][1]);
		karashi_state_write_bytes(writer, machine->sports_position[port], 2);
		karashi_state_write_bytes(writer, machine->sports_latch[port], 2);
		karashi_state_write_u8(writer, machine->sports_index[port]);
		karashi_state_write_u64(writer, machine->sports_edge[port]);
	}
	karashi_state_write_u64(writer, machine->sound_clock);
	karashi_state_write_u64(writer, machine->fm_clock);
	return karashi_state_write_u32(writer, machine->audio_clock);
}

bool karashi_sms_load(struct karashi_sms *machine, struct karashi_state_reader *reader) {
	bool loaded = karashi_z80_load(&machine->cpu, reader) && karashi_smsvdp_load(&machine->vdp, reader)
	              && karashi_sn76489_load(&machine->psg, reader);
	unsigned port;

	karashi_state_read_bytes(reader, machine->ram, sizeof(machine->ram));
	karashi_state_read_bytes(reader, machine->cart_ram, sizeof(machine->cart_ram));
	karashi_state_read_bytes(reader, machine->pages, sizeof(machine->pages));
	karashi_state_read_u8(reader, &machine->ram_control);
	karashi_state_read_bool(reader, &machine->eeprom_enabled);
	karashi_eeprom93_load(&machine->eeprom, reader);
	machine->eeprom_enabled = machine->eeprom_enabled && machine->eeprom_present;
	karashi_state_read_u8(reader, &machine->fm_control);
	karashi_ym2413_load(&machine->fm, reader);
	machine->fm_control &= 3;
	karashi_state_read_u8(reader, &machine->memory_control);
	karashi_state_read_u8(reader, &machine->mapper);
	karashi_state_read_bytes(reader, machine->banks8, sizeof(machine->banks8));
	karashi_state_read_bool(reader, &machine->sega_paging_seen);
	if (machine->mapper > KARASHI_SMS_MAPPER_MSX) machine->mapper = machine->base_mapper;
	karashi_state_read_u8(reader, &machine->io_control);
	karashi_state_read_bool(reader, &machine->pause_seen);
	for (port = 0; port < 2; port++) {
		uint16_t move[2] = {0, 0};
		karashi_state_read_u16(reader, &move[0]);
		karashi_state_read_u16(reader, &move[1]);
		machine->sports_move[port][0] = (int16_t) move[0];
		machine->sports_move[port][1] = (int16_t) move[1];
		karashi_state_read_bytes(reader, machine->sports_position[port], 2);
		karashi_state_read_bytes(reader, machine->sports_latch[port], 2);
		karashi_state_read_u8(reader, &machine->sports_index[port]);
		karashi_state_read_u64(reader, &machine->sports_edge[port]);
	}
	loaded = karashi_state_read_u64(reader, &machine->sound_clock) && loaded;
	loaded = karashi_state_read_u64(reader, &machine->fm_clock) && loaded;
	if (!karashi_state_read_u32(reader, &machine->audio_clock) || !loaded) return false;
	machine->audio_clock %= cpu_clock(machine);
	map_rom(machine);
	update_irq(machine);
	return true;
}
