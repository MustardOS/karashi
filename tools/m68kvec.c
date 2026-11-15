#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runs single-instruction 68000 test vectors in the SingleStepTests "json.bin" layout.
 * The vectors are black-box observations and are read in place; none are stored in this repository.
 */

#define MEMORY_SIZE (1u << 24)
#define JOURNAL_SIZE 4096

struct state {
	uint32_t reg[19];
	uint32_t prefetch[2];
	uint32_t ram_count;
	const uint8_t *ram;
};

static uint8_t *memory;
static uint32_t journal[JOURNAL_SIZE];
static unsigned journal_count;

static uint32_t get32(const uint8_t *data) {
	return (uint32_t) data[0] | (uint32_t) data[1] << 8 | (uint32_t) data[2] << 16 | (uint32_t) data[3] << 24;
}

static uint8_t bus_read(void *context, uint32_t address) {
	(void) context;
	return memory[address & (MEMORY_SIZE - 1)];
}

static void bus_write(void *context, uint32_t address, uint8_t value) {
	(void) context;
	address &= MEMORY_SIZE - 1;
	memory[address] = value;
	if (journal_count < JOURNAL_SIZE) journal[journal_count++] = address;
}

static const uint8_t *read_state(const uint8_t *cursor, struct state *state) {
	unsigned index;

	cursor += 8;
	for (index = 0; index < 19; index++) state->reg[index] = get32(cursor + index * 4);
	cursor += 19 * 4;
	state->prefetch[0] = get32(cursor);
	state->prefetch[1] = get32(cursor + 4);
	cursor += 8;
	state->ram_count = get32(cursor);
	state->ram = cursor + 4;
	return state->ram + state->ram_count * 6;
}

static void apply_ram(const struct state *state) {
	uint32_t index;

	for (index = 0; index < state->ram_count; index++) {
		uint32_t address = get32(state->ram + index * 6) & (MEMORY_SIZE - 2);
		memory[address] = state->ram[index * 6 + 5];
		memory[address + 1] = state->ram[index * 6 + 4];
		if (journal_count < JOURNAL_SIZE) journal[journal_count++] = address;
		if (journal_count < JOURNAL_SIZE) journal[journal_count++] = address + 1;
	}
}

