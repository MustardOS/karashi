#ifndef KARASHI_EEPROM_H
#define KARASHI_EEPROM_H

#include "state.h"

#include <stdbool.h>
#include <stdint.h>

enum karashi_eeprom_mode {
	KARASHI_EEPROM_AUTO = 0,
	KARASHI_EEPROM_WORD_ADDRESS,
	KARASHI_EEPROM_DEVICE_ADDRESS
};

struct karashi_eeprom {
	uint8_t state;
	uint8_t mode;
	uint8_t next_state;
	uint8_t shift;
	uint8_t bits;
	uint16_t address;
	uint16_t address_mask;
	uint16_t page_mask;
	uint8_t address_bytes;
	uint8_t address_received;
	bool scl;
	bool sda;
	bool drive_low;
	bool ack_pending;
	bool ack_active;
	bool ack_low;
	bool master_ack;
};

void karashi_eeprom_reset(struct karashi_eeprom *eeprom);
void karashi_eeprom_configure(
	struct karashi_eeprom *eeprom, enum karashi_eeprom_mode mode, uint16_t address_mask, uint16_t page_mask,
	uint8_t address_bytes
);
void karashi_eeprom_write_lines(struct karashi_eeprom *eeprom, bool scl, bool sda, uint8_t *data);
bool karashi_eeprom_read_sda(const struct karashi_eeprom *eeprom);
bool karashi_eeprom_save(const struct karashi_eeprom *eeprom, struct karashi_state_writer *writer);
bool karashi_eeprom_load(struct karashi_eeprom *eeprom, struct karashi_state_reader *reader);

#endif
