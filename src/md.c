#include "md.h"

#include <string.h>

#define PAD_TIMEOUT_MCLK 80540
#define PAD_LATENCY_MCLK 1204

static uint64_t now(const struct karashi_md *machine) {
	return machine->cpu.cycles * 7;
}

static unsigned line_position(const struct karashi_md *machine) {
	uint64_t time = now(machine);
	uint64_t offset = time > machine->line_start ? time - machine->line_start : 0;
	return offset < KARASHI_MD_LINE_MCLK ? (unsigned) offset : KARASHI_MD_LINE_MCLK - 1;
}

static void update_irq(struct karashi_md *machine) {
	karashi_m68k_set_irq(&machine->cpu, karashi_mdvdp_irq_level(&machine->vdp));
}

static void fm_sync(struct karashi_md *machine, uint64_t target) {
	unsigned frames;

	if (target <= machine->fm_clock) return;
	frames = (unsigned) ((target - machine->fm_clock) / KARASHI_MD_FM_MCLK);
	if (!frames) return;
	machine->fm_clock += (uint64_t) frames * KARASHI_MD_FM_MCLK;
	if (frames > KARASHI_MD_FM_CAPACITY - machine->fm_frames) {
		unsigned kept = KARASHI_MD_FM_CAPACITY - machine->fm_frames;
		karashi_ym2612_generate(&machine->ym, machine->fm + machine->fm_frames * 2, kept);
		karashi_ym2612_generate(&machine->ym, NULL, frames - kept);
		machine->fm_frames = KARASHI_MD_FM_CAPACITY;
		return;
	}
	karashi_ym2612_generate(&machine->ym, machine->fm + machine->fm_frames * 2, frames);
	machine->fm_frames += frames;
}

static void psg_sync(struct karashi_md *machine, uint64_t target) {
	uint64_t elapsed;
	uint64_t scaled;
	unsigned frames;

	if (target <= machine->psg_clock) return;
	elapsed = target - machine->psg_clock;
	scaled = machine->audio_clock + elapsed * KARASHI_MD_SAMPLE_RATE;
	frames = (unsigned) (scaled / machine->master_clock);
	machine->audio_clock = (uint32_t) (scaled % machine->master_clock);
	machine->psg_clock = target;
	if (frames > KARASHI_MD_AUDIO_CAPACITY - machine->audio_frames) {
		frames = KARASHI_MD_AUDIO_CAPACITY - machine->audio_frames;
	}
	karashi_sn76489_generate(
		&machine->psg, machine->audio + machine->audio_frames * 2, frames, KARASHI_MD_SAMPLE_RATE
	);
	machine->audio_frames += frames;
}

static void z80_sync(struct karashi_md *machine, uint64_t target) {
	if (!machine->z80_reset_asserted && !machine->z80_bus_requested) {
		while (machine->z80.cycles * 15 < target) karashi_z80_step(&machine->z80);
	} else if (machine->z80.cycles * 15 < target) machine->z80.cycles = (target + 14) / 15;
}

static bool sram_index(const struct karashi_md *machine, uint32_t address, uint32_t *index) {
	uint32_t offset;

	if (!machine->sram_present || !machine->sram_enabled || address < machine->sram_start
	    || address > machine->sram_end) return false;
	if (machine->sram_mode == KARASHI_MD_SRAM_EVEN && (address & 1)) return false;
	if (machine->sram_mode == KARASHI_MD_SRAM_ODD && !(address & 1)) return false;
	offset = address - machine->sram_start;
	if (machine->sram_mode != KARASHI_MD_SRAM_BOTH) offset >>= 1;
	*index = offset % machine->sram_size;
	return true;
}

static bool sram_window(const struct karashi_md *machine, uint32_t address) {
	return machine->sram_present && machine->sram_enabled && address >= machine->sram_start
	       && address <= machine->sram_end;
}

static void set_sram(
	struct karashi_md *machine, uint32_t start, uint32_t end, enum karashi_md_sram_mode mode
) {
	uint32_t span = end - start;

	machine->sram_start = start;
	machine->sram_end = end;
	machine->sram_mode = (uint8_t) mode;
	machine->sram_size = mode == KARASHI_MD_SRAM_BOTH ? span + 1 : span / 2 + 1;
	machine->sram_present = true;
	machine->sram_persistent = true;
}

static uint8_t rom_byte(const struct karashi_md *machine, uint32_t address) {
	uint32_t index;

	if (machine->tmss_mapped && address < 0x400000) return machine->tmss[address % machine->tmss_size];
	if (sram_index(machine, address, &index)) return machine->sram[index];
	if (machine->mapper == KARASHI_MD_MAPPER_SF004 && address < 0x200000) {
		address = ((uint32_t) machine->rom_bank[0] << 18)
		          + (machine->sf004_linear ? address : address & 0x3ffff);
	} else if (machine->mapper == KARASHI_MD_MAPPER_T5740 && address < 0x400000) {
		if ((address & 0xffff00) == 0x018100) address &= 0x7fff;
		else address = (uint32_t) machine->rom_bank[address >> 19] << 19 | (address & 0x7ffff);
	} else if (machine->mapper == KARASHI_MD_MAPPER_SSF2) {
		address = (uint32_t) machine->rom_bank[address >> 19] << 19 | (address & 0x7ffff);
	}
	return address < machine->rom_size ? machine->rom[address] : 0xff;
}

static uint16_t rom_checksum(const struct karashi_md *machine) {
	uint16_t checksum = 0;
	size_t address;

	for (address = 0x200; address + 1 < machine->rom_size; address += 2) {
		checksum = (uint16_t) (checksum + ((uint16_t) machine->rom[address] << 8 | machine->rom[address + 1]));
	}
	return checksum;
}

static uint8_t pad_read(struct karashi_md *machine, unsigned port) {
	uint8_t control = port < 2 ? machine->io_control[port] : 0x40;
	uint8_t data = port < 2 ? machine->io_data[port] : machine->jcart_th ? 0x40 : 0;
	uint16_t pad = machine->controller[port];
	bool th = control & 0x40 ? (data & 0x40) != 0 : now(machine) >= machine->pad_latency[port];
	unsigned phase = 0;
	uint8_t pins;

	if (machine->pad_type[port] == KARASHI_MD_PAD_NONE) pins = 0x7f;
	else {
		if (machine->pad_type[port] == KARASHI_MD_PAD_SIX) {
			if (now(machine) - machine->pad_time[port] > PAD_TIMEOUT_MCLK) machine->pad_phase[port] = th ? 0 : 1;
			phase = machine->pad_phase[port];
		}
		if (th) {
			pins = 0x7f;
			if (phase == 6) {
				if (pad & KARASHI_MD_PAD_Z) pins &= (uint8_t) ~0x01;
				if (pad & KARASHI_MD_PAD_Y) pins &= (uint8_t) ~0x02;
				if (pad & KARASHI_MD_PAD_X) pins &= (uint8_t) ~0x04;
				if (pad & KARASHI_MD_PAD_MODE) pins &= (uint8_t) ~0x08;
			} else {
				if (pad & KARASHI_MD_PAD_UP) pins &= (uint8_t) ~0x01;
				if (pad & KARASHI_MD_PAD_DOWN) pins &= (uint8_t) ~0x02;
				if (pad & KARASHI_MD_PAD_LEFT) pins &= (uint8_t) ~0x04;
				if (pad & KARASHI_MD_PAD_RIGHT) pins &= (uint8_t) ~0x08;
			}
			if (pad & KARASHI_MD_PAD_B) pins &= (uint8_t) ~0x10;
			if (pad & KARASHI_MD_PAD_C) pins &= (uint8_t) ~0x20;
		} else {
			pins = 0x33;
			if (phase == 5) pins = 0x30;
			else if (phase == 7) pins = 0x3f;
			else {
				if (pad & KARASHI_MD_PAD_UP) pins &= (uint8_t) ~0x01;
				if (pad & KARASHI_MD_PAD_DOWN) pins &= (uint8_t) ~0x02;
			}
			if (pad & KARASHI_MD_PAD_A) pins &= (uint8_t) ~0x10;
			if (pad & KARASHI_MD_PAD_START) pins &= (uint8_t) ~0x20;
		}
	}
	return (uint8_t) ((pins & ~control & 0x7f) | (data & (control | 0x80)));
}

