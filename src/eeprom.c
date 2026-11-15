#include "eeprom.h"

#include <string.h>

enum {
	EEPROM_IDLE,
	EEPROM_CONTROL,
	EEPROM_ADDRESS,
	EEPROM_WRITE,
	EEPROM_READ,
	EEPROM_MASTER_ACK
};

static void prepare_read(struct karashi_eeprom *eeprom, const uint8_t *data) {
	eeprom->state = EEPROM_READ;
	eeprom->shift = data[eeprom->address];
	eeprom->bits = 0;
	eeprom->drive_low = (eeprom->shift & 0x80) == 0;
}

static void receive_byte(struct karashi_eeprom *eeprom, uint8_t *data) {
	bool accepted = true;
	uint8_t next = EEPROM_IDLE;

	if (eeprom->state == EEPROM_CONTROL && eeprom->mode == KARASHI_EEPROM_AUTO) {
		if ((eeprom->shift & 0xf0) == 0xa0) eeprom->mode = KARASHI_EEPROM_DEVICE_ADDRESS;
		else {
			eeprom->mode = KARASHI_EEPROM_WORD_ADDRESS;
			eeprom->address_mask = 0x7f;
			eeprom->page_mask = 3;
			eeprom->address_bytes = 0;
		}
	}
	if (eeprom->state == EEPROM_CONTROL && eeprom->mode == KARASHI_EEPROM_WORD_ADDRESS) {
		eeprom->address = eeprom->shift >> 1;
		next = eeprom->shift & 1 ? EEPROM_READ : EEPROM_WRITE;
	} else if (eeprom->state == EEPROM_CONTROL) {
		accepted = (eeprom->shift & 0xf0) == 0xa0;
		if (accepted) {
			if (eeprom->address_bytes == 1) {
				eeprom->address = (uint16_t) ((eeprom->address & 0xff)
				                              | (((eeprom->shift >> 1) & 7) << 8));
				eeprom->address &= eeprom->address_mask;
			}
			eeprom->address_received = 0;
			next = eeprom->shift & 1 ? EEPROM_READ : EEPROM_ADDRESS;
		}
	} else if (eeprom->state == EEPROM_ADDRESS) {
		if (eeprom->address_bytes == 2 && eeprom->address_received == 0) {
			eeprom->address = (uint16_t) eeprom->shift << 8;
			eeprom->address_received = 1;
			next = EEPROM_ADDRESS;
		} else {
			eeprom->address = (uint16_t) ((eeprom->address & 0xff00) | eeprom->shift) & eeprom->address_mask;
			next = EEPROM_WRITE;
		}
	} else if (eeprom->state == EEPROM_WRITE && eeprom->mode == KARASHI_EEPROM_WORD_ADDRESS) {
		data[eeprom->address] = eeprom->shift;
		eeprom->address = (uint16_t) ((eeprom->address & ~eeprom->page_mask)
		                                    | ((eeprom->address + 1) & eeprom->page_mask));
		next = EEPROM_WRITE;
	} else if (eeprom->state == EEPROM_WRITE) {
		data[eeprom->address] = eeprom->shift;
		eeprom->address = (uint16_t) ((eeprom->address & ~eeprom->page_mask)
		                                    | ((eeprom->address + 1) & eeprom->page_mask));
		eeprom->address &= eeprom->address_mask;
		next = EEPROM_WRITE;
	} else {
		accepted = false;
	}
	eeprom->next_state = next;
	eeprom->bits = 0;
	eeprom->shift = 0;
	eeprom->ack_low = accepted;
	eeprom->ack_pending = true;
}

void karashi_eeprom_reset(struct karashi_eeprom *eeprom) {
	memset(eeprom, 0, sizeof(*eeprom));
	eeprom->address_mask = 0xff;
	eeprom->page_mask = 3;
	eeprom->address_bytes = 1;
	eeprom->scl = true;
	eeprom->sda = true;
}

void karashi_eeprom_configure(
	struct karashi_eeprom *eeprom, enum karashi_eeprom_mode mode, uint16_t address_mask, uint16_t page_mask,
	uint8_t address_bytes
) {
	eeprom->mode = (uint8_t) mode;
	eeprom->address_mask = address_mask;
	eeprom->page_mask = page_mask;
	eeprom->address_bytes = address_bytes;
}

bool karashi_eeprom_read_sda(const struct karashi_eeprom *eeprom) {
	return eeprom->sda && !eeprom->drive_low;
}

