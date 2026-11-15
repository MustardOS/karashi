#ifndef KARASHI_EEPROM93_H
#define KARASHI_EEPROM93_H

#include "state.h"

#include <stdbool.h>
#include <stdint.h>

#define KARASHI_EEPROM93_SIZE 128

struct karashi_eeprom93 {
	uint8_t state;
	uint8_t count;
	uint8_t address;
	uint16_t shift;
	bool cs;
	bool clock;
	bool data_out;
	bool write_enable;
};

void karashi_eeprom93_reset(struct karashi_eeprom93 *chip);
void karashi_eeprom93_write(struct karashi_eeprom93 *chip, uint8_t lines, uint8_t data[KARASHI_EEPROM93_SIZE]);
uint8_t karashi_eeprom93_read(const struct karashi_eeprom93 *chip);
bool karashi_eeprom93_save(const struct karashi_eeprom93 *chip, struct karashi_state_writer *writer);
bool karashi_eeprom93_load(struct karashi_eeprom93 *chip, struct karashi_state_reader *reader);

#endif