static uint16_t jcart_read(struct karashi_md *machine) {
	uint16_t value = (uint16_t) (pad_read(machine, 2) | (uint16_t) (pad_read(machine, 3) & 0x3f) << 8);

	if (machine->eeprom_present && karashi_eeprom_read_sda(&machine->eeprom)) value |= 0x0080;
	return value;
}

static void jcart_write(struct karashi_md *machine, uint8_t value) {
	bool old_th = machine->jcart_th;
	bool new_th = (value & 1) != 0;
	unsigned port;

	for (port = 2; port < KARASHI_MD_CONTROLLER_PORTS; port++) {
		if (!old_th && new_th) machine->pad_phase[port] = (uint8_t) ((machine->pad_phase[port] + 1) & 7);
		if (old_th != new_th) machine->pad_time[port] = now(machine);
	}
	machine->jcart_th = new_th;
}

static void pad_write(struct karashi_md *machine, unsigned port, uint8_t data, uint8_t control) {
	bool old_th = machine->io_control[port] & 0x40 ? (machine->io_data[port] & 0x40) != 0 : true;
	bool output = (control & 0x40) != 0;
	bool new_th = output ? (data & 0x40) != 0 : true;
	uint64_t time = now(machine);

	machine->io_data[port] = data;
	machine->io_control[port] = control;
	if (!output) {
		if (!old_th) machine->pad_latency[port] = time + PAD_LATENCY_MCLK;
		return;
	}
	machine->pad_latency[port] = 0;
	if (old_th == new_th) return;
	if (time - machine->pad_time[port] > PAD_TIMEOUT_MCLK) machine->pad_phase[port] = old_th ? 0 : 1;
	machine->pad_phase[port] = (machine->pad_phase[port] + 1) & 7;
	if ((machine->pad_phase[port] & 1) != (new_th ? 0u : 1u)) machine->pad_phase[port] = new_th ? 0 : 1;
	machine->pad_time[port] = time;
}

static uint8_t io_read(struct karashi_md *machine, uint32_t address) {
	switch ((address >> 1) & 15) {
		case 0: return machine->version;
		case 1: return pad_read(machine, 0);
		case 2: return pad_read(machine, 1);
		case 3: return (uint8_t) (0x7f | (machine->io_data[2] & 0x80));
		case 4: return machine->io_control[0];
		case 5: return machine->io_control[1];
		case 6: return machine->io_control[2];
		case 7:
		case 10:
		case 13: return 0xff;
		default: return 0;
	}
}

static void io_write(struct karashi_md *machine, uint32_t address, uint8_t value) {
	switch ((address >> 1) & 15) {
		case 1: pad_write(machine, 0, value, machine->io_control[0]); break;
		case 2: pad_write(machine, 1, value, machine->io_control[1]); break;
		case 3: machine->io_data[2] = value; break;
		case 4: pad_write(machine, 0, machine->io_data[0], value); break;
		case 5: pad_write(machine, 1, machine->io_data[1], value); break;
		case 6: machine->io_control[2] = value; break;
		default: break;
	}
}

static uint8_t sound_read(struct karashi_md *machine, uint16_t address, uint64_t time) {
	if (address < 0x4000) return machine->z80_ram[address & (KARASHI_MD_Z80_RAM_SIZE - 1)];
	if (address < 0x6000) {
		fm_sync(machine, time);
		return karashi_ym2612_read_at(&machine->ym, time);
	}
	return 0xff;
}

static void sound_write(struct karashi_md *machine, uint16_t address, uint8_t value, uint64_t time) {
	if (address < 0x4000) machine->z80_ram[address & (KARASHI_MD_Z80_RAM_SIZE - 1)] = value;
	else if (address < 0x6000) {
		fm_sync(machine, time);
		karashi_ym2612_write_at(&machine->ym, address & 3, value, time);
	} else if (address < 0x6100) machine->z80_bank = (uint16_t) ((machine->z80_bank >> 1) | ((value & 1) << 8));
	else if (address == 0x7f11) {
		psg_sync(machine, time);
		karashi_sn76489_write(&machine->psg, value);
	}
}

static uint16_t vdp_read(struct karashi_md *machine, uint32_t address) {
	switch (address & 0x1c) {
		case 0x00: return karashi_mdvdp_read_data(&machine->vdp);
		case 0x04:
			return karashi_mdvdp_read_status_at(&machine->vdp, line_position(machine), now(machine));
		case 0x08:
		case 0x0c: return karashi_mdvdp_hv_counter_at(&machine->vdp, line_position(machine));
		default: return 0xffff;
	}
}

static void vdp_write(struct karashi_md *machine, uint32_t address, uint16_t value) {
	unsigned position = line_position(machine);
	uint64_t master_clock = now(machine);

	switch (address & 0x1c) {
		case 0x00:
			machine->cpu.cycles += karashi_mdvdp_write_data_at(
				&machine->vdp, value, position, master_clock
			);
			break;
		case 0x04:
			machine->cpu.cycles += karashi_mdvdp_write_control_at(
				&machine->vdp, value, position, master_clock
			);
			update_irq(machine);
			break;
		case 0x10:
		case 0x14:
			psg_sync(machine, master_clock);
			karashi_sn76489_write(&machine->psg, (uint8_t) value);
			break;
		default: break;
	}
}

static bool vdp_address(uint32_t address) {
	return (address & 0xe700e0) == 0xc00000;
}

static uint8_t open_bus(const struct karashi_md *machine, uint32_t address) {
	return rom_byte(machine, machine->cpu.pc + (address & 1));
}

static bool mars_access(struct karashi_md *machine, uint32_t address) {
	if (!machine->mars_present || !karashi_mars_owns(&machine->mars, address)) return false;
	if (address >= 0xa15100) karashi_mars_run(&machine->mars, now(machine));
	return true;
}

static bool eeprom_address(const struct karashi_md *machine, uint32_t address) {
	if (!machine->eeprom_present || address < 0x200000) return false;
	if (machine->eeprom_mapping == KARASHI_MD_EEPROM_SPI) return false;
	if (machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM) return address < 0x300000;
	if (machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART) return address >= 0x300000 && address < 0x400000;
	return address < 0x400000;
}

static bool eeprom_read_active(const struct karashi_md *machine, uint32_t address) {
	if (!eeprom_address(machine, address)) return false;
	if (machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART) return address >= 0x380000;
	return machine->eeprom_mapping != KARASHI_MD_EEPROM_ACCLAIM || machine->eeprom_read_enabled;
}