void karashi_eeprom_write_lines(struct karashi_eeprom *eeprom, bool scl, bool sda, uint8_t *data) {
	bool old_scl = eeprom->scl;
	bool old_sda = eeprom->sda;

	eeprom->scl = scl;
	eeprom->sda = sda;
	if (old_sda && !sda && scl) {
		eeprom->state = EEPROM_CONTROL;
		eeprom->bits = 0;
		eeprom->shift = 0;
		eeprom->drive_low = false;
		eeprom->ack_pending = false;
		eeprom->ack_active = false;
		return;
	}
	if (!old_sda && sda && scl) {
		eeprom->state = EEPROM_IDLE;
		eeprom->drive_low = false;
		eeprom->ack_pending = false;
		eeprom->ack_active = false;
		return;
	}
	if (!old_scl && scl) {
		if (eeprom->ack_active) return;
		if (eeprom->state == EEPROM_CONTROL || eeprom->state == EEPROM_ADDRESS
		    || eeprom->state == EEPROM_WRITE) {
			eeprom->shift = (uint8_t) (eeprom->shift << 1 | (sda ? 1 : 0));
			if (++eeprom->bits == 8) receive_byte(eeprom, data);
		} else if (eeprom->state == EEPROM_READ) {
			eeprom->bits++;
		} else if (eeprom->state == EEPROM_MASTER_ACK) {
			eeprom->master_ack = !sda;
		}
		return;
	}
	if (old_scl && !scl) {
		if (eeprom->ack_pending) {
			eeprom->ack_pending = false;
			eeprom->ack_active = true;
			eeprom->drive_low = eeprom->ack_low;
		} else if (eeprom->ack_active) {
			eeprom->ack_active = false;
			eeprom->drive_low = false;
			eeprom->state = eeprom->next_state;
			if (eeprom->state == EEPROM_READ) prepare_read(eeprom, data);
		} else if (eeprom->state == EEPROM_READ) {
			if (eeprom->bits < 8) eeprom->drive_low = (eeprom->shift & (0x80u >> eeprom->bits)) == 0;
			else {
				eeprom->drive_low = false;
				eeprom->state = EEPROM_MASTER_ACK;
			}
		} else if (eeprom->state == EEPROM_MASTER_ACK) {
			if (eeprom->master_ack) {
				eeprom->address = (eeprom->address + 1) & eeprom->address_mask;
				prepare_read(eeprom, data);
			} else {
				eeprom->state = EEPROM_IDLE;
				eeprom->drive_low = false;
			}
		}
	}
}

bool karashi_eeprom_save(const struct karashi_eeprom *eeprom, struct karashi_state_writer *writer) {
	karashi_state_write_u8(writer, eeprom->state);
	karashi_state_write_u8(writer, eeprom->mode);
	karashi_state_write_u8(writer, eeprom->next_state);
	karashi_state_write_u8(writer, eeprom->shift);
	karashi_state_write_u8(writer, eeprom->bits);
	karashi_state_write_u16(writer, eeprom->address);
	karashi_state_write_u16(writer, eeprom->address_mask);
	karashi_state_write_u16(writer, eeprom->page_mask);
	karashi_state_write_u8(writer, eeprom->address_bytes);
	karashi_state_write_u8(writer, eeprom->address_received);
	karashi_state_write_bool(writer, eeprom->scl);
	karashi_state_write_bool(writer, eeprom->sda);
	karashi_state_write_bool(writer, eeprom->drive_low);
	karashi_state_write_bool(writer, eeprom->ack_pending);
	karashi_state_write_bool(writer, eeprom->ack_active);
	karashi_state_write_bool(writer, eeprom->ack_low);
	return karashi_state_write_bool(writer, eeprom->master_ack);
}

bool karashi_eeprom_load(struct karashi_eeprom *eeprom, struct karashi_state_reader *reader) {
	bool loaded = karashi_state_read_u8(reader, &eeprom->state);

	loaded = karashi_state_read_u8(reader, &eeprom->mode) && loaded;
	loaded = karashi_state_read_u8(reader, &eeprom->next_state) && loaded;
	loaded = karashi_state_read_u8(reader, &eeprom->shift) && loaded;
	loaded = karashi_state_read_u8(reader, &eeprom->bits) && loaded;
	loaded = karashi_state_read_u16(reader, &eeprom->address) && loaded;
	loaded = karashi_state_read_u16(reader, &eeprom->address_mask) && loaded;
	loaded = karashi_state_read_u16(reader, &eeprom->page_mask) && loaded;
	loaded = karashi_state_read_u8(reader, &eeprom->address_bytes) && loaded;
	loaded = karashi_state_read_u8(reader, &eeprom->address_received) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->scl) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->sda) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->drive_low) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->ack_pending) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->ack_active) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->ack_low) && loaded;
	loaded = karashi_state_read_bool(reader, &eeprom->master_ack) && loaded;
	return loaded && eeprom->state <= EEPROM_MASTER_ACK && eeprom->next_state <= EEPROM_MASTER_ACK
	       && eeprom->bits <= 8 && eeprom->mode <= KARASHI_EEPROM_DEVICE_ADDRESS && eeprom->address_bytes <= 2
	       && eeprom->address_received <= eeprom->address_bytes && eeprom->address <= eeprom->address_mask
	       && eeprom->page_mask <= eeprom->address_mask;
}
