#include "eeprom93.h"

#include <string.h>

enum {
	STATE_START,
	STATE_COMMAND,
	STATE_READ,
	STATE_WRITE,
	STATE_WRITE_ALL,
	STATE_DONE
};

#define LINE_DATA 0x01
#define LINE_CLOCK 0x02
#define LINE_SELECT 0x04
#define WORDS (KARASHI_EEPROM93_SIZE / 2)

static uint16_t word_at(const uint8_t *data, unsigned address) {
	return (uint16_t) (data[address * 2] | data[address * 2 + 1] << 8);
}

static void store_word(uint8_t *data, unsigned address, uint16_t value) {
	data[address * 2] = (uint8_t) value;
	data[address * 2 + 1] = (uint8_t) (value >> 8);
}

static void command(struct karashi_eeprom93 *chip, uint8_t *data) {
	unsigned opcode = (chip->shift >> 6) & 3;
	unsigned address = chip->shift & 0x3f;
	unsigned index;

	chip->address = (uint8_t) address;
	chip->count = 0;
	chip->shift = 0;
	chip->state = STATE_DONE;
	if (opcode == 2) {
		chip->state = STATE_READ;
		chip->data_out = false;
	} else if (opcode == 1) chip->state = STATE_WRITE;
	else if (opcode == 3) {
		if (chip->write_enable) store_word(data, address, 0xffff);
	} else {
		switch (address >> 4) {
			case 0: chip->write_enable = false; break;
			case 1: chip->state = STATE_WRITE_ALL; break;
			case 2:
				if (chip->write_enable) {
					for (index = 0; index < WORDS; index++) store_word(data, index, 0xffff);
				}
				break;
			default: chip->write_enable = true; break;
		}
	}
}

static void clock_bit(struct karashi_eeprom93 *chip, bool bit, uint8_t *data) {
	unsigned index;

	switch (chip->state) {
		case STATE_START:
			if (bit) {
				chip->state = STATE_COMMAND;
				chip->count = 0;
				chip->shift = 0;
			}
			break;
		case STATE_COMMAND:
			chip->shift = (uint16_t) (chip->shift << 1 | bit);
			if (++chip->count == 8) command(chip, data);
			break;
		case STATE_READ:
			chip->data_out = (word_at(data, chip->address) >> (15 - chip->count)) & 1;
			if (++chip->count == 16) {
				chip->count = 0;
				chip->address = (uint8_t) ((chip->address + 1) % WORDS);
			}
			break;
		case STATE_WRITE:
		case STATE_WRITE_ALL:
			chip->shift = (uint16_t) (chip->shift << 1 | bit);
			if (++chip->count < 16) break;
			if (chip->write_enable && chip->state == STATE_WRITE) store_word(data, chip->address, chip->shift);
			else if (chip->write_enable) {
				for (index = 0; index < WORDS; index++) store_word(data, index, chip->shift);
			}
			chip->state = STATE_DONE;
			break;
		default: break;
	}
}

void karashi_eeprom93_reset(struct karashi_eeprom93 *chip) {
	memset(chip, 0, sizeof(*chip));
	chip->data_out = true;
}

void karashi_eeprom93_write(struct karashi_eeprom93 *chip, uint8_t lines, uint8_t data[KARASHI_EEPROM93_SIZE]) {
	bool select = (lines & LINE_SELECT) != 0;
	bool clock = (lines & LINE_CLOCK) != 0;

	if (chip->cs && !select) {
		chip->state = STATE_START;
		chip->data_out = true;
	} else if (select && clock && !chip->clock) clock_bit(chip, (lines & LINE_DATA) != 0, data);
	chip->cs = select;
	chip->clock = clock;
}

uint8_t karashi_eeprom93_read(const struct karashi_eeprom93 *chip) {
	return (uint8_t) ((chip->data_out ? 1 : 0) | 0x02 | (chip->cs ? 0x04 : 0));
}

bool karashi_eeprom93_save(const struct karashi_eeprom93 *chip, struct karashi_state_writer *writer) {
	karashi_state_write_u8(writer, chip->state);
	karashi_state_write_u8(writer, chip->count);
	karashi_state_write_u8(writer, chip->address);
	karashi_state_write_u16(writer, chip->shift);
	karashi_state_write_bool(writer, chip->cs);
	karashi_state_write_bool(writer, chip->clock);
	karashi_state_write_bool(writer, chip->data_out);
	return karashi_state_write_bool(writer, chip->write_enable);
}

bool karashi_eeprom93_load(struct karashi_eeprom93 *chip, struct karashi_state_reader *reader) {
	bool loaded = karashi_state_read_u8(reader, &chip->state);

	loaded = karashi_state_read_u8(reader, &chip->count) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->address) && loaded;
	loaded = karashi_state_read_u16(reader, &chip->shift) && loaded;
	loaded = karashi_state_read_bool(reader, &chip->cs) && loaded;
	loaded = karashi_state_read_bool(reader, &chip->clock) && loaded;
	loaded = karashi_state_read_bool(reader, &chip->data_out) && loaded;
	loaded = karashi_state_read_bool(reader, &chip->write_enable) && loaded;
	return loaded && chip->state <= STATE_DONE && chip->count < 16 && chip->address < WORDS;
}
