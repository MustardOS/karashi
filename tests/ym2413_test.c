#include "ym2413.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct karashi_ym2413 chip;

static void test_silicon_instruments(void) {
	static const uint8_t expected[19][8] = {
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0x71, 0x61, 0x1e, 0x17, 0xd0, 0x78, 0x00, 0x17},
		{0x13, 0x41, 0x1a, 0x0d, 0xd8, 0xf7, 0x23, 0x13},
		{0x13, 0x01, 0x99, 0x00, 0xf2, 0xc4, 0x11, 0x23},
		{0x31, 0x61, 0x0e, 0x07, 0xa8, 0x64, 0x70, 0x27},
		{0x32, 0x21, 0x1e, 0x06, 0xe0, 0x76, 0x00, 0x28},
		{0x31, 0x22, 0x16, 0x05, 0xe0, 0x71, 0x00, 0x18},
		{0x21, 0x61, 0x1d, 0x07, 0x82, 0x81, 0x10, 0x07},
		{0x23, 0x21, 0x2d, 0x14, 0xa2, 0x72, 0x00, 0x07},
		{0x61, 0x61, 0x1b, 0x06, 0x64, 0x65, 0x10, 0x17},
		{0x41, 0x61, 0x0b, 0x18, 0x85, 0xf7, 0x71, 0x07},
		{0x13, 0x01, 0x83, 0x11, 0xfa, 0xe4, 0x10, 0x04},
		{0x17, 0xc1, 0x24, 0x07, 0xf8, 0xf8, 0x22, 0x12},
		{0x61, 0x50, 0x0c, 0x05, 0xc2, 0xf5, 0x20, 0x42},
		{0x01, 0x01, 0x55, 0x03, 0xc9, 0x95, 0x03, 0x02},
		{0x61, 0x41, 0x89, 0x03, 0xf1, 0xe4, 0x40, 0x13},
		{0x01, 0x01, 0x18, 0x0f, 0xdf, 0xf8, 0x6a, 0x6d},
		{0x01, 0x01, 0x00, 0x00, 0xc8, 0xd8, 0xa7, 0x48},
		{0x05, 0x01, 0x00, 0x00, 0xf8, 0xaa, 0x59, 0x55}
	};

	assert(memcmp(karashi_ym2413_instruments, expected, sizeof(expected)) == 0);
}

static void write(uint8_t address, uint8_t value) {
	karashi_ym2413_write(&chip, false, address);
	karashi_ym2413_write(&chip, true, value);
}

static unsigned crossings(unsigned samples, int32_t *peak) {
	int32_t previous = karashi_ym2413_sample(&chip);
	unsigned count = 0;
	unsigned index;

	*peak = 0;
	for (index = 0; index < samples; index++) {
		int32_t value = karashi_ym2413_sample(&chip);
		if ((previous < 0) != (value < 0)) count++;
		if (abs(value) > *peak) *peak = abs(value);
		previous = value;
	}
	return count;
}

static void test_silence_and_pitch(void) {
	int32_t peak;
	unsigned count;
	unsigned index;

	karashi_ym2413_reset(&chip);
	for (index = 0; index < 1000; index++) assert(karashi_ym2413_sample(&chip) == 0);

	write(0x00, 0x01);
	write(0x01, 0x21);
	write(0x02, 0x3f);
	write(0x04, 0xff);
	write(0x05, 0xf0);
	write(0x06, 0x0f);
	write(0x07, 0x0f);
	write(0x30, 0x00);
	write(0x10, 0x20);
	write(0x20, 0x10 | (4 << 1) | 1);
	for (index = 0; index < 2000; index++) karashi_ym2413_sample(&chip);
	count = crossings(49716, &peak);
	assert(peak > 1500);
	assert(count > 2 * 430 && count < 2 * 445);

	write(0x20, (4 << 1) | 1);
	for (index = 0; index < 200000; index++) karashi_ym2413_sample(&chip);
	crossings(4000, &peak);
	assert(peak < 32);
}

static void test_rhythm(void) {
	int32_t peak;
	unsigned index;

	karashi_ym2413_reset(&chip);
	write(0x16, 0x20);
	write(0x26, 0x05);
	write(0x36, 0x00);
	write(0x0e, 0x30);
	for (index = 0; index < 100; index++) karashi_ym2413_sample(&chip);
	crossings(4000, &peak);
	assert(peak > 200);
}

static void test_state(void) {
	static uint8_t buffer[4096];
	struct karashi_ym2413 restored;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;
	unsigned index;

	karashi_ym2413_reset(&chip);
	write(0x31, 0x52);
	write(0x11, 0x81);
	write(0x21, 0x17);
	write(0x0e, 0x3f);
	for (index = 0; index < 3000; index++) karashi_ym2413_sample(&chip);
	assert(karashi_state_writer_begin(&writer, buffer, sizeof(buffer), 1, 1));
	assert(karashi_ym2413_save(&chip, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	memset(&restored, 0xa5, sizeof(restored));
	assert(karashi_state_reader_begin(&reader, buffer, size));
	assert(karashi_ym2413_load(&restored, &reader));
	for (index = 0; index < 5000; index++) assert(karashi_ym2413_sample(&chip) == karashi_ym2413_sample(&restored));
}

int main(void) {
	test_silicon_instruments();
	test_silence_and_pitch();
	test_rhythm();
	test_state();
	puts("YM2413 silicon instruments, silence, pitch, release, rhythm and state: pass");
	return 0;
}