int main(int argc, char **argv) {
	struct karashi_m68k_bus bus;
	struct karashi_m68k cpu;
	int file_index;
	int verbose = 0;
	int show_timing = 0;
	unsigned long total = 0;
	unsigned long total_failed = 0;
	unsigned long total_timing = 0;
	unsigned long total_prefetch = 0;
	bool check_prefetch = false;

	memory = calloc(MEMORY_SIZE, 1);
	if (!memory) return 1;
	memset(&bus, 0, sizeof(bus));
	bus.read = bus_read;
	bus.write = bus_write;
	for (file_index = 1; file_index < argc; file_index++) {
		FILE *file;
		long length;
		uint8_t *data;
		const uint8_t *cursor;
		uint32_t count;
		uint32_t test;
		unsigned long failed = 0;
		unsigned long timing = 0;
		unsigned long address_errors = 0;
		unsigned long prefetch_differences = 0;

		if (strcmp(argv[file_index], "-t") == 0) {
			show_timing = 1;
			verbose = 5;
			continue;
		}
		if (strcmp(argv[file_index], "-v") == 0) {
			verbose = 12;
			continue;
		}
		if (strcmp(argv[file_index], "-p") == 0) {
			check_prefetch = true;
			verbose = 12;
			continue;
		}
		file = fopen(argv[file_index], "rb");
		if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 8 || fseek(file, 0, SEEK_SET)) return 1;
		data = malloc((size_t) length);
		if (!data || fread(data, 1, (size_t) length, file) != (size_t) length) return 1;
		fclose(file);
		count = get32(data + 4);
		cursor = data + 8;
		for (test = 0; test < count; test++) {
			struct state initial;
			struct state final;
			const uint8_t *name;
			uint32_t name_length;
			uint32_t cycles;
			unsigned index;
			bool supervisor;
			bool bad = false;
			bool address_case;
			bool prefetch_bad;
			unsigned used;

			cursor += 8;
			name_length = get32(cursor + 8);
			name = cursor + 12;
			cursor = read_state(name + name_length, &initial);
			cursor = read_state(cursor, &final);
			cycles = get32(cursor + 8);
			cursor += get32(cursor);
			journal_count = 0;
			apply_ram(&initial);
			karashi_m68k_init(&cpu, &bus);
			supervisor = (initial.reg[17] & 0x2000) != 0;
			for (index = 0; index < 8; index++) cpu.d[index] = initial.reg[index];
			for (index = 0; index < 7; index++) cpu.a[index] = initial.reg[8 + index];
			cpu.usp = initial.reg[15];
			cpu.ssp = initial.reg[16];
			cpu.a[7] = supervisor ? cpu.ssp : cpu.usp;
			cpu.sr = (uint16_t) initial.reg[17];
			cpu.pc = (initial.reg[18] - 4) & 0xffffff;
			cpu.fault = KARASHI_M68K_FAULT_NONE;
			address_case = ((final.reg[18] - 4) & 0xffffff) == (((uint32_t) memory[13] << 16 | (uint32_t) memory[14] << 8 | memory[15]) & 0xffffff);
			used = karashi_m68k_step(&cpu);
			prefetch_bad = cpu.prefetch_count != 2 || (!cpu.stopped && cpu.prefetch_address != cpu.pc)
			               || cpu.prefetch[0] != (uint16_t) final.prefetch[0]
			               || cpu.prefetch[1] != (uint16_t) final.prefetch[1];
			if (check_prefetch && prefetch_bad) {
				prefetch_differences++;
				if (verbose) {
					verbose--;
					printf(
						"PREFETCH %.*s: %u at %06x %04x %04x want %04x %04x\n", (int) name_length, name,
						cpu.prefetch_count, cpu.prefetch_address & 0xffffff, cpu.prefetch[0], cpu.prefetch[1],
						(uint16_t) final.prefetch[0], (uint16_t) final.prefetch[1]
					);
				}
			}
			if (address_case) {
				address_errors++;
				supervisor = (cpu.sr & 0x2000) != 0;
				if (supervisor) cpu.ssp = cpu.a[7];
				else cpu.usp = cpu.a[7];
				bad = cpu.address_error_count != 1 || ((cpu.pc + 4) & 0xffffff) != (final.reg[18] & 0xffffff);
				for (index = 0; index < 8; index++) bad |= cpu.d[index] != final.reg[index];
				for (index = 0; index < 7; index++) bad |= cpu.a[index] != final.reg[8 + index];
				bad |= cpu.usp != final.reg[15] || cpu.ssp != final.reg[16];
				bad |= cpu.sr != (uint16_t) final.reg[17];
				for (index = 0; index < final.ram_count; index++) {
					uint32_t address = get32(final.ram + index * 6) & (MEMORY_SIZE - 2);
					bad |= memory[address] != final.ram[index * 6 + 5]
					       || memory[address + 1] != final.ram[index * 6 + 4];
				}
				if (bad) {
					failed++;
					if (verbose) {
						verbose--;
						printf(
							"FAIL address %.*s sr=%04x/%04x usp=%08x/%08x ssp=%08x/%08x pc=%06x/%06x start=%06x\n",
							(int) name_length, name, cpu.sr, (uint16_t) final.reg[17], cpu.usp, final.reg[15], cpu.ssp,
							final.reg[16], (cpu.pc + 4) & 0xffffff, final.reg[18] & 0xffffff,
							(initial.reg[18] - 4) & 0xffffff
						);
						printf(
							"  fault=%08x %s a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x a6=%08x\n",
							cpu.error_address, cpu.error_write ? "write" : cpu.error_instruction ? "fetch" : "read",
							initial.reg[8], initial.reg[9], initial.reg[10], initial.reg[11], initial.reg[12], initial.reg[13],
							initial.reg[14]
						);
						for (index = 0; index < 8; index++) {
							if (cpu.d[index] != final.reg[index]) {
								printf("  d%u %08x want %08x\n", index, cpu.d[index], final.reg[index]);
							}
						}
						for (index = 0; index < 7; index++) {
							if (cpu.a[index] != final.reg[8 + index]) {
								printf("  a%u %08x want %08x\n", index, cpu.a[index], final.reg[8 + index]);
							}
						}
						{
							uint32_t start = (initial.reg[18] - 4) & (MEMORY_SIZE - 1);
							printf(
								"  code %04x %04x %04x %04x\n",
								(unsigned) memory[start] << 8 | memory[(start + 1) & (MEMORY_SIZE - 1)],
								(unsigned) memory[(start + 2) & (MEMORY_SIZE - 1)] << 8 | memory[(start + 3) & (MEMORY_SIZE - 1)],
								(unsigned) memory[(start + 4) & (MEMORY_SIZE - 1)] << 8 | memory[(start + 5) & (MEMORY_SIZE - 1)],
								(unsigned) memory[(start + 6) & (MEMORY_SIZE - 1)] << 8 | memory[(start + 7) & (MEMORY_SIZE - 1)]
							);
						}
						for (index = 0; index < final.ram_count; index++) {
							uint32_t address = get32(final.ram + index * 6) & (MEMORY_SIZE - 2);
							unsigned want = (unsigned) final.ram[index * 6 + 5] << 8 | final.ram[index * 6 + 4];
							unsigned have = (unsigned) memory[address] << 8 | memory[address + 1];
							if (want != have) printf("  ram %06x %04x want %04x\n", address, have, want);
						}
					}
				}
				for (index = 0; index < journal_count; index++) memory[journal[index]] = 0;
				continue;
			}
			supervisor = (cpu.sr & 0x2000) != 0;
			if (supervisor) cpu.ssp = cpu.a[7];
			else cpu.usp = cpu.a[7];
			for (index = 0; index < 8; index++) bad |= cpu.d[index] != final.reg[index];
			for (index = 0; index < 7; index++) bad |= cpu.a[index] != final.reg[8 + index];
			bad |= cpu.usp != final.reg[15] || cpu.ssp != final.reg[16];
			bad |= cpu.sr != (uint16_t) final.reg[17];
			bad |= ((cpu.pc + (cpu.stopped ? 0 : 4)) & 0xffffff) != (final.reg[18] & 0xffffff);
			for (index = 0; index < final.ram_count; index++) {
				uint32_t address = get32(final.ram + index * 6) & (MEMORY_SIZE - 2);
				bad |= memory[address] != final.ram[index * 6 + 5] || memory[address + 1] != final.ram[index * 6 + 4];
			}
			if (bad) {
				failed++;
				if (verbose) {
					verbose--;
					printf("FAIL %.*s\n", (int) name_length, name);
					for (index = 0; index < 8; index++) {
						if (cpu.d[index] != final.reg[index]) {
							printf("  d%u %08x want %08x\n", index, cpu.d[index], final.reg[index]);
						}
					}
					for (index = 0; index < 7; index++) {
						if (cpu.a[index] != final.reg[8 + index]) {
							printf("  a%u %08x want %08x\n", index, cpu.a[index], final.reg[8 + index]);
						}
					}
					printf(
						"  usp %08x/%08x ssp %08x/%08x sr %04x/%04x pc %06x/%06x (start sr %04x pc %06x)\n", cpu.usp,
						final.reg[15], cpu.ssp, final.reg[16], cpu.sr, final.reg[17], (cpu.pc + 4) & 0xffffff,
						final.reg[18] & 0xffffff, initial.reg[17], (initial.reg[18] - 4) & 0xffffff
					);
					for (index = 0; index < final.ram_count; index++) {
						uint32_t address = get32(final.ram + index * 6) & (MEMORY_SIZE - 2);
						unsigned want = (unsigned) final.ram[index * 6 + 5] << 8 | final.ram[index * 6 + 4];
						unsigned have = (unsigned) memory[address] << 8 | memory[address + 1];
						if (want != have) printf("  ram %06x %04x want %04x\n", address, have, want);
					}
				}
			} else if (used != cycles) {
				timing++;
				if (verbose && show_timing) {
					verbose--;
					printf("TIME %.*s: %u want %u\n", (int) name_length, name, used, cycles);
				}
			}
			for (index = 0; index < journal_count; index++) memory[journal[index]] = 0;
		}
		{ const char *base = strrchr(argv[file_index], '/'); printf("%-22s %5u tests, %5lu wrong, %5lu timing, %5lu address errors", base ? base + 1 : argv[file_index], count, failed, timing, address_errors); if (check_prefetch) printf(", %5lu prefetch", prefetch_differences); putchar('\n'); }
		total += count;
		total_failed += failed;
		total_timing += timing;
		total_prefetch += prefetch_differences;
		free(data);
	}
	printf("total %lu tests, %lu wrong, %lu timing only", total, total_failed, total_timing);
	if (check_prefetch) printf(", %lu prefetch differences", total_prefetch);
	putchar('\n');
	free(memory);
	return total_failed ? 3 : 0;
}
