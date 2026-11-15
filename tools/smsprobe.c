#include "sms.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned visible_sprites(const struct karashi_smsvdp *vdp, unsigned line) {
	const uint8_t *vram = vdp->tms.vram;
	const uint8_t *registers = vdp->tms.registers;
	unsigned table = (registers[5] & 0x7e) << 7;
	unsigned height = (registers[1] & 0x02) ? 16 : 8;
	unsigned zoom = registers[1] & 0x01;
	unsigned count = 0;
	unsigned sprite;

	for (sprite = 0; sprite < 64; sprite++) {
		unsigned raw_y = vram[(table + sprite) & 0x3fff];
		int top;
		if (raw_y == 0xd0 && vdp->height == KARASHI_SMSVDP_HEIGHT) break;
		top = (raw_y > (vdp->height == KARASHI_SMSVDP_HEIGHT ? 0xd0u : 0xf0u) ? (int) raw_y - 256 : (int) raw_y) + 1;
		if ((int) line >= top && (int) line < top + (int) (height << zoom)) count++;
	}
	return count;
}

int main(int argc, char **argv) {
	struct karashi_sms machine;
	uint8_t *rom;
	FILE *file;
	long length;
	unsigned frames;
	unsigned frame;
	unsigned line;
	unsigned maximum = 0;

	if (argc != 3) return 2;
	frames = (unsigned) strtoul(argv[2], NULL, 0);
	file = fopen(argv[1], "rb");
	if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) return 1;
	rom = malloc((size_t) length);
	if (!rom || fread(rom, 1, (size_t) length, file) != (size_t) length) return 1;
	fclose(file);
	karashi_sms_init(&machine, rom, (size_t) length, false, false, true);
	for (frame = 0; frame < frames; frame++) {
		unsigned phase = frame % 120;
		uint8_t pad = 0;
		bool pause = false;
		if (frame >= 150) {
			pause = phase < 6;
			if (phase >= 40 && phase < 46) pad |= KARASHI_SMS_PAD_ONE;
			if (phase >= 80 && phase < 86) pad |= KARASHI_SMS_PAD_TWO;
		}
		karashi_sms_set_controller(&machine, 0, pad);
		karashi_sms_set_pause(&machine, pause);
		karashi_sms_run_frame(&machine);
	}
	for (line = 0; line < machine.vdp.height; line++) {
		unsigned count = visible_sprites(&machine.vdp, line);
		if (line >= 17 && count > maximum) maximum = count;
		if (line >= 17 && count >= 6) printf("line=%u sprites=%u\n", line, count);
	}
	printf(
		"frames=%u pc=%04x height=%u maximum=%u status=%02x table=%04x mode=%02x,%02x\n", frames,
		machine.cpu.pc, machine.vdp.height, maximum, machine.vdp.tms.status,
		(machine.vdp.tms.registers[5] & 0x7e) << 7, machine.vdp.tms.registers[0], machine.vdp.tms.registers[1]
	);
	{
		unsigned table = (machine.vdp.tms.registers[5] & 0x7e) << 7;
		unsigned sprite;
		for (sprite = 0; sprite < 64; sprite++) {
			unsigned y = machine.vdp.tms.vram[(table + sprite) & 0x3fff];
			unsigned attribute = (table + 128 + sprite * 2) & 0x3fff;
			if (y != 0xff && y != 0xd0) {
				printf(
					"sprite=%u y=%02x x=%02x pattern=%02x\n", sprite, y, machine.vdp.tms.vram[attribute],
					machine.vdp.tms.vram[(attribute + 1) & 0x3fff]
				);
			}
		}
	}
	free(rom);
	return 0;
}
