#include "mdvdp.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RED KARASHI_PIXEL(255, 0, 0)
#define GREEN KARASHI_PIXEL(0, 255, 0)
#define BLUE KARASHI_PIXEL(0, 0, 255)
#define BLACK KARASHI_PIXEL(0, 0, 0)

static uint16_t source[32];

static uint16_t dma_source(void *context, uint32_t address) {
	(void) context;
	return source[(address >> 1) & 31];
}

static void command(struct karashi_mdvdp *vdp, unsigned code, unsigned address) {
	karashi_mdvdp_write_control(vdp, (uint16_t) ((code & 3) << 14 | (address & 0x3fff)));
	karashi_mdvdp_write_control(vdp, (uint16_t) ((code & 0x3c) << 2 | ((address >> 14) & 3)));
}

static void tile(struct karashi_mdvdp *vdp, unsigned index, uint16_t row) {
	unsigned line;

	command(vdp, 1, index * 32);
	for (line = 0; line < 16; line++) karashi_mdvdp_write_data(vdp, row);
}

static void name(struct karashi_mdvdp *vdp, unsigned address, uint16_t value) {
	command(vdp, 1, address);
	karashi_mdvdp_write_data(vdp, value);
}

static void colour(struct karashi_mdvdp *vdp, unsigned index, uint16_t value) {
	command(vdp, 3, index * 2);
	karashi_mdvdp_write_data(vdp, value);
}

static void setup(struct karashi_mdvdp *vdp) {
	memset(vdp, 0, sizeof(*vdp));
	vdp->dma_read = dma_source;
	karashi_mdvdp_reset(vdp);
	karashi_mdvdp_write_control(vdp, 0x8004);
	karashi_mdvdp_write_control(vdp, 0x8144);
	karashi_mdvdp_write_control(vdp, 0x8230);
	karashi_mdvdp_write_control(vdp, 0x8328);
	karashi_mdvdp_write_control(vdp, 0x8407);
	karashi_mdvdp_write_control(vdp, 0x8578);
	karashi_mdvdp_write_control(vdp, 0x8c81);
	karashi_mdvdp_write_control(vdp, 0x8d3f);
	karashi_mdvdp_write_control(vdp, 0x8f02);
	karashi_mdvdp_write_control(vdp, 0x9001);
	colour(vdp, 1, 0x000e);
	colour(vdp, 2, 0x00e0);
	colour(vdp, 3, 0x0e00);
	tile(vdp, 1, 0x1111);
	tile(vdp, 2, 0x2222);
	tile(vdp, 3, 0x3333);
}

static void test_planes_and_priority(void) {
	static struct karashi_mdvdp vdp;
	karashi_pixel line[KARASHI_MD_WIDTH];
	unsigned index;

	setup(&vdp);
	assert(karashi_mdvdp_width(&vdp) == 320 && karashi_mdvdp_height(&vdp) == 224);
	name(&vdp, 0xe000, 0x0002);
	karashi_mdvdp_render_line(&vdp, 0, line);
	for (index = 0; index < 8; index++) assert(line[index] == GREEN);
	assert(line[8] == BLACK);
	name(&vdp, 0xc000, 0x0001);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == RED);
	name(&vdp, 0xe000, 0x8002);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == GREEN);
	name(&vdp, 0xc000, 0x8001);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == RED);
	karashi_mdvdp_write_control(&vdp, 0x8104);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == BLACK);
}

static void test_scroll_and_window(void) {
	static struct karashi_mdvdp vdp;
	karashi_pixel line[KARASHI_MD_WIDTH];

	setup(&vdp);
	name(&vdp, 0xc000, 0x0001);
	name(&vdp, 0xfc00, 0x0004);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[3] == BLACK && line[4] == RED && line[11] == RED && line[12] == BLACK);
	command(&vdp, 5, 0);
	karashi_mdvdp_write_data(&vdp, 8);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[4] == BLACK);
	karashi_mdvdp_render_line(&vdp, 248, line);
	assert(line[4] == RED);
	command(&vdp, 5, 0);
	karashi_mdvdp_write_data(&vdp, 0);
	name(&vdp, 0xa000, 0x0003);
	karashi_mdvdp_write_control(&vdp, 0x9101);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == BLUE && line[7] == BLUE && line[8] == BLACK);
	assert(line[16] == BLACK);
	karashi_mdvdp_write_control(&vdp, 0x9100);
	karashi_mdvdp_write_control(&vdp, 0x9201);
	karashi_mdvdp_render_line(&vdp, 7, line);
	assert(line[0] == BLUE);
	karashi_mdvdp_render_line(&vdp, 8, line);
	assert(line[0] == BLACK);
}

