#include "mars.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct karashi_mars mars;
static struct karashi_mars restored;
static uint8_t rom[0x1000];
static uint8_t master_boot[0x800];
static uint8_t slave_boot[0x400];
static uint8_t boot_68k[0x100];
static uint8_t state[1024 * 1024];

static void put16(uint8_t *data, uint32_t address, uint16_t value) {
	data[address] = (uint8_t) (value >> 8);
	data[address + 1] = (uint8_t) value;
}

static void put32(uint8_t *data, uint32_t address, uint32_t value) {
	put16(data, address, (uint16_t) (value >> 16));
	put16(data, address + 2, (uint16_t) value);
}

static void build_boot(uint8_t *boot, uint32_t comm, uint16_t value) {
	put32(boot, 0, 0x00000100);
	put32(boot, 4, 0x06040000);
	put16(boot, 0x100, 0xd102);
	put16(boot, 0x102, 0xd203);
	put16(boot, 0x104, 0x2121);
	put16(boot, 0x106, 0xaffe);
	put16(boot, 0x108, 0x0009);
	put16(boot, 0x10a, 0x0009);
	put32(boot, 0x10c, comm);
	put32(boot, 0x110, value);
}

static void setup(void) {
	memset(rom, 0xff, sizeof(rom));
	memset(boot_68k, 0, sizeof(boot_68k));
	put32(boot_68k, 0x70, 0x00880246);
	build_boot(master_boot, 0x20004020, 0x1234);
	build_boot(slave_boot, 0x20004022, 0x5678);
	karashi_mars_init(&mars, rom, sizeof(rom));
	karashi_mars_set_boot(&mars, boot_68k, master_boot, sizeof(master_boot), slave_boot, sizeof(slave_boot));
	karashi_mars_reset(&mars, false);
}

static void test_boot_and_communication(void) {
	setup();
	assert(karashi_mars_owns(&mars, 0xa15120) && karashi_mars_owns(&mars, 0xa130ec));
	assert(!karashi_mars_owns(&mars, 0x000000) && !karashi_mars_owns(&mars, 0x880000));
	assert(karashi_mars_read16(&mars, 0xa130ec) == 0x4d41 && karashi_mars_read16(&mars, 0xa130ee) == 0x5253);
	karashi_mars_write16(&mars, 0xa15100, 0x0003);
	assert(karashi_mars_owns(&mars, 0x000000) && karashi_mars_owns(&mars, 0x880000));
	assert(karashi_mars_read16(&mars, 0x000072) == 0x0246);
	karashi_mars_write16(&mars, 0x000072, 0x1000);
	assert(karashi_mars_read16(&mars, 0x000072) == 0x1000 && karashi_mars_read16(&mars, 0x000070) == 0x0088);
	karashi_mars_run(&mars, 20000);
	assert(karashi_mars_read16(&mars, 0xa15120) == 0x1234);
	assert(karashi_mars_read16(&mars, 0xa15122) == 0x5678);
	assert(mars.cpu[0].r[15] == 0x06040000);
	karashi_mars_write16(&mars, 0xa15124, 0xbeef);
	assert(mars.comm[2] == 0xbeef);
	mars.interrupt_mask[0] = 0x0002;
	karashi_mars_write16(&mars, 0xa15102, 0x0001);
	assert(mars.cpu[0].irl == 8 && mars.cpu[1].irl == 0);
	karashi_mars_write16(&mars, 0xa15102, 0x0000);
	assert(mars.cpu[0].irl == 0);
}

static void test_frame_buffer(void) {
	karashi_pixel line[KARASHI_MARS_WIDTH];
	uint8_t backdrop[KARASHI_MARS_WIDTH];
	unsigned x;

	setup();
	karashi_mars_write16(&mars, 0xa15100, 0x0001);
	karashi_mars_begin_line(&mars, 0, 224, 0);
	karashi_mars_write16(&mars, 0xa15180, 0x0001);
	karashi_mars_write16(&mars, 0xa1520a, 0x001f);
	karashi_mars_write16(&mars, 0xa1520c, 0x83e0);
	karashi_mars_write16(&mars, 0x840000, 0x0100);
	karashi_mars_write16(&mars, 0x840200, 0x0505);
	karashi_mars_write16(&mars, 0x840202, 0x0600);
	karashi_mars_write8(&mars, 0x840203, 0x05);
	karashi_mars_write8(&mars, 0x840202, 0x00);
	karashi_mars_write16(&mars, 0x860204, 0x0500);
	karashi_mars_write16(&mars, 0x840206, 0x0506);
	karashi_mars_write16(&mars, 0x86020a, 0x0000);
	assert(mars.frame[1][0x204] == 0x05 && mars.frame[1][0x205] == 0x00);
	assert(mars.frame[1][0x202] == 0x06 && mars.frame[1][0x203] == 0x05);
	karashi_mars_write16(&mars, 0xa1518a, 0x0001);
	assert(mars.display == 0 && mars.swap_pending);
	assert(!(karashi_mars_read16(&mars, 0xa1518a) & 1));
	karashi_mars_begin_line(&mars, 224, 224, 0);
	assert(mars.display == 1 && (karashi_mars_read16(&mars, 0xa1518a) & 0x8001) == 0x8001);
	for (x = 0; x < KARASHI_MARS_WIDTH; x++) {
		line[x] = KARASHI_PIXEL(0, 0, 255);
		backdrop[x] = x != 2 && x != 4;
	}
	karashi_mars_render_line(&mars, 0, line, backdrop);
	assert(line[0] == KARASHI_PIXEL(255, 0, 0) && line[1] == KARASHI_PIXEL(255, 0, 0));
	assert(line[2] == KARASHI_PIXEL(0, 255, 0));
	assert(line[4] == KARASHI_PIXEL(0, 0, 255));
	karashi_mars_write16(&mars, 0xa15180, 0x0081);
	for (x = 0; x < KARASHI_MARS_WIDTH; x++) {
		line[x] = KARASHI_PIXEL(0, 0, 255);
		backdrop[x] = 0;
	}
	karashi_mars_render_line(&mars, 0, line, backdrop);
	assert(line[0] == KARASHI_PIXEL(255, 0, 0) && line[4] == KARASHI_PIXEL(255, 0, 0));
	assert(line[2] == KARASHI_PIXEL(0, 0, 255));
	backdrop[2] = 1;
	karashi_mars_render_line(&mars, 0, line, backdrop);
	assert(line[2] == KARASHI_PIXEL(0, 255, 0));
	karashi_mars_write16(&mars, 0xa15184, 0x0003);
	karashi_mars_write16(&mars, 0xa15186, 0x01fe);
	karashi_mars_write16(&mars, 0xa15188, 0x1234);
	assert(mars.frame[0][0x3fc] == 0x12 && mars.frame[0][0x3fe] == 0x12);
	assert(mars.frame[0][0x200] == 0x12 && mars.frame[0][0x202] == 0x12 && mars.frame[0][0x204] == 0x00);
	assert(karashi_mars_height(&mars) == 224);
}

