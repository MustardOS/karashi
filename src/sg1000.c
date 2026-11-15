#include "sg1000.h"

#include <stdbool.h>
#include <string.h>

static uint8_t memory_read(void *context, uint16_t address) {
	struct karashi_sg1000 *machine = context;

	if (address >= 0xc000) {
		if (machine->msx_layout) return machine->work_ram[address & 0x1fff];
		if (machine->sc3000) return machine->work_ram[address & 0x07ff];
		return machine->ram[address & 0x03ff];
	}
	if (machine->msx_layout && address >= 0x2000 && address < 0x4000) return machine->expansion_ram[address & 0x1fff];
	if (address < machine->rom_size) return machine->rom[address];
	if (address >= 0x8000) return machine->cart_ram[address & (KARASHI_SG1000_CART_RAM_SIZE - 1)];
	return 0xff;
}

static void memory_write(void *context, uint16_t address, uint8_t value) {
	struct karashi_sg1000 *machine = context;

	if (address >= 0xc000) {
		if (machine->msx_layout) machine->work_ram[address & 0x1fff] = value;
		else if (machine->sc3000) machine->work_ram[address & 0x07ff] = value;
		else machine->ram[address & 0x03ff] = value;
	} else if (machine->msx_layout && address >= 0x2000 && address < 0x4000) {
		machine->expansion_ram[address & 0x1fff] = value;
	} else if (address >= 0x8000 && address >= machine->rom_size) {
		machine->cart_ram[address & (KARASHI_SG1000_CART_RAM_SIZE - 1)] = value;
	}
}

static uint8_t controller_one(const struct karashi_sg1000 *machine) {
	uint8_t value = 0xff;

	value &= (uint8_t) ~(machine->controller[0] & 0x3f);
	value &= (uint8_t) ~((machine->controller[1] & 0x03) << 6);
	return value;
}

static uint8_t controller_two(const struct karashi_sg1000 *machine) {
	return (uint8_t) (0xf0 | (~(machine->controller[1] >> 2) & 0x0f));
}

static uint16_t sc3000_matrix(const struct karashi_sg1000 *machine) {
	unsigned column = machine->ppi_port_c & 7;
	uint16_t value = machine->keyboard[column];

	if (column != 7) return value;
	value |= machine->controller[0];
	value |= (uint16_t) (machine->controller[1] & 0x03) << 6;
	value |= (uint16_t) (machine->controller[1] >> 2 & 0x0f) << 8;
	return value;
}

static uint8_t io_read(void *context, uint16_t port) {
	struct karashi_sg1000 *machine = context;
	uint8_t decoded = (uint8_t) port;

	if ((decoded & 0xc1) == 0x80) return karashi_tms9918_read_data(&machine->vdp);
	if ((decoded & 0xc1) == 0x81) return karashi_tms9918_read_status(&machine->vdp);
	if ((decoded & 0xc0) != 0xc0) return 0xff;
	if (machine->sc3000) {
		switch (decoded & 3) {
			case 0: return (uint8_t) ~sc3000_matrix(machine);
			case 1: return (uint8_t) (0xf0 | (~sc3000_matrix(machine) >> 8 & 0x0f));
			case 2: return machine->ppi_port_c;
			default: return 0xff;
		}
	}
	return decoded & 1 ? controller_two(machine) : controller_one(machine);
}

static void io_write(void *context, uint16_t port, uint8_t value) {
	struct karashi_sg1000 *machine = context;
	uint8_t decoded = (uint8_t) port;

	if ((decoded & 0xc1) == 0x80) karashi_tms9918_write_data(&machine->vdp, value);
	else if ((decoded & 0xc1) == 0x81) karashi_tms9918_write_control(&machine->vdp, value);
	else if ((decoded & 0xc0) == 0x40) karashi_sn76489_write(&machine->psg, value);
	else if (machine->sc3000 && (decoded & 0xc3) == 0xc2) machine->ppi_port_c = value;
	else if (machine->sc3000 && (decoded & 0xc3) == 0xc3) {
		if (value & 0x80) {
			machine->ppi_control = value;
			machine->ppi_port_c = 0;
		} else {
			uint8_t mask = (uint8_t) (1u << ((value >> 1) & 7));
			if (value & 1) machine->ppi_port_c |= mask;
			else machine->ppi_port_c &= (uint8_t) ~mask;
		}
	}
}

static bool msx_layout(const uint8_t *rom, size_t rom_size) {
	size_t index;

	if (!rom || rom_size < 0x4000) return false;
	for (index = 0x2001; index < 0x4000; index++) {
		if (rom[index] != rom[0x2000]) return false;
	}
	return true;
}

void karashi_sg1000_init(struct karashi_sg1000 *machine, const uint8_t *rom, size_t rom_size, bool sc3000) {
	memset(machine, 0, sizeof(*machine));
	machine->rom = rom;
	machine->rom_size = rom_size;
	machine->sc3000 = sc3000;
	karashi_sg1000_reset(machine);
}

