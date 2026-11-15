#include "tms9918.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static karashi_pixel frame[KARASHI_TMS9918_WIDTH * KARASHI_TMS9918_HEIGHT];

static void write_register(struct karashi_tms9918 *vdp, unsigned index, uint8_t value) {
	karashi_tms9918_write_control(vdp, value);
	karashi_tms9918_write_control(vdp, (uint8_t) (0x80 | index));
}

static void set_write_address(struct karashi_tms9918 *vdp, uint16_t address) {
	karashi_tms9918_write_control(vdp, (uint8_t) address);
	karashi_tms9918_write_control(vdp, (uint8_t) (0x40 | (address >> 8)));
}

static void set_read_address(struct karashi_tms9918 *vdp, uint16_t address) {
	karashi_tms9918_write_control(vdp, (uint8_t) address);
	karashi_tms9918_write_control(vdp, (uint8_t) (address >> 8));
}

static void test_vram_and_interrupt(void) {
	struct karashi_tms9918 vdp;

	karashi_tms9918_reset(&vdp);
	set_write_address(&vdp, 0x3ffe);
	karashi_tms9918_write_data(&vdp, 0x12);
	karashi_tms9918_write_data(&vdp, 0x34);
	karashi_tms9918_write_data(&vdp, 0x56);
	assert(vdp.vram[0x3ffe] == 0x12);
	assert(vdp.vram[0x3fff] == 0x34);
	assert(vdp.vram[0] == 0x56);
	set_read_address(&vdp, 0x3ffe);
	assert(karashi_tms9918_read_data(&vdp) == 0x12);
	assert(karashi_tms9918_read_data(&vdp) == 0x34);
	assert(karashi_tms9918_read_data(&vdp) == 0x56);
	write_register(&vdp, 1, 0x20);
	karashi_tms9918_vblank(&vdp);
	assert(karashi_tms9918_interrupt(&vdp));
	assert(karashi_tms9918_read_status(&vdp) == 0x80);
	assert(!karashi_tms9918_interrupt(&vdp));
}