static uint8_t eeprom_read(const struct karashi_md *machine, uint32_t address) {
	uint8_t bit = machine->eeprom_mapping == KARASHI_MD_EEPROM_EA
	                      || machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART ? 7
	                   : machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM_WORD ? 1
	                                                                                 : 0;
	uint8_t value = rom_byte(machine, address);
	bool line = karashi_eeprom_read_sda(&machine->eeprom);

	if (!(address & 1) || (machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART && address < 0x380000)) return value;
	return (uint8_t) ((value & ~(1u << bit)) | (line ? 1u << bit : 0));
}

static uint16_t eeprom_read16(const struct karashi_md *machine) {
	uint8_t bit = machine->eeprom_mapping == KARASHI_MD_EEPROM_EA
	                      || machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART ? 7
	                   : machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM_WORD ? 1
	                                                                                 : 0;
	return (uint16_t) (karashi_eeprom_read_sda(&machine->eeprom) ? 1u << bit : 0);
}

static void eeprom_write(struct karashi_md *machine, uint32_t address, uint8_t value) {
	bool scl = machine->eeprom.scl;
	bool sda = machine->eeprom.sda;

	if (machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM) {
		if (address & 1) sda = (value & 1) != 0;
		else scl = (value & 1) != 0;
	} else if (machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM_WORD
	           || machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART) {
		if (machine->eeprom_mapping == KARASHI_MD_EEPROM_J_CART && address >= 0x380000) return;
		scl = (value & 2) != 0;
		sda = (value & 1) != 0;
	} else {
		uint8_t scl_bit = machine->eeprom_mapping == KARASHI_MD_EEPROM_EA ? 6 : 1;
		uint8_t sda_bit = machine->eeprom_mapping == KARASHI_MD_EEPROM_EA ? 7 : 0;

		if (!(address & 1)) return;
		scl = (value & (1u << scl_bit)) != 0;
		sda = (value & (1u << sda_bit)) != 0;
	}
	karashi_eeprom_write_lines(&machine->eeprom, scl, sda, machine->sram);
}

static void update_maps(struct karashi_md *machine) {
	struct karashi_m68k_bus *bus = &machine->cpu.bus;
	bool plain = machine->mapper == KARASHI_MD_MAPPER_STANDARD && !machine->tmss_mapped && !machine->mars_present;
	unsigned page;

	for (page = 0; page < 256; page++) {
		uint32_t start = page << 16;
		bool rom = plain && start + 0x10000 <= machine->rom_size && start < 0x400000
		           && !(machine->sram_enabled && start + 0xffff >= machine->sram_start && start <= machine->sram_end)
		           && !(machine->jcart_present && start >= 0x380000)
		           && !(eeprom_read_active(machine, start));
		bus->read_map[page] = rom ? machine->rom + start : NULL;
		bus->write_map[page] = NULL;
	}
	for (page = 0xe0; page < 0x100; page++) {
		bus->read_map[page] = machine->work_ram;
		bus->write_map[page] = machine->work_ram;
	}
}

void karashi_md_rebind(struct karashi_md *machine) {
	machine->cpu.bus.context = machine;
	machine->z80.bus.context = machine;
	machine->vdp.dma_context = machine;
	update_maps(machine);
	if (machine->mars_present) karashi_mars_rebind(&machine->mars);
}

static uint8_t memory_read(void *context, uint32_t address) {
	struct karashi_md *machine = context;
	uint32_t sram_offset;

	if (mars_access(machine, address)) return karashi_mars_read8(&machine->mars, address);

	if (machine->mapper == KARASHI_MD_MAPPER_SF004
	    && (address == 0xa13001 || address == 0xa13005 || address == 0xa13009)) return 0;
	if (machine->jcart_present && address >= 0x380000 && address < 0x400000) {
		uint16_t value = jcart_read(machine);
		return address & 1 ? (uint8_t) value : (uint8_t) (value >> 8);
	}
	if (eeprom_read_active(machine, address)) return eeprom_read(machine, address);
	if (sram_index(machine, address, &sram_offset)) return machine->sram[sram_offset];
	if (machine->mapper == KARASHI_MD_MAPPER_SF004 && address >= 0x200000 && address < 0x400000
	    && !(machine->sram_enabled && address <= machine->sram_end)) return open_bus(machine, address);
	if (address < 0x400000) return rom_byte(machine, address);
	if (address >= 0xe00000) return machine->work_ram[address & 0xffff];
	if (address >= 0xa00000 && address < 0xa10000) {
		if (!machine->z80_bus_requested) return 0xff;
		return sound_read(machine, address & 0x7fff, now(machine));
	}
	if (address >= 0xa10000 && address < 0xa10020) return io_read(machine, address);
	if ((address & 0xffff00) == 0xa11100) {
		if (address & 1) return open_bus(machine, address);
		return (uint8_t) ((open_bus(machine, address) & 0xfe) | (machine->z80_bus_requested ? 0 : 1));
	}
	if ((address & 0xffff00) == 0xa11200) return open_bus(machine, address);
	if (address == 0xa13009 && machine->eeprom_mapping == KARASHI_MD_EEPROM_SPI) {
		return (uint8_t) (karashi_spi_eeprom_read(&machine->spi_eeprom) ? 1 : 0);
	}
	if (vdp_address(address)) {
		uint16_t value = vdp_read(machine, address);
		return address & 1 ? (uint8_t) value : (uint8_t) (value >> 8);
	}
	return open_bus(machine, address);
}

static void bus_request(struct karashi_md *machine, bool requested) {
	z80_sync(machine, now(machine));
	machine->z80_bus_requested = requested;
}

static void bus_reset(struct karashi_md *machine, bool asserted) {
	z80_sync(machine, now(machine));
	if (asserted && !machine->z80_reset_asserted) {
		fm_sync(machine, now(machine));
		karashi_z80_reset(&machine->z80);
		machine->z80.cycles = (now(machine) + 14) / 15;
		karashi_ym2612_reset(&machine->ym);
	}
	machine->z80_reset_asserted = asserted;
}

