#ifndef KARASHI_YM2413_H
#define KARASHI_YM2413_H

#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KARASHI_YM2413_CLOCK_DIVIDER 72

extern const uint8_t karashi_ym2413_instruments[19][8];

struct karashi_ym2413_slot {
	uint32_t phase;
	int16_t output[2];
	uint8_t level;
	uint8_t state;
	bool on;
};

struct karashi_ym2413 {
	uint8_t registers[0x40];
	uint8_t address;
	struct karashi_ym2413_slot slot[18];
	uint32_t counter;
	uint32_t noise;
};

void karashi_ym2413_reset(struct karashi_ym2413 *chip);
void karashi_ym2413_write(struct karashi_ym2413 *chip, bool data, uint8_t value);
int32_t karashi_ym2413_sample(struct karashi_ym2413 *chip);
bool karashi_ym2413_save(const struct karashi_ym2413 *chip, struct karashi_state_writer *writer);
bool karashi_ym2413_load(struct karashi_ym2413 *chip, struct karashi_state_reader *reader);

#endif
