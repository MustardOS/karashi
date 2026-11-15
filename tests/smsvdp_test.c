#include "smsvdp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static karashi_pixel row[KARASHI_SMSVDP_WIDTH];

static void write_register(struct karashi_smsvdp *vdp, unsigned index, uint8_t value) {
	karashi_smsvdp_write_control(vdp, value);
	karashi_smsvdp_write_control(vdp, (uint8_t) (0x80 | index));
}

static void set_address(struct karashi_smsvdp *vdp, unsigned code, uint16_t address) {
	karashi_smsvdp_write_control(vdp, (uint8_t) address);
	karashi_smsvdp_write_control(vdp, (uint8_t) ((code << 6) | (address >> 8)));
}

static void write_color(struct karashi_smsvdp *vdp, unsigned index, uint8_t value) {
	set_address(vdp, 3, (uint16_t) index);
	karashi_smsvdp_write_data(vdp, value);
}

static void solid_tile(struct karashi_smsvdp *vdp, unsigned pattern, unsigned color) {
	unsigned line;
	unsigned plane;

	for (line = 0; line < 8; line++) {
		for (plane = 0; plane < 4; plane++) {
			vdp->tms.vram[pattern * 32 + line * 4 + plane] = (color & (1u << plane)) ? 0xff : 0x00;
		}
	}
}

static void mode4(struct karashi_smsvdp *vdp) {
	karashi_smsvdp_reset(vdp, false, false);
	write_register(vdp, 0, 0x04);
	write_register(vdp, 1, 0x40);
	write_register(vdp, 2, 0xff);
	write_register(vdp, 5, 0xff);
	write_register(vdp, 6, 0xfb);
	write_register(vdp, 7, 0x00);
}