static void memory_write(void *context, uint32_t address, uint8_t value) {
	struct karashi_md *machine = context;
	uint32_t sram_offset;

	if (mars_access(machine, address)) {
		karashi_mars_write8(&machine->mars, address, value);
		return;
	}

	if (machine->mapper == KARASHI_MD_MAPPER_SF004 && address == 0x000d01) {
		machine->sram_enabled = (value & 0x80) != 0;
		update_maps(machine);
		return;
	}
	if (machine->jcart_present && address >= 0x380000 && address < 0x400000) {
		jcart_write(machine, value);
		return;
	}
	if (machine->mapper == KARASHI_MD_MAPPER_T5740 && address >= 0xa13003 && address <= 0xa13007
	    && (address & 1)) {
		machine->rom_bank[(address - 0xa13003) / 2 + 5] = value & 0x0f;
		return;
	}
	if (machine->mapper == KARASHI_MD_MAPPER_T5740 && address == 0xa13009) {
		karashi_spi_eeprom_write(&machine->spi_eeprom, value, machine->sram);
		return;
	}
	if (machine->mapper == KARASHI_MD_MAPPER_SF004 && address == 0x000e01) {
		machine->sf004_linear = true;
		update_maps(machine);
		return;
	}
	if (machine->mapper == KARASHI_MD_MAPPER_SF004 && address == 0x000f01) {
		machine->rom_bank[0] = (value >> 4) & 7;
		machine->sf004_linear = false;
		update_maps(machine);
		return;
	}
	if (eeprom_address(machine, address)) {
		eeprom_write(machine, address, value);
		return;
	}
	if (address >= 0xe00000) {
		machine->work_ram[address & 0xffff] = value;
		return;
	}
	if (sram_index(machine, address, &sram_offset)) {
		if (!machine->sram_write_protected) machine->sram[sram_offset] = value;
		return;
	}
	if (sram_window(machine, address)) return;
	if (address < 0x400000) {
		return;
	}
	if (address >= 0xa00000 && address < 0xa10000) {
		if (machine->z80_bus_requested) sound_write(machine, address & 0x7fff, value, now(machine));
		return;
	}
	if (address >= 0xa10000 && address < 0xa10020) {
		if (address & 1) io_write(machine, address, value);
		return;
	}
	if (machine->tmss && address >= 0xa14000 && address < 0xa14004) machine->tmss_key[address & 3] = value;
	else if (machine->tmss && address == 0xa14101) {
		machine->tmss_mapped = !(value & 1);
		update_maps(machine);
	}
	else if (address == 0xa11100) bus_request(machine, (value & 1) != 0);
	else if (address == 0xa11200) bus_reset(machine, (value & 1) == 0);
	else if (address == 0xa130f1) {
		if (machine->sram_present) {
			if (machine->sram_start < machine->rom_size) machine->sram_enabled = (value & 1) != 0;
			machine->sram_write_protected = (value & 2) != 0;
		}
		update_maps(machine);
	} else if (address >= 0xa130f3 && address <= 0xa130ff && (address & 1)) {
		if (machine->mapper == KARASHI_MD_MAPPER_SSF2) machine->rom_bank[(address >> 1) & 7] = value & 0x3f;
	} else if (vdp_address(address)) {
		if ((address & 0x1c) >= 0x10) {
			if (address & 1) vdp_write(machine, address, value);
		} else vdp_write(machine, address, (uint16_t) (value << 8 | value));
	}
}

static uint16_t memory_read16(void *context, uint32_t address) {
	struct karashi_md *machine = context;
	uint32_t sram_offset;

	if (mars_access(machine, address)) return karashi_mars_read16(&machine->mars, address);

	if (address >= 0xe00000) {
		address &= 0xfffe;
		return (uint16_t) ((uint16_t) machine->work_ram[address] << 8 | machine->work_ram[address + 1]);
	}
	if (machine->jcart_present && address >= 0x380000 && address < 0x400000) return jcart_read(machine);
	if (eeprom_read_active(machine, address)) return eeprom_read16(machine);
	if (machine->sram_mode != KARASHI_MD_SRAM_BOTH
	    && (sram_index(machine, address, &sram_offset) || sram_index(machine, address + 1, &sram_offset))) {
		uint8_t value = machine->sram[sram_offset];
		return (uint16_t) ((uint16_t) value << 8 | value);
	}
	if (address + 1 < machine->rom_size && machine->mapper == KARASHI_MD_MAPPER_STANDARD && !machine->tmss_mapped
	    && !(machine->sram_enabled && address >= machine->sram_start)) {
		return (uint16_t) ((uint16_t) machine->rom[address] << 8 | machine->rom[address + 1]);
	}
	if (vdp_address(address)) return vdp_read(machine, address);
	return (uint16_t) ((uint16_t) memory_read(context, address) << 8 | memory_read(context, address + 1));
}

static void memory_write16(void *context, uint32_t address, uint16_t value) {
	struct karashi_md *machine = context;

	if (mars_access(machine, address)) {
		karashi_mars_write16(&machine->mars, address, value);
		return;
	}

	if (address >= 0xe00000) {
		address &= 0xfffe;
		machine->work_ram[address] = (uint8_t) (value >> 8);
		machine->work_ram[address + 1] = (uint8_t) value;
		return;
	}
	if (machine->jcart_present && address >= 0x380000 && address < 0x400000) {
		jcart_write(machine, (uint8_t) value);
		return;
	}
	if (vdp_address(address)) {
		vdp_write(machine, address, value);
		return;
	}
	if (eeprom_address(machine, address)) {
		if (machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM) {
			machine->eeprom_read_enabled = (value & 1) == 0;
			update_maps(machine);
		} else eeprom_write(machine, address | 1, (uint8_t) value);
		return;
	}
	memory_write(context, address, (uint8_t) (value >> 8));
	memory_write(context, address + 1, (uint8_t) value);
}

static void acknowledge(void *context, unsigned level) {
	struct karashi_md *machine = context;

	karashi_mdvdp_acknowledge(&machine->vdp, level);
	update_irq(machine);
}

static uint16_t dma_read(void *context, uint32_t address) {
	struct karashi_md *machine = context;
	uint32_t sram_offset;

	address &= 0xfffffe;
	if (address >= 0xe00000) {
		address &= 0xfffe;
		return (uint16_t) ((uint16_t) machine->work_ram[address] << 8 | machine->work_ram[address + 1]);
	}
	if (machine->sram_mode != KARASHI_MD_SRAM_BOTH
	    && (sram_index(machine, address, &sram_offset) || sram_index(machine, address + 1, &sram_offset))) {
		uint8_t value = machine->sram[sram_offset];
		return (uint16_t) ((uint16_t) value << 8 | value);
	}
	if (sram_window(machine, address) || address < 0x400000) {
		return (uint16_t) ((uint16_t) rom_byte(machine, address) << 8 | rom_byte(machine, address + 1));
	}
	return 0xffff;
}

static uint8_t z80_read(void *context, uint16_t address) {
	struct karashi_md *machine = context;
	uint32_t target;

	if (address < 0x8000) return sound_read(machine, address, machine->z80.cycles * 15);
	target = (uint32_t) machine->z80_bank << 15 | (address & 0x7fff);
	return memory_read(machine, target);
}

static void z80_write(void *context, uint16_t address, uint8_t value) {
	struct karashi_md *machine = context;
	uint32_t target;

	if (address < 0x8000) {
		sound_write(machine, address, value, machine->z80.cycles * 15);
		return;
	}
	target = (uint32_t) machine->z80_bank << 15 | (address & 0x7fff);
	memory_write(machine, target, value);
}

static uint8_t z80_io_read(void *context, uint16_t port) {
	(void) context;
	(void) port;
	return 0xff;
}

static void z80_io_write(void *context, uint16_t port, uint8_t value) {
	(void) context;
	(void) port;
	(void) value;
}

static uint32_t rom_long(const struct karashi_md *machine, uint32_t address) {
	if ((size_t) address + 3 >= machine->rom_size) return 0;
	return (uint32_t) machine->rom[address] << 24 | (uint32_t) machine->rom[address + 1] << 16
	       | (uint32_t) machine->rom[address + 2] << 8 | machine->rom[address + 3];
}

static enum karashi_md_region header_region(const struct karashi_md *machine) {
	/* European releases that refuse to run at 60 Hz although their header allows it; found by black-box comparison. */
	static const uint32_t pal_only[] = {0x28165bd1, 0x2737f92e, 0x90f5c2b7, 0xc0dce0e5, 0xa920c852, 0xf09353b4};
	bool usa = false;
	bool japan = false;
	bool europe = false;
	uint32_t checksum;
	unsigned index;

