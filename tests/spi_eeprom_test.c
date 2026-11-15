#include "spi_eeprom.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t data[KARASHI_SPI_EEPROM_SIZE];

static void lines(struct karashi_spi_eeprom *chip, uint8_t value) {
	karashi_spi_eeprom_write(chip, value, data);
}

static void select(struct karashi_spi_eeprom *chip) {
	lines(chip, 0x0c);
	lines(chip, 0x04);
}

static void deselect(struct karashi_spi_eeprom *chip) {
	lines(chip, 0x0c);
}

static void send_byte(struct karashi_spi_eeprom *chip, uint8_t value) {
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		uint8_t input = (value & (0x80 >> bit)) ? 1 : 0;
		lines(chip, (uint8_t) (0x04 | input));
		lines(chip, (uint8_t) (0x06 | input));
	}
}

static uint8_t receive_byte(struct karashi_spi_eeprom *chip) {
	uint8_t value = 0;
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		lines(chip, 0x04);
		lines(chip, 0x06);
		value = (uint8_t) (value << 1 | (karashi_spi_eeprom_read(chip) ? 1 : 0));
	}
	return value;
}

static void command(struct karashi_spi_eeprom *chip, uint8_t opcode) {
	select(chip);
	send_byte(chip, opcode);
	deselect(chip);
}

static uint8_t read_status(struct karashi_spi_eeprom *chip) {
	uint8_t status;

	select(chip);
	send_byte(chip, 0x05);
	status = receive_byte(chip);
	deselect(chip);
	return status;
}

int main(void) {
	struct karashi_spi_eeprom chip;
	struct karashi_spi_eeprom restored;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	uint8_t state[128];
	size_t state_size;

	memset(data, 0xff, sizeof(data));
	karashi_spi_eeprom_reset(&chip);
	select(&chip);
	send_byte(&chip, 0x02);
	send_byte(&chip, 0x00);
	send_byte(&chip, 0x10);
	send_byte(&chip, 0x55);
	deselect(&chip);
	assert(data[0x10] == 0xff);

	command(&chip, 0x06);
	assert((read_status(&chip) & 2) != 0);
	command(&chip, 0x04);
	assert((read_status(&chip) & 2) == 0);
	command(&chip, 0x06);
	select(&chip);
	send_byte(&chip, 0x02);
	send_byte(&chip, 0x00);
	send_byte(&chip, 0x7f);
	send_byte(&chip, 0x12);
	send_byte(&chip, 0x34);
	deselect(&chip);
	assert(data[0x7f] == 0x12 && data[0] == 0x34);
	assert((chip.status & 2) == 0);
	assert((read_status(&chip) & 2) == 0);

	select(&chip);
	send_byte(&chip, 0x03);
	send_byte(&chip, 0x00);
	send_byte(&chip, 0x7f);
	assert(receive_byte(&chip) == 0x12);
	assert(receive_byte(&chip) == 0xff);
	deselect(&chip);

	command(&chip, 0x06);
	select(&chip);
	send_byte(&chip, 0x01);
	send_byte(&chip, 0x0c);
	deselect(&chip);
	command(&chip, 0x06);
	select(&chip);
	send_byte(&chip, 0x02);
	send_byte(&chip, 0x00);
	send_byte(&chip, 0x10);
	send_byte(&chip, 0x56);
	deselect(&chip);
	assert(data[0x10] == 0xff);

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 1, 1));
	assert(karashi_spi_eeprom_save(&chip, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	memset(&restored, 0, sizeof(restored));
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_spi_eeprom_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.address == chip.address && restored.status == chip.status && restored.state == chip.state);
	chip.state = KARASHI_SPI_READ;
	chip.bits = 8;
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 1, 1));
	assert(karashi_spi_eeprom_save(&chip, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(!karashi_spi_eeprom_load(&restored, &reader));
	puts("SPI EEPROM write enable, page wrap, read, protection and state: pass");
	return 0;
}
