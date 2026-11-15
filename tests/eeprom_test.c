#include "eeprom.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void lines(struct karashi_eeprom *eeprom, uint8_t *data, bool scl, bool sda) {
	karashi_eeprom_write_lines(eeprom, scl, sda, data);
}

static void start(struct karashi_eeprom *eeprom, uint8_t *data) {
	lines(eeprom, data, false, true);
	lines(eeprom, data, true, true);
	lines(eeprom, data, true, false);
	lines(eeprom, data, false, false);
}

static void stop(struct karashi_eeprom *eeprom, uint8_t *data) {
	lines(eeprom, data, false, false);
	lines(eeprom, data, true, false);
	lines(eeprom, data, true, true);
}

static void write_byte(struct karashi_eeprom *eeprom, uint8_t *data, uint8_t value) {
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		bool level = (value & (0x80u >> bit)) != 0;
		lines(eeprom, data, false, level);
		lines(eeprom, data, true, level);
		lines(eeprom, data, false, level);
	}
	lines(eeprom, data, false, true);
	lines(eeprom, data, true, true);
	assert(!karashi_eeprom_read_sda(eeprom));
	lines(eeprom, data, false, true);
}

static uint8_t read_byte(struct karashi_eeprom *eeprom, uint8_t *data) {
	uint8_t value = 0;
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		lines(eeprom, data, true, true);
		value = (uint8_t) (value << 1 | (karashi_eeprom_read_sda(eeprom) ? 1 : 0));
		lines(eeprom, data, false, true);
	}
	lines(eeprom, data, false, true);
	lines(eeprom, data, true, true);
	lines(eeprom, data, false, true);
	return value;
}

static void test_word_address(void) {
	struct karashi_eeprom eeprom;
	uint8_t data[256];

	memset(data, 0xff, sizeof(data));
	karashi_eeprom_reset(&eeprom);
	start(&eeprom, data);
	write_byte(&eeprom, data, 10 << 1);
	assert(eeprom.mode == KARASHI_EEPROM_WORD_ADDRESS);
	write_byte(&eeprom, data, 0x11);
	write_byte(&eeprom, data, 0x22);
	write_byte(&eeprom, data, 0x33);
	write_byte(&eeprom, data, 0x44);
	write_byte(&eeprom, data, 0x55);
	stop(&eeprom, data);
	assert(data[8] == 0x33 && data[9] == 0x44 && data[10] == 0x55 && data[11] == 0x22 && data[12] == 0xff);
	start(&eeprom, data);
	write_byte(&eeprom, data, 8 << 1 | 1);
	assert(read_byte(&eeprom, data) == 0x33);
	stop(&eeprom, data);
}

static void test_larger_devices(void) {
	struct karashi_eeprom eeprom;
	static uint8_t data[8192];

	memset(data, 0xff, sizeof(data));
	karashi_eeprom_reset(&eeprom);
	karashi_eeprom_configure(&eeprom, KARASHI_EEPROM_DEVICE_ADDRESS, 0x3ff, 0x0f, 1);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa4);
	write_byte(&eeprom, data, 0x34);
	write_byte(&eeprom, data, 0x6d);
	stop(&eeprom, data);
	assert(data[0x234] == 0x6d);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa4);
	write_byte(&eeprom, data, 0x34);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa5);
	assert(read_byte(&eeprom, data) == 0x6d);
	stop(&eeprom, data);

	karashi_eeprom_reset(&eeprom);
	karashi_eeprom_configure(&eeprom, KARASHI_EEPROM_DEVICE_ADDRESS, 0x1fff, 0x3f, 2);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa0);
	write_byte(&eeprom, data, 0x12);
	write_byte(&eeprom, data, 0x34);
	write_byte(&eeprom, data, 0xa5);
	stop(&eeprom, data);
	assert(data[0x1234] == 0xa5);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa0);
	write_byte(&eeprom, data, 0x12);
	write_byte(&eeprom, data, 0x34);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa1);
	assert(read_byte(&eeprom, data) == 0xa5);
	stop(&eeprom, data);
}

int main(void) {
	struct karashi_eeprom eeprom;
	struct karashi_eeprom restored;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	uint8_t data[256];
	uint8_t state[128];
	size_t state_size;

	test_word_address();
	test_larger_devices();
	memset(data, 0xff, sizeof(data));
	karashi_eeprom_reset(&eeprom);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa0);
	assert(eeprom.mode == KARASHI_EEPROM_DEVICE_ADDRESS);
	write_byte(&eeprom, data, 0x12);
	write_byte(&eeprom, data, 0x5a);
	stop(&eeprom, data);
	assert(data[0x12] == 0x5a);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa0);
	write_byte(&eeprom, data, 0x12);
	start(&eeprom, data);
	write_byte(&eeprom, data, 0xa1);
	assert(read_byte(&eeprom, data) == 0x5a);
	stop(&eeprom, data);
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 1, 1));
	assert(karashi_eeprom_save(&eeprom, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	memset(&restored, 0, sizeof(restored));
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_eeprom_load(&restored, &reader));
	assert(restored.scl == eeprom.scl && restored.sda == eeprom.sda);
	puts("I2C EEPROM lines, word and device addressing, page write, random read and state: pass");
	return 0;
}