static void sprite(struct karashi_mdvdp *vdp, unsigned index, unsigned y, unsigned size, unsigned link,
	uint16_t attributes, unsigned x) {
	command(vdp, 1, 0xf000 + index * 8);
	karashi_mdvdp_write_data(vdp, (uint16_t) y);
	karashi_mdvdp_write_data(vdp, (uint16_t) (size << 8 | link));
	karashi_mdvdp_write_data(vdp, attributes);
	karashi_mdvdp_write_data(vdp, (uint16_t) x);
}

static void test_sprites(void) {
	static struct karashi_mdvdp vdp;
	karashi_pixel line[KARASHI_MD_WIDTH];
	unsigned index;

	setup(&vdp);
	sprite(&vdp, 0, 128, 0, 1, 0x0003, 128);
	sprite(&vdp, 1, 128, 0, 0, 0x0001, 132);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == BLUE && line[7] == BLUE && line[8] == RED && line[11] == RED && line[12] == BLACK);
	assert(karashi_mdvdp_read_control(&vdp) & 0x0020);
	assert(!(karashi_mdvdp_read_control(&vdp) & 0x0020));
	name(&vdp, 0xc000, 0x8002);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == GREEN && line[8] == RED);
	sprite(&vdp, 0, 128, 0, 1, 0x8003, 128);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == BLUE);
	for (index = 0; index < 22; index++) sprite(&vdp, index, 128, 0, index + 1, 0x0003, 128 + index * 8);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[19 * 8] == BLUE && line[20 * 8] == BLACK);
	assert(karashi_mdvdp_read_control(&vdp) & 0x0040);
	sprite(&vdp, 0, 128, 0, 1, 0x0003, 200);
	sprite(&vdp, 1, 128, 0, 2, 0x0003, 0);
	sprite(&vdp, 2, 128, 0, 0, 0x0003, 228);
	name(&vdp, 0xc000, 0);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[72] == BLUE && line[100] == BLACK);
}

static void test_shadow_and_highlight(void) {
	static struct karashi_mdvdp vdp;
	karashi_pixel line[KARASHI_MD_WIDTH];

	setup(&vdp);
	karashi_mdvdp_write_control(&vdp, 0x8c89);
	name(&vdp, 0xc000, 0x0001);
	name(&vdp, 0xc002, 0x8001);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[0] == KARASHI_PIXEL(127, 0, 0));
	assert(line[8] == RED);
	colour(&vdp, 0x3e, 0x0eee);
	tile(&vdp, 4, 0xeeee);
	sprite(&vdp, 0, 128, 0, 0, 0xe004, 136);
	karashi_mdvdp_render_line(&vdp, 0, line);
	assert(line[8] == KARASHI_PIXEL(255, 127, 127));
}

