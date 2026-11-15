#include "eeprom93.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct karashi_eeprom93 chip;
static uint8_t data[KARASHI_EEPROM93_SIZE];

static void clock_in(unsigned bit) {
	karashi_eeprom93_write(&chip, (uint8_t) (0x04 | bit), data);
	karashi_eeprom93_write(&chip, (uint8_t) (0x06 | bit), data);
}

static void send(unsigned value, unsigned bits) {
	while (bits--) clock_in((value >> bits) & 1);
}

static void command(unsigned opcode, unsigned address) {
	karashi_eeprom93_write(&chip, 0x00, data);
	send(1, 1);
	send(opcode, 2);
	send(address, 6);
}

static void deselect(void) {
	karashi_eeprom93_write(&chip, 0x00, data);
}

static unsigned read_word(void) {
	unsigned value = 0;
	unsigned bit;

	for (bit = 0; bit < 16; bit++) {
		clock_in(0);
		value = value << 1 | (karashi_eeprom93_read(&chip) & 1);
	}
	return value;
}

int main(void) {
	memset(data, 0xff, sizeof(data));
	karashi_eeprom93_reset(&chip);
	assert(karashi_eeprom93_read(&chip) == 0x03);

	command(1, 5);
	send(0x1234, 16);
	deselect();
	assert(data[10] == 0xff && data[11] == 0xff);

	command(0, 0x30);
	deselect();
	command(1, 5);
	send(0x1234, 16);
	deselect();
	command(1, 6);
	send(0xabcd, 16);
	deselect();
	assert(data[10] == 0x34 && data[11] == 0x12);

	command(2, 5);
	assert((karashi_eeprom93_read(&chip) & 0x05) == 0x04);
	assert(read_word() == 0x1234);
	assert(read_word() == 0xabcd);
	deselect();
	assert(karashi_eeprom93_read(&chip) == 0x03);

	command(3, 5);
	deselect();
	assert(data[10] == 0xff && data[11] == 0xff);

	command(0, 0x10);
	send(0x5a5a, 16);
	deselect();
	assert(data[0] == 0x5a && data[127] == 0x5a);
	command(0, 0x20);
	deselect();
	assert(data[0] == 0xff && data[127] == 0xff);

	command(0, 0x00);
	deselect();
	command(1, 0);
	send(0x0000, 16);
	deselect();
	assert(data[0] == 0xff);

	puts("93C46 EEPROM write protect, read, write, erase, ERAL and WRAL: pass");
	return 0;
}