static void test_fifo_and_pwm(void) {
	unsigned index;

	setup();
	karashi_mars_write16(&mars, 0xa15100, 0x0003);
	karashi_mars_run(&mars, 20000);
	mars.cpu[0].dma[0].source = 0x20004012;
	mars.cpu[0].dma[0].destination = 0x06001000;
	mars.cpu[0].dma[0].count = 4;
	mars.cpu[0].dma[0].control = 0x4405;
	mars.cpu[0].dma_operation = 1;
	karashi_mars_write16(&mars, 0xa15110, 0x0004);
	karashi_mars_write16(&mars, 0xa15106, 0x0004);
	assert((karashi_mars_read16(&mars, 0xa15106) & 0x0084) == 0x0004);
	for (index = 0; index < 4; index++) karashi_mars_write16(&mars, 0xa15112, (uint16_t) (0x1110 * (index + 1)));
	assert(mars.sdram[0x1000] == 0x11 && mars.sdram[0x1006] == 0x44 && mars.sdram[0x1007] == 0x40);
	assert(!(karashi_mars_read16(&mars, 0xa15106) & 0x0004));
	assert(mars.cpu[0].dma[0].control & 2);
	karashi_mars_write16(&mars, 0xa15106, 0x0004);
	for (index = 0; index < 8; index++) karashi_mars_write16(&mars, 0xa15112, 0x1111);
	assert(karashi_mars_read16(&mars, 0xa15106) & 0x0080);
	karashi_mars_write16(&mars, 0xa15106, 0x0000);
	assert(!(karashi_mars_read16(&mars, 0xa15106) & 0x0080));

	karashi_mars_write16(&mars, 0xa15132, 0x0418);
	karashi_mars_write16(&mars, 0xa15130, 0x0105);
	for (index = 0; index < 3; index++) karashi_mars_write16(&mars, 0xa15138, 0x0300);
	assert(karashi_mars_read16(&mars, 0xa15138) & 0x8000);
	mars.interrupt_mask[1] = 0x0001;
	karashi_mars_run(&mars, 40000);
	assert(karashi_mars_read16(&mars, 0xa15138) & 0x4000);
	assert(mars.pwm_frames >= 3 && mars.pwm_audio[0] > 0 && mars.pwm_audio[1] > 0);
	assert(mars.pending[1] & 1);
}

static void test_state(void) {
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;

	setup();
	karashi_mars_write16(&mars, 0xa15100, 0x0003);
	karashi_mars_run(&mars, 20000);
	karashi_mars_write16(&mars, 0xa15126, 0x4321);
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 5, 1));
	assert(karashi_mars_save(&mars, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	restored = mars;
	memset(restored.sdram, 0, sizeof(restored.sdram));
	memset(restored.comm, 0, sizeof(restored.comm));
	assert(karashi_state_reader_begin(&reader, state, size));
	assert(karashi_mars_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.comm[3] == 0x4321 && restored.comm[0] == 0x1234);
	assert(memcmp(restored.sdram, mars.sdram, sizeof(mars.sdram)) == 0);
	assert(restored.cpu[0].pc == mars.cpu[0].pc && restored.cpu[1].pc == mars.cpu[1].pc);
	assert(restored.cpu[0].bus.direct[0x04] == restored.frame[restored.display ^ 1]);
	assert(restored.cpu[1].bus.direct[0x04] == restored.frame[restored.display ^ 1]);
}

int main(void) {
	test_boot_and_communication();
	test_frame_buffer();
	test_fifo_and_pwm();
	test_state();
	puts("32X boot, communication, interrupts, frame buffers, priority, fill, FIFO DMA, PWM and state: pass");
	return 0;
}