static void test_ports_and_dma(void) {
	static struct karashi_mdvdp vdp;
	unsigned index;

	setup(&vdp);
	command(&vdp, 0, 32);
	assert(karashi_mdvdp_read_data(&vdp) == 0x1111);
	command(&vdp, 8, 2);
	assert(karashi_mdvdp_read_data(&vdp) == 0x000e);
	for (index = 0; index < 32; index++) source[index] = (uint16_t) (0x1000 + index);
	karashi_mdvdp_write_control(&vdp, 0x8154);
	karashi_mdvdp_write_control(&vdp, 0x9314);
	karashi_mdvdp_write_control(&vdp, 0x9400);
	karashi_mdvdp_write_control(&vdp, 0x9502);
	karashi_mdvdp_write_control(&vdp, 0x9600);
	karashi_mdvdp_write_control(&vdp, 0x9700);
	karashi_mdvdp_write_control(&vdp, 0x5000);
	{
		unsigned cycles = karashi_mdvdp_write_control_at(&vdp, 0x0080, 0, 100);
		assert(cycles > 0);
		assert(karashi_mdvdp_read_status_at(&vdp, 0, 100) & 0x0002);
		assert(!(karashi_mdvdp_read_status_at(&vdp, 0, 100 + (uint64_t) cycles * 7) & 0x0002));
	}
	assert(vdp.vram[0x1000] == 0x10 && vdp.vram[0x1001] == 0x02 && vdp.vram[0x1007] == 0x05);
	assert(vdp.registers[19] == 0 && vdp.registers[21] == 22);
	karashi_mdvdp_write_control(&vdp, 0x8f01);
	karashi_mdvdp_write_control(&vdp, 0x9308);
	karashi_mdvdp_write_control(&vdp, 0x9780);
	command(&vdp, 0x21, 0x2000);
	karashi_mdvdp_write_data(&vdp, 0xab00);
	assert(vdp.vram[0x2000] == 0xab && vdp.vram[0x2001] == 0x00);
	for (index = 2; index < 8; index++) assert(vdp.vram[0x2000 + index] == 0xab);
	karashi_mdvdp_write_control(&vdp, 0x9304);
	karashi_mdvdp_write_control(&vdp, 0x9500);
	karashi_mdvdp_write_control(&vdp, 0x9610);
	karashi_mdvdp_write_control(&vdp, 0x97c0);
	command(&vdp, 0x30, 0x3000);
	assert(vdp.vram[0x3000] == 0x10 && vdp.vram[0x3001] == 0x02 && vdp.vram[0x3003] == 0x03);
}

static void test_fifo_timing(void) {
	static struct karashi_mdvdp vdp;
	uint64_t last_finish;
	uint64_t vram_time;
	uint64_t cram_time;
	unsigned index;

	setup(&vdp);
	karashi_mdvdp_begin_line(&vdp, 0);
	command(&vdp, 1, 0x2000);
	for (index = 0; index < 4; index++) {
		assert(karashi_mdvdp_write_data_at(&vdp, (uint16_t) index, 100, 100) == 0);
	}
	assert((karashi_mdvdp_read_status_at(&vdp, 100, 100) & 0x0300) == 0x0100);
	assert(karashi_mdvdp_write_data_at(&vdp, 4, 100, 100) > 0);
	assert(vdp.fifo_count == 4);
	last_finish = vdp.fifo_finish[(vdp.fifo_head + vdp.fifo_count - 1) & 3];
	assert((karashi_mdvdp_read_status_at(&vdp, 100, last_finish) & 0x0300) == 0x0200);

	setup(&vdp);
	karashi_mdvdp_begin_line(&vdp, 0);
	command(&vdp, 1, 0);
	assert(karashi_mdvdp_write_data_at(&vdp, 0x1234, 0, 100) == 0);
	vram_time = vdp.fifo_finish[vdp.fifo_head] - 100;
	assert(vram_time == 284);
	setup(&vdp);
	karashi_mdvdp_begin_line(&vdp, 0);
	command(&vdp, 3, 0);
	assert(karashi_mdvdp_write_data_at(&vdp, 0x000e, 0, 100) == 0);
	cram_time = vdp.fifo_finish[vdp.fifo_head] - 100;
	assert(cram_time == 156);
	assert(vram_time > cram_time);

	karashi_mdvdp_write_control(&vdp, 0x8104);
	command(&vdp, 1, 0);
	assert(karashi_mdvdp_write_data_at(&vdp, 0, 0, 10000) == 0);
	assert((karashi_mdvdp_read_status_at(&vdp, 0, 10000) & 0x0300) == 0x0200);
}