static void test_graphics_one(void) {
	struct karashi_tms9918 vdp;

	karashi_tms9918_reset(&vdp);
	write_register(&vdp, 1, 0x40);
	write_register(&vdp, 2, 0x06);
	write_register(&vdp, 3, 0x80);
	write_register(&vdp, 4, 0x00);
	write_register(&vdp, 7, 0x04);
	vdp.vram[0x1800] = 1;
	vdp.vram[8] = 0x80;
	vdp.vram[0x2000] = 0xf4;
	memset(frame, 0, sizeof(frame));
	karashi_tms9918_render(&vdp, frame, KARASHI_TMS9918_WIDTH);
	assert(frame[0] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(frame[1] == KARASHI_PIXEL(0x54, 0x55, 0xed));
	assert(frame[8] == KARASHI_PIXEL(0x54, 0x55, 0xed));
}

static void test_graphics_two_and_multicolor(void) {
	struct karashi_tms9918 vdp;

	karashi_tms9918_reset(&vdp);
	write_register(&vdp, 0, 0x02);
	write_register(&vdp, 1, 0x40);
	write_register(&vdp, 2, 0x06);
	write_register(&vdp, 3, 0xff);
	write_register(&vdp, 4, 0x03);
	write_register(&vdp, 7, 0x01);
	vdp.vram[0x1800] = 1;
	vdp.vram[0x1800 + 8 * 32] = 1;
	vdp.vram[0x0008] = 0x80;
	vdp.vram[0x2008] = 0xf4;
	vdp.vram[0x0808] = 0x40;
	vdp.vram[0x2808] = 0x6a;
	karashi_tms9918_render(&vdp, frame, KARASHI_TMS9918_WIDTH);
	assert(frame[0] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(frame[1] == KARASHI_PIXEL(0x54, 0x55, 0xed));
	assert(frame[64 * KARASHI_TMS9918_WIDTH + 0] == KARASHI_PIXEL(0xd4, 0xc1, 0x54));
	assert(frame[64 * KARASHI_TMS9918_WIDTH + 1] == KARASHI_PIXEL(0xd4, 0x52, 0x4d));

	write_register(&vdp, 4, 0x00);
	write_register(&vdp, 3, 0x80);
	karashi_tms9918_render(&vdp, frame, KARASHI_TMS9918_WIDTH);
	assert(frame[64 * KARASHI_TMS9918_WIDTH + 0] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(frame[64 * KARASHI_TMS9918_WIDTH + 1] == KARASHI_PIXEL(0x54, 0x55, 0xed));

	karashi_tms9918_reset(&vdp);
	write_register(&vdp, 1, 0x48);
	write_register(&vdp, 2, 0x06);
	write_register(&vdp, 4, 0x00);
	vdp.vram[0x1800] = 1;
	vdp.vram[0x1800 + 32] = 1;
	vdp.vram[0x0008] = 0x4f;
	vdp.vram[0x0009] = 0x6a;
	vdp.vram[0x000a] = 0xf4;
	karashi_tms9918_render(&vdp, frame, KARASHI_TMS9918_WIDTH);
	assert(frame[0] == KARASHI_PIXEL(0x54, 0x55, 0xed));
	assert(frame[4] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(frame[4 * KARASHI_TMS9918_WIDTH] == KARASHI_PIXEL(0xd4, 0x52, 0x4d));
	assert(frame[4 * KARASHI_TMS9918_WIDTH + 4] == KARASHI_PIXEL(0xd4, 0xc1, 0x54));
	assert(frame[8 * KARASHI_TMS9918_WIDTH] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(frame[8 * KARASHI_TMS9918_WIDTH + 4] == KARASHI_PIXEL(0x54, 0x55, 0xed));
}

static void test_sprites(void) {
	struct karashi_tms9918 vdp;
	unsigned attributes = 0x1b00;

	karashi_tms9918_reset(&vdp);
	write_register(&vdp, 1, 0x40);
	write_register(&vdp, 5, 0x36);
	write_register(&vdp, 6, 0x00);
	vdp.vram[8] = 0x80;
	vdp.vram[attributes] = 9;
	vdp.vram[attributes + 1] = 20;
	vdp.vram[attributes + 2] = 1;
	vdp.vram[attributes + 3] = 0x0f;
	vdp.vram[attributes + 4] = 9;
	vdp.vram[attributes + 5] = 20;
	vdp.vram[attributes + 6] = 1;
	vdp.vram[attributes + 7] = 0x06;
	vdp.vram[attributes + 8] = 0xd0;
	karashi_tms9918_render(&vdp, frame, KARASHI_TMS9918_WIDTH);
	assert(frame[10 * KARASHI_TMS9918_WIDTH + 20] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(vdp.status & 0x20);
	assert(karashi_tms9918_read_status(&vdp) & 0x20);
}

static void test_fifth_sprite(void) {
	struct karashi_tms9918 vdp;
	unsigned attributes = 0x1b00;
	unsigned sprite;

	karashi_tms9918_reset(&vdp);
	write_register(&vdp, 1, 0x40);
	write_register(&vdp, 5, 0x36);
	for (sprite = 0; sprite < 5; sprite++) {
		vdp.vram[attributes + sprite * 4] = 19;
		vdp.vram[attributes + sprite * 4 + 1] = (uint8_t) (sprite * 8);
		vdp.vram[attributes + sprite * 4 + 2] = 0;
		vdp.vram[attributes + sprite * 4 + 3] = 1;
	}
	vdp.vram[attributes + 20] = 0xd0;
	karashi_tms9918_render(&vdp, frame, KARASHI_TMS9918_WIDTH);
	assert((vdp.status & 0x5f) == 0x44);
}

int main(void) {
	test_vram_and_interrupt();
	test_graphics_one();
	test_graphics_two_and_multicolor();
	test_sprites();
	test_fifth_sprite();
	puts("TMS9918 VRAM, display, sprites and interrupt: pass");
	return 0;
}