	if (machine->rom_size < 0x200) return KARASHI_MD_REGION_USA;
	checksum = karashi_state_crc32(machine->rom, machine->rom_size);
	for (index = 0; index < sizeof(pal_only) / sizeof(pal_only[0]); index++) {
		if (pal_only[index] == checksum) return KARASHI_MD_REGION_EUROPE;
	}
	if (memcmp(machine->rom + 0x1f0, "EUR", 3) == 0) return KARASHI_MD_REGION_EUROPE;
	for (index = 0; index < 3; index++) {
		uint8_t code = machine->rom[0x1f0 + index];
		if (code == 'U') usa = true;
		else if (code == 'J' || code == 'K') japan = true;
		else if (code == 'E') europe = true;
		else if (index == 0 && ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'F'))) {
			unsigned mask = code <= '9' ? (unsigned) (code - '0') : (unsigned) (code - 'A') + 10u;
			if (mask & 4) usa = true;
			if (mask & 1) japan = true;
			if (mask & 8) europe = true;
		} else if (code != ' ') break;
	}
	if (usa) return KARASHI_MD_REGION_USA;
	if (japan) return KARASHI_MD_REGION_JAPAN;
	if (europe) return KARASHI_MD_REGION_EUROPE;
	return KARASHI_MD_REGION_USA;
}