static void test_interrupts_and_counters(void) {
	static struct karashi_mdvdp vdp;
	static struct karashi_mdvdp restored;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	static uint8_t state[80 * 1024];
	size_t size;
	unsigned line;

	setup(&vdp);
	karashi_mdvdp_write_control(&vdp, 0x8014);
	karashi_mdvdp_write_control(&vdp, 0x8164);
	karashi_mdvdp_write_control(&vdp, 0x8a01);
	assert(karashi_mdvdp_begin_line(&vdp, 0) == 0);
	karashi_mdvdp_end_active(&vdp);
	assert(karashi_mdvdp_irq_level(&vdp) == 4);
	karashi_mdvdp_acknowledge(&vdp, 4);
	assert(karashi_mdvdp_irq_level(&vdp) == 0);
	karashi_mdvdp_begin_line(&vdp, 1);
	karashi_mdvdp_end_active(&vdp);
	assert(karashi_mdvdp_irq_level(&vdp) == 0);
	karashi_mdvdp_begin_line(&vdp, 2);
	karashi_mdvdp_end_active(&vdp);
	assert(karashi_mdvdp_irq_level(&vdp) == 4);
	karashi_mdvdp_acknowledge(&vdp, 4);
	assert(karashi_mdvdp_begin_line(&vdp, 224) == 0);
	karashi_mdvdp_raise_vint(&vdp);
	assert(karashi_mdvdp_irq_level(&vdp) == 6);
	assert((karashi_mdvdp_read_control(&vdp) & 0x0088) == 0x0088);
	karashi_mdvdp_acknowledge(&vdp, 6);
	assert((karashi_mdvdp_read_control(&vdp) & 0x0088) == 0x0008);
	karashi_mdvdp_begin_line(&vdp, 224);
	karashi_mdvdp_acknowledge(&vdp, 6);
	assert((karashi_mdvdp_read_control(&vdp) & 0x0088) == 0x0088);
	assert(karashi_mdvdp_hv_counter_at(&vdp, 0) == 0xe000);
	assert(karashi_mdvdp_hv_counter_at(&vdp, KARASHI_MD_HBLANK_MCLK) >> 8 == 0xe1);
	karashi_mdvdp_begin_line(&vdp, 240);
	assert(karashi_mdvdp_hv_counter_at(&vdp, 0) >> 8 == 0xea);
	assert(karashi_mdvdp_hv_counter_at(&vdp, 16 * 0xb6) == 0xebb6);
	assert(karashi_mdvdp_hv_counter_at(&vdp, 16 * 0xb7) == 0xebe4);
	karashi_mdvdp_begin_line(&vdp, 261);
	assert(karashi_mdvdp_hv_counter_at(&vdp, 0) >> 8 == 0xff);
	assert(karashi_mdvdp_read_status(&vdp, 3000) & 0x0004);
	assert(!(karashi_mdvdp_read_status(&vdp, 1000) & 0x0004));
	for (line = 0; line < 3; line++) karashi_mdvdp_begin_line(&vdp, line);
	assert(!(karashi_mdvdp_read_control(&vdp) & 0x0008));
	karashi_mdvdp_set_pal(&vdp, true);
	karashi_mdvdp_write_control(&vdp, 0x814c);
	assert(karashi_mdvdp_height(&vdp) == 240 && vdp.lines == 313);
	assert(karashi_mdvdp_read_control(&vdp) & 0x0001);
	vdp.fifo_finish[0] = 400;
	vdp.fifo_finish[2] = 200;
	vdp.fifo_finish[3] = 300;
	vdp.fifo_head = 2;
	vdp.fifo_count = 3;
	vdp.dma_finish = 900;
	vdp.port_clock = 500;

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_mdvdp_save(&vdp, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	restored = vdp;
	memset(restored.vram, 0, sizeof(restored.vram));
	memset(restored.palette, 0, sizeof(restored.palette));
	memset(restored.fifo_finish, 0, sizeof(restored.fifo_finish));
	restored.fifo_head = 0;
	restored.fifo_count = 0;
	restored.dma_finish = 0;
	restored.port_clock = 0;
	assert(karashi_state_reader_begin(&reader, state, size));
	assert(karashi_mdvdp_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(memcmp(restored.vram, vdp.vram, sizeof(vdp.vram)) == 0);
	assert(memcmp(restored.palette, vdp.palette, sizeof(vdp.palette)) == 0);
	assert(restored.registers[1] == 0x4c);
	assert(restored.fifo_finish[0] == 400 && restored.fifo_finish[2] == 200);
	assert(restored.fifo_head == 2 && restored.fifo_count == 3);
	assert(restored.dma_finish == 900);
	assert(restored.port_clock == 500);
}

int main(void) {
	test_planes_and_priority();
	test_scroll_and_window();
	test_sprites();
	test_shadow_and_highlight();
	test_ports_and_dma();
	test_fifo_timing();
	test_interrupts_and_counters();
	puts("Mega Drive VDP planes, window, sprites, shadow, DMA, interrupts, counters and state: pass");
	return 0;
}
