#include "z80.h"
#include <stdio.h>
#include <string.h>
static uint8_t memory[65536];
static uint8_t rd(void *c, uint16_t a) { (void) c; return memory[a]; }
static void wr(void *c, uint16_t a, uint8_t v) { (void) c; memory[a] = v; }
static uint8_t ird(void *c, uint16_t p) { (void) c; (void) p; return 0xff; }
static void iwr(void *c, uint16_t p, uint8_t v) { (void) c; (void) p; (void) v; }
int main(int argc, char **argv) {
	struct karashi_z80 cpu; struct karashi_z80_bus bus = {NULL, rd, wr, ird, iwr};
	FILE *file = fopen(argv[1], "rb");
	(void) argc;
	if (!file || !fread(memory + 0x100, 1, 0xff00, file)) return 1;
	memory[5] = 0xc9; memory[0] = 0x76;
	memory[6] = 0x00; memory[7] = 0xf0;
	karashi_z80_init(&cpu, &bus);
	cpu.pc = 0x100; cpu.sp = 0xf000;
	for (;;) {
		if (cpu.pc == 5 && !cpu.prefix_active && !cpu.prefetched_valid) {
			unsigned function = cpu.bc & 0xff;
			if (function == 2) putchar(cpu.de & 0xff);
			else if (function == 9) { uint16_t a = cpu.de; while (memory[a] != '$') putchar(memory[a++]); }
			fflush(stdout);
		}
		if (cpu.pc == 0 || cpu.halted) break;
		karashi_z80_step(&cpu);
	}
	putchar('\n');
	return 0;
}