static void detect_cartridge(struct karashi_md *machine) {
	static const struct {
		const char *product;
		uint8_t mapping;
		uint16_t mask;
		uint16_t page;
		uint8_t address_bytes;
	} eeprom_products[] = {
		{"T-081326", KARASHI_MD_EEPROM_ACCLAIM_WORD, 0x00ff, 0x0003, 1},
		{"T-81033", KARASHI_MD_EEPROM_ACCLAIM_WORD, 0x00ff, 0x0003, 1},
		{"T-081276", KARASHI_MD_EEPROM_ACCLAIM, 0x00ff, 0x0007, 1},
		{"T-81406", KARASHI_MD_EEPROM_ACCLAIM, 0x01ff, 0x000f, 1},
		{"T-081586", KARASHI_MD_EEPROM_ACCLAIM, 0x07ff, 0x000f, 1},
		{"T-81476", KARASHI_MD_EEPROM_ACCLAIM, 0x1fff, 0x003f, 2},
		{"T-81576", KARASHI_MD_EEPROM_ACCLAIM, 0x1fff, 0x003f, 2},
		{"T-120106", KARASHI_MD_EEPROM_J_CART, 0x03ff, 0x000f, 1},
		{"T-120096", KARASHI_MD_EEPROM_J_CART, 0x07ff, 0x000f, 1},
		{"T-120146", KARASHI_MD_EEPROM_J_CART, 0x1fff, 0x003f, 2}
	};
	static const char *const sega_products[] = {
		"T-12046", "T-12053", "MK-1215", "MK-1228", "G-5538", "PR-1993", "G-4060", "00001211",
		"00004076", "G-4524", "00054503"
	};
	static const char *const ea_products[] = {"T-50176", "T-50396", "T-50446", "T-50516", "T-50606"};
	int header_sram_mode = -1;
	bool parallel_sram;
	bool volatile_internal_ram = machine->rom_size >= 0x1bc && memcmp(machine->rom + 0x1b0, "RA", 2) == 0
	                             && (rom_long(machine, 0x1b4) & 0xffffff) == 0xff0000;
	bool barkley_sram = machine->rom_size >= 0x18b && memcmp(machine->rom + 0x183, "T-119186", 8) == 0;
	bool triple_play_sram = machine->rom_size >= 0x18b
	                        && (memcmp(machine->rom + 0x183, "T-172026", 8) == 0
	                            || memcmp(machine->rom + 0x183, "T-172116", 8) == 0);
	bool sonic_three_knuckles = machine->rom_size == 0x400000 && machine->rom_size >= 0x161
	                             && memcmp(machine->rom + 0x150, "SONIC & KNUCKLES", 16) == 0;
	uint16_t header_checksum = machine->rom_size >= 0x190
	                               ? (uint16_t) ((uint16_t) machine->rom[0x18e] << 8 | machine->rom[0x18f])
	                               : 0;
	uint16_t calculated_checksum = header_checksum == 0x8104 ? rom_checksum(machine) : 0;
	bool xin_qigai_wangzi = header_checksum == 0x8104
	                        && (calculated_checksum == 0xaeaa || calculated_checksum == 0x8dba);
	unsigned index;

	if (machine->rom_size >= 0x1bc && memcmp(machine->rom + 0x1b0, "RA", 2) == 0) {
		if (machine->rom[0x1b2] == 0xa0 || machine->rom[0x1b2] == 0xe0) {
			header_sram_mode = KARASHI_MD_SRAM_BOTH;
		} else if (machine->rom[0x1b2] == 0xb0 || machine->rom[0x1b2] == 0xf0) {
			header_sram_mode = KARASHI_MD_SRAM_EVEN;
		} else if (machine->rom[0x1b2] == 0xb8 || machine->rom[0x1b2] == 0xf8) {
			header_sram_mode = KARASHI_MD_SRAM_ODD;
		}
	}
	parallel_sram = header_sram_mode >= 0 && rom_long(machine, 0x1b8) > rom_long(machine, 0x1b4) + 2;

	machine->mapper = machine->rom_size > 0x400000 ? KARASHI_MD_MAPPER_SSF2 : KARASHI_MD_MAPPER_STANDARD;
	if (machine->rom_size >= 0x186 && memcmp(machine->rom + 0x180, "SF-004", 6) == 0) {
		machine->mapper = KARASHI_MD_MAPPER_SF004;
	}
	if (machine->rom_size >= 0x18a && memcmp(machine->rom + 0x183, "T-5740", 6) == 0) {
		machine->mapper = KARASHI_MD_MAPPER_T5740;
	}
	machine->sf004_linear = false;
	machine->jcart_present = false;
	machine->eeprom_present = false;
	machine->eeprom_read_enabled = false;
	machine->eeprom_mapping = KARASHI_MD_EEPROM_NONE;
	machine->eeprom_size = 0;
	machine->eeprom_address_mask = 0;
	machine->eeprom_page_mask = 0;
	machine->eeprom_mode = KARASHI_EEPROM_AUTO;
	machine->eeprom_address_bytes = 0;
	if (machine->rom_size >= 0x1bc && memcmp(machine->rom + 0x1b0, "RA", 2) == 0 && machine->rom[0x1b3] == 0x40
	    && rom_long(machine, 0x1b4) == 0x200001 && rom_long(machine, 0x1b8) == 0x200001) {
		machine->eeprom_present = true;
		machine->eeprom_mapping = KARASHI_MD_EEPROM_SEGA;
		machine->eeprom_size = 128;
		machine->eeprom_address_mask = 0x7f;
		machine->eeprom_page_mask = 3;
		machine->eeprom_mode = KARASHI_EEPROM_WORD_ADDRESS;
	}
	if (machine->rom_size >= 0x190) {
		uint16_t checksum = header_checksum;

		if (memcmp(machine->rom + 0x183, "T-120096", 8) == 0
		    || memcmp(machine->rom + 0x183, "T-120066", 8) == 0
		    || memcmp(machine->rom + 0x183, "T-123456", 8) == 0
		    || (memcmp(machine->rom + 0x183, "XXXXXXXX", 8) == 0 && checksum == 0xdf39)
		    || (memcmp(machine->rom + 0x183, "00000000", 8) == 0
		        && ((rom_long(machine, 0) == 0x444e4c44 && (checksum == 0x168b || checksum == 0x165e))
		            || (rom_long(machine, 0) == 0xfffffffc && (checksum == 0x168b || checksum == 0x165e))))) {
			machine->jcart_present = true;
		}
		for (index = 0; index < sizeof(eeprom_products) / sizeof(eeprom_products[0]); index++) {
			size_t length = strlen(eeprom_products[index].product);
			if (memcmp(machine->rom + 0x183, eeprom_products[index].product, length) == 0) {
				machine->eeprom_present = true;
				machine->eeprom_mapping = eeprom_products[index].mapping;
				machine->eeprom_address_mask = eeprom_products[index].mask;
				machine->eeprom_page_mask = eeprom_products[index].page;
				machine->eeprom_address_bytes = eeprom_products[index].address_bytes;
				machine->eeprom_size = (uint32_t) eeprom_products[index].mask + 1;
				machine->eeprom_mode = KARASHI_EEPROM_DEVICE_ADDRESS;
				break;
			}
		}
		for (index = 0; index < sizeof(sega_products) / sizeof(sega_products[0]); index++) {
			size_t length = strlen(sega_products[index]);
			if (!parallel_sram && memcmp(machine->rom + 0x183, sega_products[index], length) == 0) {
				machine->eeprom_present = true;
				machine->eeprom_mapping = KARASHI_MD_EEPROM_SEGA;
				machine->eeprom_size = 128;
				machine->eeprom_address_mask = 0x7f;
				machine->eeprom_page_mask = 3;
				machine->eeprom_mode = KARASHI_EEPROM_WORD_ADDRESS;
				break;
			}
		}
		for (index = 0; index < sizeof(ea_products) / sizeof(ea_products[0]); index++) {
			size_t length = strlen(ea_products[index]);
			if (memcmp(machine->rom + 0x183, ea_products[index], length) == 0) {
				machine->eeprom_present = true;
				machine->eeprom_mapping = KARASHI_MD_EEPROM_EA;
				machine->eeprom_size = 128;
				machine->eeprom_address_mask = 0x7f;
				machine->eeprom_page_mask = 3;
				machine->eeprom_mode = KARASHI_EEPROM_WORD_ADDRESS;
				break;
			}
		}
		if (!machine->eeprom_present && memcmp(machine->rom + 0x183, "00000000", 8) == 0
		    && rom_long(machine, 0) == 0x444e4c44) {
			if (checksum == 0x168b || checksum == 0x165e) {
				machine->eeprom_present = true;
				machine->eeprom_mapping = KARASHI_MD_EEPROM_J_CART;
				machine->eeprom_address_mask = checksum == 0x168b ? 0x03ff : 0x07ff;
				machine->eeprom_page_mask = 0x000f;
				machine->eeprom_address_bytes = 1;
				machine->eeprom_size = (uint32_t) machine->eeprom_address_mask + 1;
				machine->eeprom_mode = KARASHI_EEPROM_DEVICE_ADDRESS;
			}
		}
	}
	if (machine->mapper == KARASHI_MD_MAPPER_T5740) {
		machine->eeprom_present = true;
		machine->eeprom_mapping = KARASHI_MD_EEPROM_SPI;
		machine->eeprom_size = KARASHI_SPI_EEPROM_SIZE;
	}
	for (index = 0; index < 8; index++) machine->rom_bank[index] = (uint8_t) index;
	machine->sram_present = false;
	machine->sram_persistent = false;
	machine->sram_size = 0;
	machine->sram_mode = KARASHI_MD_SRAM_BOTH;
	machine->sram_write_protected = false;
	if (machine->mapper == KARASHI_MD_MAPPER_SF004) {
		set_sram(machine, 0x200001, 0x203fff, KARASHI_MD_SRAM_ODD);
		machine->sram_end = 0x2fffff;
	} else if (!machine->eeprom_present && !volatile_internal_ram && header_sram_mode >= 0) {
		uint32_t start = rom_long(machine, 0x1b4) & 0xffffff;
		uint32_t end = rom_long(machine, 0x1b8) & 0xffffff;
		if (start >= 0x800000) {
			start = header_sram_mode == KARASHI_MD_SRAM_ODD ? 0x200001 : 0x200000;
			end = 0x20ffff;
		} else if (end < start) {
			end = start + KARASHI_MD_SRAM_SIZE - 1;
		}
		if (start < 0x800000) {
			if (end - start >= KARASHI_MD_SRAM_SIZE) end = start + KARASHI_MD_SRAM_SIZE - 1;
			set_sram(machine, start, end, (enum karashi_md_sram_mode) header_sram_mode);
			machine->sram_persistent = (machine->rom[0x1b2] & 0x40) != 0;
		}
	}
	if (!machine->eeprom_present && !machine->sram_present && xin_qigai_wangzi) {
		set_sram(machine, 0x400001, 0x40ffff, KARASHI_MD_SRAM_ODD);
	}
	if (!machine->eeprom_present && !machine->sram_present && sonic_three_knuckles) {
		set_sram(machine, 0x200001, 0x203fff, KARASHI_MD_SRAM_ODD);
	}
	if (!machine->eeprom_present && machine->rom_size >= 0x18a
	    && memcmp(machine->rom + 0x183, "T-26013", 7) == 0) {
		set_sram(machine, 0x200001, 0x203fff, KARASHI_MD_SRAM_ODD);
	} else if (!machine->eeprom_present && !machine->sram_present && machine->rom_size >= 0x18a) {
		if (memcmp(machine->rom + 0x183, "T-50086", 7) == 0
		    || memcmp(machine->rom + 0x183, "T-50286", 7) == 0) {
			set_sram(machine, 0x200001, 0x203fff, KARASHI_MD_SRAM_ODD);
		} else if (memcmp(machine->rom + 0x183, "ACLD007", 7) == 0) {
			set_sram(machine, 0x200001, 0x200fff, KARASHI_MD_SRAM_ODD);
		}
	}
	if (machine->sram_present && barkley_sram) machine->sram_end = 0x3fffff;
	else if (machine->sram_present && triple_play_sram) machine->sram_end = 0x2fffff;
	machine->sram_enabled = machine->mapper != KARASHI_MD_MAPPER_SF004 && machine->sram_present
	                        && (machine->sram_start >= machine->rom_size || triple_play_sram
	                            || sonic_three_knuckles);
}

void karashi_md_init(struct karashi_md *machine, const uint8_t *rom, size_t rom_size) {
	unsigned port;

	memset(machine, 0, sizeof(*machine));
	machine->rom = rom;
	machine->rom_size = rom_size;
	for (port = 0; port < KARASHI_MD_CONTROLLER_PORTS; port++) machine->pad_type[port] = KARASHI_MD_PAD_THREE;
	memset(machine->sram, 0xff, sizeof(machine->sram));
	karashi_md_reset(machine);
}

void karashi_md_attach_mars(
	struct karashi_md *machine, const uint8_t *boot_68k, const uint8_t *master, size_t master_size,
	const uint8_t *slave, size_t slave_size
) {
	karashi_mars_init(&machine->mars, machine->rom, machine->rom_size);
	karashi_mars_set_boot(&machine->mars, boot_68k, master, master_size, slave, slave_size);
	machine->mars_present = true;
	karashi_md_reset(machine);
}

void karashi_md_set_tmss(struct karashi_md *machine, const uint8_t *tmss, size_t tmss_size) {
	machine->tmss = tmss_size ? tmss : NULL;
	machine->tmss_size = machine->tmss ? tmss_size : 0;
	karashi_md_reset(machine);
}

