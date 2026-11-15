#include "spi_eeprom.h"

#include <string.h>

enum {
	LINE_DATA = 0x01,
	LINE_CLOCK = 0x02,
	LINE_HOLD = 0x04,
	LINE_SELECT = 0x08,
	STATUS_WRITE_ENABLE = 0x02,
	STATUS_PROTECT = 0x0c,
	PAGE_MASK = 0x7f
};

static void begin_transaction(struct karashi_spi_eeprom *chip) {
	chip->state = KARASHI_SPI_OPCODE;
	chip->opcode = 0;
	chip->shift = 0;
	chip->bits = 0;
	chip->output = true;
	chip->wrote = false;
}

void karashi_spi_eeprom_reset(struct karashi_spi_eeprom *chip) {
	memset(chip, 0, sizeof(*chip));
	chip->chip_select = true;
	begin_transaction(chip);
}

static bool shift_input(struct karashi_spi_eeprom *chip, bool bit, unsigned width) {
	chip->shift = (uint8_t) (chip->shift << 1 | (bit ? 1 : 0));
	chip->bits++;
	return chip->bits == width;
}

static bool address_protected(const struct karashi_spi_eeprom *chip) {
	switch ((chip->status & STATUS_PROTECT) >> 2) {
		case 1: return chip->address >= 0xc000;
		case 2: return chip->address >= 0x8000;
		case 3: return true;
		default: return false;
	}
}

static void decode_opcode(struct karashi_spi_eeprom *chip) {
	chip->opcode = chip->shift;
	chip->shift = 0;
	chip->bits = 0;
	switch (chip->opcode) {
		case 0x01: chip->state = KARASHI_SPI_WRITE; break;
		case 0x02:
		case 0x03:
			chip->address = 0;
			chip->state = KARASHI_SPI_ADDRESS;
			break;
		case 0x04:
			chip->status &= (uint8_t) ~STATUS_WRITE_ENABLE;
			chip->state = KARASHI_SPI_STANDBY;
			break;
		case 0x05:
			chip->shift = chip->status;
			chip->state = KARASHI_SPI_READ;
			break;
		case 0x06:
			chip->status |= STATUS_WRITE_ENABLE;
			chip->state = KARASHI_SPI_STANDBY;
			break;
		default: chip->state = KARASHI_SPI_STANDBY; break;
	}
}

static void rising_edge(
	struct karashi_spi_eeprom *chip, bool input, uint8_t data[KARASHI_SPI_EEPROM_SIZE]
) {
	if (chip->state == KARASHI_SPI_OPCODE) {
		if (shift_input(chip, input, 8)) decode_opcode(chip);
	} else if (chip->state == KARASHI_SPI_ADDRESS) {
		chip->address = (uint16_t) (chip->address << 1 | (input ? 1 : 0));
		chip->bits++;
		if (chip->bits == 16) {
			chip->bits = 0;
			chip->shift = chip->opcode == 0x03 ? data[chip->address] : 0;
			chip->state = chip->opcode == 0x03 ? KARASHI_SPI_READ : KARASHI_SPI_WRITE;
		}
	} else if (chip->state == KARASHI_SPI_WRITE) {
		if (shift_input(chip, input, 8)) {
			if (chip->opcode == 0x01) {
				if (chip->status & STATUS_WRITE_ENABLE) {
					chip->status = (uint8_t) ((chip->status & STATUS_WRITE_ENABLE) | (chip->shift & STATUS_PROTECT));
					chip->wrote = true;
				}
				chip->state = KARASHI_SPI_STANDBY;
			} else {
				if ((chip->status & STATUS_WRITE_ENABLE) && !address_protected(chip)) {
					data[chip->address] = chip->shift;
					chip->wrote = true;
				}
				chip->address = (uint16_t) ((chip->address & ~PAGE_MASK) | ((chip->address + 1) & PAGE_MASK));
				chip->shift = 0;
				chip->bits = 0;
			}
		}
	} else if (chip->state == KARASHI_SPI_READ) {
		chip->output = ((chip->shift >> (7 - chip->bits)) & 1) != 0;
		chip->bits++;
		if (chip->bits == 8) {
			chip->bits = 0;
			if (chip->opcode == 0x03) {
				chip->address++;
				chip->shift = data[chip->address];
			}
		}
	}
}

void karashi_spi_eeprom_write(
	struct karashi_spi_eeprom *chip, uint8_t lines, uint8_t data[KARASHI_SPI_EEPROM_SIZE]
) {
	bool select = (lines & LINE_SELECT) != 0;
	bool clock = (lines & LINE_CLOCK) != 0;

	if (select) {
		if (!chip->chip_select) {
			if (chip->wrote) chip->status &= (uint8_t) ~STATUS_WRITE_ENABLE;
			begin_transaction(chip);
		}
	} else if ((lines & LINE_HOLD) && clock && !chip->clock) {
		rising_edge(chip, (lines & LINE_DATA) != 0, data);
	}
	chip->chip_select = select;
	chip->clock = clock;
}

bool karashi_spi_eeprom_read(const struct karashi_spi_eeprom *chip) {
	return chip->output;
}

bool karashi_spi_eeprom_save(const struct karashi_spi_eeprom *chip, struct karashi_state_writer *writer) {
	return karashi_state_write_u16(writer, chip->address) && karashi_state_write_u8(writer, chip->state)
	       && karashi_state_write_u8(writer, chip->opcode) && karashi_state_write_u8(writer, chip->shift)
	       && karashi_state_write_u8(writer, chip->bits) && karashi_state_write_u8(writer, chip->status)
	       && karashi_state_write_bool(writer, chip->chip_select) && karashi_state_write_bool(writer, chip->clock)
	       && karashi_state_write_bool(writer, chip->output) && karashi_state_write_bool(writer, chip->wrote);
}

bool karashi_spi_eeprom_load(struct karashi_spi_eeprom *chip, struct karashi_state_reader *reader) {
	bool loaded = karashi_state_read_u16(reader, &chip->address) && karashi_state_read_u8(reader, &chip->state)
	              && karashi_state_read_u8(reader, &chip->opcode) && karashi_state_read_u8(reader, &chip->shift)
	              && karashi_state_read_u8(reader, &chip->bits) && karashi_state_read_u8(reader, &chip->status)
	              && karashi_state_read_bool(reader, &chip->chip_select) && karashi_state_read_bool(reader, &chip->clock)
	              && karashi_state_read_bool(reader, &chip->output) && karashi_state_read_bool(reader, &chip->wrote);

	if (!loaded || chip->state > KARASHI_SPI_STANDBY) return false;
	if (chip->state == KARASHI_SPI_ADDRESS) return chip->bits < 16;
	if (chip->state == KARASHI_SPI_STANDBY) return chip->bits <= 8;
	return chip->bits < 8;
}
