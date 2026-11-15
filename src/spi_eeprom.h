#ifndef KARASHI_SPI_EEPROM_H
#define KARASHI_SPI_EEPROM_H

#include "state.h"

#include <stdbool.h>
#include <stdint.h>

#define KARASHI_SPI_EEPROM_SIZE 65536

enum karashi_spi_eeprom_state {
	KARASHI_SPI_OPCODE = 0,
	KARASHI_SPI_ADDRESS,
	KARASHI_SPI_WRITE,
	KARASHI_SPI_READ,
	KARASHI_SPI_STANDBY
};

struct karashi_spi_eeprom {
	uint16_t address;
	uint8_t state;
	uint8_t opcode;
	uint8_t shift;
	uint8_t bits;
	uint8_t status;
	bool chip_select;
	bool clock;
	bool output;
	bool wrote;
};

void karashi_spi_eeprom_reset(struct karashi_spi_eeprom *chip);
void karashi_spi_eeprom_write(
	struct karashi_spi_eeprom *chip, uint8_t lines, uint8_t data[KARASHI_SPI_EEPROM_SIZE]
);
bool karashi_spi_eeprom_read(const struct karashi_spi_eeprom *chip);
bool karashi_spi_eeprom_save(const struct karashi_spi_eeprom *chip, struct karashi_state_writer *writer);
bool karashi_spi_eeprom_load(struct karashi_spi_eeprom *chip, struct karashi_state_reader *reader);

#endif