void karashi_md_set_region(struct karashi_md *machine, enum karashi_md_region region) {
	machine->region = (uint8_t) region;
	karashi_md_reset(machine);
}

void karashi_md_set_pad(struct karashi_md *machine, unsigned port, enum karashi_md_pad type) {
	if (port < KARASHI_MD_CONTROLLER_PORTS) machine->pad_type[port] = (uint8_t) type;
}

void karashi_md_reset(struct karashi_md *machine) {
	struct karashi_m68k_bus main_bus;
	struct karashi_z80_bus sound_bus;
	enum karashi_md_region region = (enum karashi_md_region) machine->region;

	if (region == KARASHI_MD_REGION_AUTO) region = header_region(machine);
	machine->pal = region == KARASHI_MD_REGION_EUROPE;
	machine->version = (uint8_t) ((region == KARASHI_MD_REGION_JAPAN ? 0x00 : 0x80) | (machine->pal ? 0x40 : 0) | 0x20 | (machine->tmss ? 1 : 0));
	machine->tmss_mapped = machine->tmss != NULL;
	memset(machine->tmss_key, 0, sizeof(machine->tmss_key));
	machine->master_clock = machine->pal ? KARASHI_MD_PAL_CLOCK : KARASHI_MD_NTSC_CLOCK;
	detect_cartridge(machine);
	memset(machine->work_ram, 0, sizeof(machine->work_ram));
	memset(machine->z80_ram, 0, sizeof(machine->z80_ram));
	memset(machine->io_data, 0, sizeof(machine->io_data));
	memset(machine->io_control, 0, sizeof(machine->io_control));
	memset(machine->pad_phase, 0, sizeof(machine->pad_phase));
	memset(machine->pad_time, 0, sizeof(machine->pad_time));
	memset(machine->pad_latency, 0, sizeof(machine->pad_latency));
	memset(machine->video, 0, sizeof(machine->video));
	machine->io_data[0] = machine->io_data[1] = machine->io_data[2] = 0x7f;
	machine->line_start = 0;
	machine->fm_clock = 0;
	machine->psg_clock = 0;
	machine->audio_clock = 0;
	machine->audio_frames = 0;
	machine->fm_frames = 0;
	machine->z80_bank = 0;
	machine->jcart_th = true;
	machine->width = KARASHI_MD_WIDTH;
	machine->height = KARASHI_MD_HEIGHT;
	machine->z80_bus_requested = false;
	machine->z80_reset_asserted = true;
	karashi_eeprom_reset(&machine->eeprom);
	karashi_spi_eeprom_reset(&machine->spi_eeprom);
	if (machine->eeprom_present) {
		karashi_eeprom_configure(
			&machine->eeprom, (enum karashi_eeprom_mode) machine->eeprom_mode, machine->eeprom_address_mask,
			machine->eeprom_page_mask, machine->eeprom_address_bytes
		);
	}
	memset(&main_bus, 0, sizeof(main_bus));
	main_bus.context = machine;
	main_bus.read = memory_read;
	main_bus.write = memory_write;
	main_bus.read16 = memory_read16;
	main_bus.write16 = memory_write16;
	main_bus.acknowledge = acknowledge;
	main_bus.tas_read_only = true;
	karashi_m68k_init(&machine->cpu, &main_bus);
	update_maps(machine);
	sound_bus.context = machine;
	sound_bus.read = z80_read;
	sound_bus.write = z80_write;
	sound_bus.io_read = z80_io_read;
	sound_bus.io_write = z80_io_write;
	karashi_z80_init(&machine->z80, &sound_bus);
	machine->vdp.dma_read = dma_read;
	machine->vdp.dma_context = machine;
	machine->vdp.pal = machine->pal;
	karashi_mdvdp_reset(&machine->vdp);
	if (machine->mars_present) karashi_mars_reset(&machine->mars, machine->pal);
	karashi_sn76489_reset(&machine->psg, machine->master_clock / 15);
	karashi_ym2612_reset(&machine->ym);
}

void karashi_md_set_controller(struct karashi_md *machine, unsigned port, uint16_t state) {
	if (port < KARASHI_MD_CONTROLLER_PORTS) machine->controller[port] = state & 0xfff;
}

double karashi_md_frame_rate(const struct karashi_md *machine) {
	return (double) machine->master_clock / ((double) KARASHI_MD_LINE_MCLK * (machine->pal ? 313 : 262));
}

static void run_cpu(struct karashi_md *machine, uint64_t target) {
	while (machine->cpu.cycles * 7 < target) {
		uint64_t slice = target;
		if (machine->mars_present && machine->cpu.cycles * 7 + KARASHI_MD_MARS_SLICE < target) {
			slice = machine->cpu.cycles * 7 + KARASHI_MD_MARS_SLICE;
		}
		if (machine->cpu.cycles * 7 < slice) karashi_m68k_run(&machine->cpu, (slice + 6) / 7);
		if (machine->mars_present) karashi_mars_run(&machine->mars, machine->cpu.cycles * 7);
	}
	if (machine->mars_present) karashi_mars_run(&machine->mars, target);
}

static void mix_audio(struct karashi_md *machine) {
	unsigned frames = machine->audio_frames;
	unsigned available = machine->fm_frames;
	uint64_t position = 0;
	uint64_t advance = frames ? (uint64_t) available * 65536 / frames : 0;
	unsigned advance_remainder = frames ? (unsigned) ((uint64_t) available * 65536 % frames) : 0;
	unsigned remainder = 0;
	unsigned index;

	for (index = 0; index < frames; index++) {
		int32_t left = machine->audio[index * 2] / 2;
		int32_t right = machine->audio[index * 2 + 1] / 2;
		if (available) {
			unsigned first = (unsigned) (position >> 16);
			unsigned second = first + 1 < available ? first + 1 : first;
			int32_t weight = (int32_t) (position & 0xffff);
			left += machine->fm[first * 2] + (((machine->fm[second * 2] - machine->fm[first * 2]) * weight) >> 16);
			right += machine->fm[first * 2 + 1]
			         + (((machine->fm[second * 2 + 1] - machine->fm[first * 2 + 1]) * weight) >> 16);
		}
		machine->audio[index * 2] = (int16_t) (left > 32767 ? 32767 : left < -32768 ? -32768 : left);
		machine->audio[index * 2 + 1] = (int16_t) (right > 32767 ? 32767 : right < -32768 ? -32768 : right);
		position += advance;
		remainder += advance_remainder;
		if (remainder >= frames) {
			position++;
			remainder -= frames;
		}
	}
}

