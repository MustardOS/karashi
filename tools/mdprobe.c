#include "md.h"

#include <stdio.h>
#include <stdlib.h>

static uint16_t rom_word(const uint8_t *rom, size_t size, uint32_t address) {
	if ((size_t) address + 1 >= size) return 0xffff;
	return (uint16_t) ((uint16_t) rom[address] << 8 | rom[address + 1]);
}

static unsigned ym_keys(const struct karashi_ym2612 *ym) {
	unsigned channel;
	unsigned keys = 0;

	for (channel = 0; channel < 6; channel++) {
		unsigned operator;
		for (operator = 0; operator < 4; operator++) keys += ym->channel[channel].op[operator].key;
	}
	return keys;
}

static unsigned peak(const int16_t *samples, unsigned frames) {
	unsigned index;
	unsigned result = 0;

	for (index = 0; index < frames * 2; index++) {
		unsigned value = samples[index] < 0 ? (unsigned) -(int32_t) samples[index] : (unsigned) samples[index];
		if (value > result) result = value;
	}
	return result;
}

static unsigned nonzero(const uint8_t *data, size_t size) {
	size_t index;
	unsigned count = 0;

	for (index = 0; index < size; index++) count += data[index] != 0;
	return count;
}

int main(int argc, char **argv) {
	struct karashi_md machine;
	uint8_t *rom;
	uint8_t *tmss = NULL;
	FILE *file;
	long length;
	unsigned long steps;
	unsigned long limit = 1000000;
	uint32_t instruction_pc;
	bool frame_mode = false;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s ROM [STEPS]\n", argv[0]);
		return 2;
	}
	if (argc == 3) {
		frame_mode = argv[2][0] == 'f';
		limit = strtoul(argv[2] + frame_mode, NULL, 0);
	}
	file = fopen(argv[1], "rb");
	if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "Could not open ROM: %s\n", argv[1]);
		if (file) fclose(file);
		return 1;
	}
	rom = malloc((size_t) length);
	if (!rom || fread(rom, 1, (size_t) length, file) != (size_t) length) {
		fprintf(stderr, "Could not read ROM: %s\n", argv[1]);
		free(rom);
		fclose(file);
		return 1;
	}
	fclose(file);
	karashi_md_init(&machine, rom, (size_t) length);
	if (getenv("KARASHI_SYSTEM_DIR")) {
		char path[4096];
		long tmss_length;

		snprintf(path, sizeof(path), "%s/bios_MD.bin", getenv("KARASHI_SYSTEM_DIR"));
		file = fopen(path, "rb");
		if (!file || fseek(file, 0, SEEK_END) != 0 || (tmss_length = ftell(file)) < 2048
		    || tmss_length > 16384 || fseek(file, 0, SEEK_SET) != 0) {
			fprintf(stderr, "Could not open TMSS ROM: %s\n", path);
			if (file) fclose(file);
			free(rom);
			return 1;
		}
		tmss = malloc((size_t) tmss_length);
		if (!tmss || fread(tmss, 1, (size_t) tmss_length, file) != (size_t) tmss_length) {
			fprintf(stderr, "Could not read TMSS ROM: %s\n", path);
			free(tmss);
			free(rom);
			fclose(file);
			return 1;
		}
		fclose(file);
		karashi_md_set_tmss(&machine, tmss, (size_t) tmss_length);
	}
	instruction_pc = machine.cpu.pc;
	if (frame_mode) {
		for (steps = 0; steps < limit && machine.cpu.fault == KARASHI_M68K_FAULT_NONE; steps++) {
			karashi_md_run_frame(&machine);
		}
		printf(
			"frames=%lu cycles=%llu pc=%06x instruction=%06x opcode=%04x sr=%04x irq=%u fault=%u illegal=%u first_illegal=%06x:%04x address=%u stopped=%u mapper=%u eeprom=%u,%u jcart=%u sram=%u,%u,%u,%u,%u,%06x,%06x,%u now=%llu z80=%04x,%llu,%u,%u,%u,%03x,%u ym=%u,%u,%02x,%02x,%llu,%u,%u psg=%u,%u,%u,%u line=%u status=%04x pending=%u,%u vdp=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
			steps, (unsigned long long) machine.cpu.cycles, machine.cpu.pc, machine.cpu.instruction_pc,
			rom_word(rom, (size_t) length, machine.cpu.instruction_pc), machine.cpu.sr, machine.cpu.irq_level,
			machine.cpu.fault, machine.cpu.illegal_count,
			machine.cpu.first_illegal_pc, machine.cpu.first_illegal_opcode, machine.cpu.address_error_count,
			machine.cpu.stopped, machine.mapper, machine.eeprom_mapping, machine.eeprom_size,
			machine.jcart_present, machine.sram_present, machine.sram_persistent, machine.sram_enabled,
			machine.sram_write_protected, machine.sram_mode, machine.sram_start, machine.sram_end,
			machine.sram_size,
			(unsigned long long) (machine.cpu.cycles * 7),
			machine.z80.pc, (unsigned long long) machine.z80.cycles, machine.z80.halted,
			machine.z80_reset_asserted, machine.z80_bus_requested, machine.z80_bank,
			nonzero(machine.z80_ram, sizeof(machine.z80_ram)),
			ym_keys(&machine.ym), machine.ym.dac_enabled, machine.ym.address, machine.ym.part,
			(unsigned long long) machine.ym.busy_until,
			peak(machine.fm, machine.fm_frames), peak(machine.audio, machine.audio_frames),
			machine.psg.volume[0], machine.psg.volume[1], machine.psg.volume[2], machine.psg.volume[3],
			machine.vdp.line, machine.vdp.status, machine.vdp.vint_pending,
			machine.vdp.hint_pending,
			machine.vdp.registers[0], machine.vdp.registers[1], machine.vdp.registers[10],
			machine.vdp.registers[11], machine.vdp.registers[12], machine.vdp.registers[13],
			machine.vdp.registers[16], machine.vdp.registers[17], machine.vdp.registers[18],
			machine.vdp.registers[23]
		);
		for (unsigned channel = 0; channel < 6; channel++) {
			const struct karashi_ym2612_channel *ym_channel = &machine.ym.channel[channel];
			printf(
				"ym%u frequency=%03x block=%u algorithm=%u feedback=%u pan=%u,%u key=%u,%u,%u,%u level=%u,%u,%u,%u output=%d,%d,%d,%d\n",
				channel, ym_channel->frequency, ym_channel->block, ym_channel->algorithm,
				ym_channel->feedback, ym_channel->left, ym_channel->right,
				ym_channel->op[0].key, ym_channel->op[1].key, ym_channel->op[2].key,
				ym_channel->op[3].key, ym_channel->op[0].level, ym_channel->op[1].level,
				ym_channel->op[2].level, ym_channel->op[3].level, ym_channel->op[0].output,
				ym_channel->op[1].output, ym_channel->op[2].output, ym_channel->op[3].output
			);
		}
		printf("z80ram=");
		for (unsigned index = 0; index < 64; index++) printf("%02x", machine.z80_ram[index]);
		putchar('\n');
		free(rom);
		free(tmss);
		return machine.cpu.fault == KARASHI_M68K_FAULT_NONE ? 0 : 3;
	}
	for (steps = 0; steps < limit && machine.cpu.fault == KARASHI_M68K_FAULT_NONE; steps++) {
		instruction_pc = machine.cpu.pc;
		if (!karashi_m68k_step(&machine.cpu) && !machine.cpu.stopped) break;
	}
	printf(
		"steps=%lu cycles=%llu pc=%06x fault=%u illegal=%u first_illegal=%06x:%04x address=%u stopped=%u mapper=%u eeprom=%u,%u jcart=%u sram=%u,%u,%u,%u,%u,%06x,%06x,%u opcode=%04x\n", steps,
		(unsigned long long) machine.cpu.cycles, machine.cpu.pc, machine.cpu.fault, machine.cpu.illegal_count,
		machine.cpu.first_illegal_pc, machine.cpu.first_illegal_opcode, machine.cpu.address_error_count, machine.cpu.stopped,
		machine.mapper, machine.eeprom_mapping, machine.eeprom_size, machine.jcart_present,
		machine.sram_present, machine.sram_persistent, machine.sram_enabled, machine.sram_write_protected,
		machine.sram_mode, machine.sram_start, machine.sram_end, machine.sram_size,
		rom_word(rom, (size_t) length, instruction_pc)
	);
	free(rom);
	free(tmss);
	return machine.cpu.fault == KARASHI_M68K_FAULT_NONE ? 0 : 3;
}
