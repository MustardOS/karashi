#include "md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * marsprobe ROM BIOS_DIRECTORY FRAMES
 * Runs 32X content with BIOS images read in place and prints the state of all three CPUs.
 * The BIOS directory must hold 32X_G_BIOS.BIN, 32X_M_BIOS.BIN and 32X_S_BIOS.BIN.
 */

static uint8_t *load(const char *path, size_t *size) {
	FILE *file = fopen(path, "rb");
	long length;
	uint8_t *data;

	if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) {
		if (file) fclose(file);
		return NULL;
	}
	data = malloc((size_t) length);
	if (data && fread(data, 1, (size_t) length, file) != (size_t) length) {
		free(data);
		data = NULL;
	}
	fclose(file);
	*size = (size_t) length;
	return data;
}

static uint8_t *bios(const char *directory, const char *name, size_t *size) {
	char path[4096];

	snprintf(path, sizeof(path), "%s/%s", directory, name);
	return load(path, size);
}

int main(int argc, char **argv) {
	static struct karashi_md machine;
	uint8_t *rom;
	uint8_t *boot_68k;
	uint8_t *master;
	uint8_t *slave;
	size_t rom_size;
	size_t size_68k;
	size_t master_size;
	size_t slave_size;
	unsigned frames;
	unsigned frame;
	unsigned index;

	if (argc != 4) {
		fprintf(stderr, "usage: %s ROM BIOS_DIRECTORY FRAMES\n", argv[0]);
		return 2;
	}
	rom = load(argv[1], &rom_size);
	boot_68k = bios(argv[2], "32X_G_BIOS.BIN", &size_68k);
	master = bios(argv[2], "32X_M_BIOS.BIN", &master_size);
	slave = bios(argv[2], "32X_S_BIOS.BIN", &slave_size);
	if (!rom || !boot_68k || !master || !slave || size_68k < 256) {
		fprintf(stderr, "could not read the content or the BIOS images\n");
		return 1;
	}
	frames = (unsigned) strtoul(argv[3], NULL, 0);
	karashi_md_init(&machine, rom, rom_size);
	karashi_md_attach_mars(&machine, boot_68k, master, master_size, slave, slave_size);
	for (frame = 0; frame < frames; frame++) karashi_md_run_frame(&machine);
	printf("68k pc=%06x sr=%04x fault=%u illegal=%u\n", machine.cpu.pc, machine.cpu.sr, machine.cpu.fault,
	       machine.cpu.illegal_count);
	for (index = 0; index < 2; index++) {
		const struct karashi_sh2 *cpu = &machine.mars.cpu[index];
		printf("%s pc=%08x sr=%03x pr=%08x vbr=%08x fault=%u stopped=%u irl=%u r0=%08x r1=%08x\n",
		       index ? "slave " : "master", cpu->pc, cpu->sr, cpu->pr, cpu->vbr, cpu->fault, cpu->stopped, cpu->irl,
		       cpu->r[0], cpu->r[1]);
	}
	printf("adapter=%04x intctl=%04x mask=%04x,%04x pending=%02x,%02x mode=%04x display=%u\n", machine.mars.adapter,
	       machine.mars.interrupt_control, machine.mars.interrupt_mask[0], machine.mars.interrupt_mask[1],
	       machine.mars.pending[0], machine.mars.pending[1], machine.mars.bitmap_mode, machine.mars.display);
	printf("comm=");
	for (index = 0; index < 8; index++) printf("%04x ", machine.mars.comm[index]);
	printf("\n");
	return 0;
}