void karashi_md_run_frame(struct karashi_md *machine) {
	unsigned lines = machine->vdp.lines;
	unsigned height = karashi_mdvdp_height(&machine->vdp);
	unsigned line;

	machine->audio_frames = 0;
	machine->fm_frames = 0;
	for (line = 0; line < lines; line++) {
		uint64_t start = machine->line_start;

		karashi_mdvdp_begin_line(&machine->vdp, line);
		if (machine->mars_present) karashi_mars_begin_line(&machine->mars, line, height, start);
		if (line == height) karashi_z80_irq(&machine->z80, true);
		else if (line == height + 1) karashi_z80_irq(&machine->z80, false);
		if (line == height) {
			run_cpu(machine, start + KARASHI_MD_VINT_DELAY_MCLK);
			karashi_mdvdp_raise_vint(&machine->vdp);
		}
		update_irq(machine);
		run_cpu(machine, start + KARASHI_MD_HBLANK_MCLK);
		karashi_mdvdp_end_active(&machine->vdp);
		if (machine->mars_present) karashi_mars_end_active(&machine->mars);
		update_irq(machine);
		run_cpu(machine, start + KARASHI_MD_LINE_MCLK);
		if (line < height && machine->mars_present) {
			karashi_pixel *row = machine->video + line * KARASHI_MD_WIDTH;
			karashi_mdvdp_render_line_mask(&machine->vdp, line, row, machine->backdrop);
			karashi_mars_render_line(&machine->mars, line, row, machine->backdrop);
		} else if (line < height) karashi_mdvdp_render_line(&machine->vdp, line, machine->video + line * KARASHI_MD_WIDTH);
		z80_sync(machine, start + KARASHI_MD_LINE_MCLK);
		machine->line_start = start + KARASHI_MD_LINE_MCLK;
	}
	fm_sync(machine, machine->line_start);
	psg_sync(machine, machine->line_start);
	mix_audio(machine);
	if (machine->mars_present) karashi_mars_mix_audio(&machine->mars, machine->audio, machine->audio_frames);
	if (machine->mars_present) karashi_mars_end_frame(&machine->mars);
	machine->width = karashi_mdvdp_width(&machine->vdp);
	machine->height = height;
}

bool karashi_md_save(const struct karashi_md *machine, struct karashi_state_writer *writer) {
	unsigned index;

	karashi_m68k_save(&machine->cpu, writer);
	karashi_z80_save(&machine->z80, writer);
	karashi_mdvdp_save(&machine->vdp, writer);
	karashi_sn76489_save(&machine->psg, writer);
	karashi_ym2612_save(&machine->ym, writer);
	karashi_eeprom_save(&machine->eeprom, writer);
	karashi_spi_eeprom_save(&machine->spi_eeprom, writer);
	karashi_state_write_bytes(writer, machine->work_ram, sizeof(machine->work_ram));
	karashi_state_write_bytes(writer, machine->z80_ram, sizeof(machine->z80_ram));
	karashi_state_write_bytes(writer, machine->sram, sizeof(machine->sram));
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) {
		karashi_state_write_u16(writer, machine->controller[index]);
	}
	karashi_state_write_bytes(writer, machine->pad_phase, sizeof(machine->pad_phase));
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) {
		karashi_state_write_u64(writer, machine->pad_time[index]);
	}
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) {
		karashi_state_write_u64(writer, machine->pad_latency[index]);
	}
	karashi_state_write_bytes(writer, machine->io_data, sizeof(machine->io_data));
	karashi_state_write_bytes(writer, machine->io_control, sizeof(machine->io_control));
	karashi_state_write_bytes(writer, machine->rom_bank, sizeof(machine->rom_bank));
	karashi_state_write_bool(writer, machine->sf004_linear);
	karashi_state_write_bool(writer, machine->tmss_mapped);
	karashi_state_write_bytes(writer, machine->tmss_key, sizeof(machine->tmss_key));
	karashi_state_write_u64(writer, machine->line_start);
	karashi_state_write_u64(writer, machine->fm_clock);
	karashi_state_write_u64(writer, machine->psg_clock);
	karashi_state_write_u32(writer, machine->audio_clock);
	karashi_state_write_u16(writer, machine->z80_bank);
	karashi_state_write_bool(writer, machine->sram_enabled);
	karashi_state_write_bool(writer, machine->sram_write_protected);
	karashi_state_write_bool(writer, machine->eeprom_read_enabled);
	karashi_state_write_bool(writer, machine->jcart_th);
	karashi_state_write_bool(writer, machine->z80_bus_requested);
	if (machine->mars_present) karashi_mars_save(&machine->mars, writer);
	return karashi_state_write_bool(writer, machine->z80_reset_asserted);
}

bool karashi_md_load(struct karashi_md *machine, struct karashi_state_reader *reader) {
	bool loaded = karashi_m68k_load(&machine->cpu, reader) && karashi_z80_load(&machine->z80, reader)
	              && karashi_mdvdp_load(&machine->vdp, reader) && karashi_sn76489_load(&machine->psg, reader)
	              && karashi_ym2612_load(&machine->ym, reader) && karashi_eeprom_load(&machine->eeprom, reader)
	              && karashi_spi_eeprom_load(&machine->spi_eeprom, reader);
	unsigned index;

	loaded = karashi_state_read_bytes(reader, machine->work_ram, sizeof(machine->work_ram)) && loaded;
	loaded = karashi_state_read_bytes(reader, machine->z80_ram, sizeof(machine->z80_ram)) && loaded;
	loaded = karashi_state_read_bytes(reader, machine->sram, sizeof(machine->sram)) && loaded;
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) {
		loaded = karashi_state_read_u16(reader, &machine->controller[index]) && loaded;
	}
	loaded = karashi_state_read_bytes(reader, machine->pad_phase, sizeof(machine->pad_phase)) && loaded;
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) {
		loaded = karashi_state_read_u64(reader, &machine->pad_time[index]) && loaded;
	}
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) {
		loaded = karashi_state_read_u64(reader, &machine->pad_latency[index]) && loaded;
	}
	loaded = karashi_state_read_bytes(reader, machine->io_data, sizeof(machine->io_data)) && loaded;
	loaded = karashi_state_read_bytes(reader, machine->io_control, sizeof(machine->io_control)) && loaded;
	loaded = karashi_state_read_bytes(reader, machine->rom_bank, sizeof(machine->rom_bank)) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->sf004_linear) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->tmss_mapped) && loaded;
	loaded = karashi_state_read_bytes(reader, machine->tmss_key, sizeof(machine->tmss_key)) && loaded;
	loaded = karashi_state_read_u64(reader, &machine->line_start) && loaded;
	loaded = karashi_state_read_u64(reader, &machine->fm_clock) && loaded;
	loaded = karashi_state_read_u64(reader, &machine->psg_clock) && loaded;
	loaded = karashi_state_read_u32(reader, &machine->audio_clock) && loaded;
	loaded = karashi_state_read_u16(reader, &machine->z80_bank) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->sram_enabled) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->sram_write_protected) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->eeprom_read_enabled) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->jcart_th) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->z80_bus_requested) && loaded;
	if (machine->mars_present) loaded = karashi_mars_load(&machine->mars, reader) && loaded;
	loaded = karashi_state_read_bool(reader, &machine->z80_reset_asserted) && loaded;
	if (!loaded) return false;
	machine->audio_clock %= machine->master_clock;
	machine->z80_bank &= 0x1ff;
	for (index = 0; index < KARASHI_MD_CONTROLLER_PORTS; index++) machine->pad_phase[index] &= 7;
	for (index = 0; index < 8; index++) {
		machine->rom_bank[index] &= machine->mapper == KARASHI_MD_MAPPER_T5740 ? 0x0f : 0x3f;
	}
	machine->sf004_linear = machine->sf004_linear && machine->mapper == KARASHI_MD_MAPPER_SF004;
	machine->sram_enabled = machine->sram_enabled && machine->sram_present;
	machine->eeprom_read_enabled = machine->eeprom_read_enabled
	                               && machine->eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM;
	machine->tmss_mapped = machine->tmss_mapped && machine->tmss;
	update_maps(machine);
	update_irq(machine);
	return true;
}