static void test_ports_and_palette(void) {
	struct karashi_smsvdp vdp;

	mode4(&vdp);
	write_color(&vdp, 16, 0x3f);
	write_color(&vdp, 17, 0x03);
	assert(vdp.cram[16] == 0x3f);
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(row[0] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	write_register(&vdp, 7, 0x01);
	write_register(&vdp, 1, 0x00);
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(row[100] == KARASHI_PIXEL(0xff, 0x00, 0x00));

	set_address(&vdp, 1, 0x1234);
	karashi_smsvdp_write_data(&vdp, 0xa5);
	assert(vdp.tms.vram[0x1234] == 0xa5);
	set_address(&vdp, 0, 0x1234);
	assert(karashi_smsvdp_read_data(&vdp) == 0xa5);

	karashi_smsvdp_reset(&vdp, true, false);
	set_address(&vdp, 3, 2);
	karashi_smsvdp_write_data(&vdp, 0x5f);
	assert(vdp.cram[1] == 0);
	karashi_smsvdp_write_data(&vdp, 0xfa);
	assert(vdp.cram[1] == 0x0a5f);
}

static void test_background(void) {
	struct karashi_smsvdp vdp;
	unsigned name = 0x3800;

	mode4(&vdp);
	write_color(&vdp, 0, 0x00);
	write_color(&vdp, 1, 0x03);
	write_color(&vdp, 17, 0x0c);
	write_color(&vdp, 18, 0x30);
	solid_tile(&vdp, 1, 1);
	vdp.tms.vram[2 * 32] = 0x80;
	vdp.tms.vram[name] = 1;
	vdp.tms.vram[name + 2] = 2;
	vdp.tms.vram[name + 4] = 2;
	vdp.tms.vram[name + 5] = 0x02;
	vdp.tms.vram[name + 6] = 1;
	vdp.tms.vram[name + 7] = 0x08;
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(row[0] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[8] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[9] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	assert(row[16] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	assert(row[23] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[24] == KARASHI_PIXEL(0x00, 0xff, 0x00));

	write_register(&vdp, 8, 4);
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(row[4] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[11] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[12] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[13] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	write_register(&vdp, 0, 0x44);
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(row[0] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	karashi_smsvdp_begin_line(&vdp, 16, row);
	assert(row[0] == KARASHI_PIXEL(0x00, 0x00, 0x00));

	write_register(&vdp, 0, 0x04);
	write_register(&vdp, 8, 0);
	write_register(&vdp, 9, 216);
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(row[0] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	karashi_smsvdp_begin_line(&vdp, 8, row);
	assert(row[0] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	write_register(&vdp, 0, 0x24);
	karashi_smsvdp_begin_line(&vdp, 8, row);
	assert(row[0] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	assert(row[8] == KARASHI_PIXEL(0xff, 0x00, 0x00));
}

static void place_sprite(struct karashi_smsvdp *vdp, unsigned sprite, uint8_t y, uint8_t x, uint8_t pattern) {
	unsigned table = 0x3f00;

	vdp->tms.vram[table + sprite] = y;
	vdp->tms.vram[table + 128 + sprite * 2] = x;
	vdp->tms.vram[table + 129 + sprite * 2] = pattern;
}

static void test_sprites(void) {
	struct karashi_smsvdp vdp;
	unsigned sprite;

	mode4(&vdp);
	write_color(&vdp, 1, 0x03);
	write_color(&vdp, 18, 0x30);
	solid_tile(&vdp, 1, 1);
	solid_tile(&vdp, 2, 2);
	vdp.tms.vram[0x3800 + 64 + 8] = 1;
	vdp.tms.vram[0x3800 + 64 + 9] = 0x10;
	place_sprite(&vdp, 0, 9, 20, 2);
	place_sprite(&vdp, 1, 9, 32, 2);
	place_sprite(&vdp, 2, 0xd0, 0, 0);
	karashi_smsvdp_begin_line(&vdp, 9, row);
	assert(row[20] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	karashi_smsvdp_begin_line(&vdp, 10, row);
	assert(row[20] == KARASHI_PIXEL(0x00, 0x00, 0xff));
	assert(row[27] == KARASHI_PIXEL(0x00, 0x00, 0xff));
	assert(row[28] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	assert(row[32] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(!(vdp.tms.status & 0x60));

	place_sprite(&vdp, 1, 9, 24, 2);
	karashi_smsvdp_begin_line(&vdp, 10, row);
	assert(vdp.tms.status & 0x20);
	assert((karashi_smsvdp_read_status(&vdp) & 0xe0) == 0x20);
	assert(!(vdp.tms.status & 0x20));

	for (sprite = 0; sprite < 9; sprite++) place_sprite(&vdp, sprite, 99, (uint8_t) (sprite * 16), 2);
	place_sprite(&vdp, 9, 0xd0, 0, 0);
	karashi_smsvdp_begin_line(&vdp, 100, row);
	assert(vdp.tms.status & 0x40);
	assert(row[7 * 16] == KARASHI_PIXEL(0x00, 0x00, 0xff));
	assert(row[8 * 16] == KARASHI_PIXEL(0x00, 0x00, 0x00));

	write_register(&vdp, 1, 0x41);
	place_sprite(&vdp, 0, 0xff, 40, 2);
	place_sprite(&vdp, 1, 0xd0, 0, 0);
	karashi_smsvdp_begin_line(&vdp, 15, row);
	assert(row[40] == KARASHI_PIXEL(0x00, 0x00, 0xff));
	assert(row[55] == KARASHI_PIXEL(0x00, 0x00, 0xff));
	assert(row[56] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	karashi_smsvdp_begin_line(&vdp, 16, row);
	assert(row[40] == KARASHI_PIXEL(0x00, 0x00, 0x00));
}

static void test_interrupts_and_counters(void) {
	struct karashi_smsvdp vdp;
	unsigned line;
	unsigned line_interrupts = 0;

	mode4(&vdp);
	write_register(&vdp, 0, 0x14);
	write_register(&vdp, 1, 0x60);
	write_register(&vdp, 10, 99);
	for (line = 193; line < 262; line++) karashi_smsvdp_end_line(&vdp, line);
	for (line = 0; line < 262; line++) {
		karashi_smsvdp_begin_line(&vdp, line, row);
		assert(karashi_smsvdp_read_vcounter(&vdp) == (line <= 0xda ? line : line - 6));
		karashi_smsvdp_end_line(&vdp, line);
		if (vdp.line_pending) {
			assert(line == 98);
			assert(karashi_smsvdp_interrupt(&vdp));
			karashi_smsvdp_read_status(&vdp);
			line_interrupts++;
		}
		if (line == 191) assert(!karashi_smsvdp_interrupt(&vdp));
		if (line == 192) {
			assert(karashi_smsvdp_interrupt(&vdp));
			assert(karashi_smsvdp_read_status(&vdp) & 0x80);
			assert(!karashi_smsvdp_interrupt(&vdp));
		}
	}
	assert(line_interrupts == 1);

	karashi_smsvdp_latch_hcounter(&vdp, 0);
	assert(karashi_smsvdp_read_hcounter(&vdp) == 0x00);
	karashi_smsvdp_latch_hcounter(&vdp, 227);
	assert(karashi_smsvdp_read_hcounter(&vdp) == 0xff);
}

static void test_pal_and_tall_modes(void) {
	struct karashi_smsvdp vdp;
	unsigned line;

	karashi_smsvdp_reset(&vdp, false, true);
	write_register(&vdp, 0, 0x04);
	write_register(&vdp, 1, 0x60);
	for (line = 0; line < KARASHI_SEGA8_PAL_LINES; line++) {
		karashi_smsvdp_begin_line(&vdp, line, line < KARASHI_SMSVDP_MAX_HEIGHT ? row : NULL);
		assert(karashi_smsvdp_read_vcounter(&vdp) == (uint8_t) (line <= 0xf2 ? line : line - 57));
		karashi_smsvdp_end_line(&vdp, line);
		assert(karashi_smsvdp_interrupt(&vdp) == (line >= 192));
	}
	assert(karashi_smsvdp_read_vcounter(&vdp) == 0xff);

	mode4(&vdp);
	write_register(&vdp, 0, 0x06);
	write_register(&vdp, 1, 0x70);
	write_color(&vdp, 1, 0x03);
	solid_tile(&vdp, 1, 1);
	vdp.tms.vram[0x3700 + 27 * 64] = 1;
	vdp.tms.vram[0x3f00] = 0xd0;
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(vdp.height == 224);
	karashi_smsvdp_begin_line(&vdp, 216, row);
	assert(row[0] == KARASHI_PIXEL(0xff, 0x00, 0x00));
	assert(row[8] == KARASHI_PIXEL(0x00, 0x00, 0x00));
	for (line = 0; line < 224; line++) karashi_smsvdp_end_line(&vdp, line);
	assert(!karashi_smsvdp_interrupt(&vdp));
	karashi_smsvdp_end_line(&vdp, 224);
	assert(karashi_smsvdp_interrupt(&vdp));
	karashi_smsvdp_begin_line(&vdp, 240, NULL);
	assert(karashi_smsvdp_read_vcounter(&vdp) == 0xea);

	write_register(&vdp, 1, 0x40);
	karashi_smsvdp_begin_line(&vdp, 0, row);
	assert(vdp.height == 192);
}

static void test_state(void) {
	static struct karashi_smsvdp vdp;
	static struct karashi_smsvdp copy;
	static uint8_t buffer[32768];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;

	mode4(&vdp);
	write_color(&vdp, 5, 0x2a);
	write_register(&vdp, 10, 77);
	vdp.tms.vram[0x2000] = 0x99;
	assert(karashi_state_writer_begin(&writer, buffer, sizeof(buffer), 2, 1));
	assert(karashi_smsvdp_save(&vdp, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	karashi_smsvdp_reset(&copy, false, false);
	assert(karashi_state_reader_begin(&reader, buffer, size));
	assert(karashi_smsvdp_load(&copy, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(memcmp(&vdp.tms, &copy.tms, sizeof(vdp.tms)) == 0);
	assert(memcmp(vdp.cram, copy.cram, sizeof(vdp.cram)) == 0);
	assert(memcmp(vdp.colors, copy.colors, sizeof(vdp.colors)) == 0);
	assert(memcmp(vdp.extended, copy.extended, sizeof(vdp.extended)) == 0);
	assert(copy.line_counter == vdp.line_counter && copy.code == vdp.code && copy.height == vdp.height);
}

int main(void) {
	test_ports_and_palette();
	test_background();
	test_sprites();
	test_interrupts_and_counters();
	test_pal_and_tall_modes();
	test_state();
	puts("SMS VDP palette, mode 4, sprites, interrupts, counters and state: pass");
	return 0;
}