void karashi_sg1000_reset(struct karashi_sg1000 *machine) {
	struct karashi_z80_bus bus;
	const uint8_t *rom = machine->rom;
	size_t rom_size = machine->rom_size;
	bool sc3000 = machine->sc3000;
	uint8_t cart_ram[KARASHI_SG1000_CART_RAM_SIZE];

	memcpy(cart_ram, machine->cart_ram, sizeof(cart_ram));
	memset(machine, 0, sizeof(*machine));
	memcpy(machine->cart_ram, cart_ram, sizeof(cart_ram));
	machine->rom = rom;
	machine->rom_size = rom_size;
	machine->sc3000 = sc3000;
	machine->msx_layout = !sc3000 && msx_layout(rom, rom_size);
	machine->ppi_control = 0x9b;
	bus.context = machine;
	bus.read = memory_read;
	bus.write = memory_write;
	bus.io_read = io_read;
	bus.io_write = io_write;
	karashi_z80_init(&machine->cpu, &bus);
	karashi_tms9918_reset(&machine->vdp);
	karashi_sn76489_reset(&machine->psg, KARASHI_SEGA8_CPU_CLOCK);
}

void karashi_sg1000_set_controller(struct karashi_sg1000 *machine, unsigned port, uint8_t state) {
	if (port < 2) machine->controller[port] = state & 0x3f;
}

void karashi_sg1000_set_pause(struct karashi_sg1000 *machine, bool pressed) {
	machine->pause = pressed;
}

void karashi_sg1000_clear_keys(struct karashi_sg1000 *machine) {
	memset(machine->keyboard, 0, sizeof(machine->keyboard));
}

void karashi_sg1000_set_key(struct karashi_sg1000 *machine, unsigned column, unsigned row, bool pressed) {
	uint16_t mask;

	if (column > 7 || row > 11) return;
	mask = (uint16_t) (1u << row);
	if (pressed) machine->keyboard[column] |= mask;
	else machine->keyboard[column] &= (uint16_t) ~mask;
}

void karashi_sg1000_run_frame(struct karashi_sg1000 *machine) {
	uint64_t target = machine->cpu.cycles;
	unsigned line;

	if (machine->pause && !machine->pause_seen) karashi_z80_nmi(&machine->cpu);
	machine->pause_seen = machine->pause;

	machine->audio_frames = 0;
	for (line = 0; line < KARASHI_SEGA8_LINES; line++) {
		unsigned frames;

		if (line < KARASHI_TMS9918_HEIGHT) {
			karashi_tms9918_render_line(&machine->vdp, line, machine->video + line * KARASHI_TMS9918_WIDTH);
		} else if (line == KARASHI_TMS9918_HEIGHT) {
			karashi_tms9918_vblank(&machine->vdp);
		}

		target += KARASHI_SEGA8_LINE_CYCLES;
		while (machine->cpu.cycles < target) {
			karashi_z80_irq(&machine->cpu, karashi_tms9918_interrupt(&machine->vdp));
			karashi_z80_step(&machine->cpu);
		}

		machine->audio_clock += KARASHI_SEGA8_SAMPLE_RATE * KARASHI_SEGA8_LINE_CYCLES;
		frames = machine->audio_clock / KARASHI_SEGA8_CPU_CLOCK;
		machine->audio_clock %= KARASHI_SEGA8_CPU_CLOCK;
		karashi_sn76489_generate(
			&machine->psg, machine->audio + machine->audio_frames * 2, frames, KARASHI_SEGA8_SAMPLE_RATE
		);
		machine->audio_frames += frames;
	}
}

bool karashi_sg1000_save(const struct karashi_sg1000 *machine, struct karashi_state_writer *writer) {
	karashi_z80_save(&machine->cpu, writer);
	karashi_tms9918_save(&machine->vdp, writer);
	karashi_sn76489_save(&machine->psg, writer);
	karashi_state_write_bytes(writer, machine->ram, sizeof(machine->ram));
	karashi_state_write_bytes(writer, machine->cart_ram, sizeof(machine->cart_ram));
	karashi_state_write_bytes(writer, machine->expansion_ram, sizeof(machine->expansion_ram));
	karashi_state_write_bytes(writer, machine->work_ram, sizeof(machine->work_ram));
	karashi_state_write_bool(writer, machine->pause_seen);
	karashi_state_write_u8(writer, machine->ppi_port_c);
	karashi_state_write_u8(writer, machine->ppi_control);
	return karashi_state_write_u32(writer, machine->audio_clock);
}

bool karashi_sg1000_load(struct karashi_sg1000 *machine, struct karashi_state_reader *reader) {
	bool loaded = karashi_z80_load(&machine->cpu, reader) && karashi_tms9918_load(&machine->vdp, reader)
	              && karashi_sn76489_load(&machine->psg, reader);

	karashi_state_read_bytes(reader, machine->ram, sizeof(machine->ram));
	karashi_state_read_bytes(reader, machine->cart_ram, sizeof(machine->cart_ram));
	karashi_state_read_bytes(reader, machine->expansion_ram, sizeof(machine->expansion_ram));
	karashi_state_read_bytes(reader, machine->work_ram, sizeof(machine->work_ram));
	karashi_state_read_bool(reader, &machine->pause_seen);
	karashi_state_read_u8(reader, &machine->ppi_port_c);
	karashi_state_read_u8(reader, &machine->ppi_control);
	if (!karashi_state_read_u32(reader, &machine->audio_clock) || !loaded) return false;
	machine->audio_clock %= KARASHI_SEGA8_CPU_CLOCK;
	return true;
}
