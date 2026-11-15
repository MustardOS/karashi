#ifndef KARASHI_YM2612_H
#define KARASHI_YM2612_H

#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct karashi_ym2612_operator {
	uint32_t phase;
	uint32_t increment;
	int32_t output;
	int32_t previous;
	uint16_t level;
	uint16_t total_level;
	uint16_t sustain_level;
	uint8_t state;
	uint8_t attack;
	uint8_t decay;
	uint8_t sustain;
	uint8_t release;
	uint8_t key_scale;
	uint8_t multiple;
	uint8_t detune;
	uint8_t ssg;
	uint8_t key_code;
	bool tremolo;
	bool key;
	bool ssg_inverted;
	bool ssg_held;
};

struct karashi_ym2612_channel {
	struct karashi_ym2612_operator op[4];
	uint16_t frequency;
	uint8_t block;
	uint8_t frequency_latch;
	uint8_t feedback;
	uint8_t algorithm;
	uint8_t tremolo_depth;
	uint8_t vibrato_depth;
	bool left;
	bool right;
};

struct karashi_ym2612 {
	struct karashi_ym2612_channel channel[6];
	uint16_t special_frequency[3];
	uint8_t special_block[3];
	uint8_t special_latch[3];
	uint8_t registers[2][256];
	uint16_t timer_a;
	uint16_t timer_a_counter;
	uint16_t envelope_counter;
	uint64_t busy_until;
	uint8_t timer_b;
	uint8_t timer_b_counter;
	uint8_t timer_b_divider;
	uint8_t timer_control;
	uint8_t status;
	uint8_t address;
	uint8_t part;
	uint8_t dac;
	uint8_t lfo_rate;
	uint8_t lfo_counter;
	uint8_t lfo_divider;
	uint8_t envelope_divider;
	uint8_t special_mode;
	bool dac_enabled;
	bool lfo_enabled;
};

void karashi_ym2612_reset(struct karashi_ym2612 *chip);
void karashi_ym2612_write(struct karashi_ym2612 *chip, unsigned port, uint8_t value);
void karashi_ym2612_write_at(
	struct karashi_ym2612 *chip, unsigned port, uint8_t value, uint64_t master_clock
);
uint8_t karashi_ym2612_read(const struct karashi_ym2612 *chip);
uint8_t karashi_ym2612_read_at(const struct karashi_ym2612 *chip, uint64_t master_clock);
void karashi_ym2612_generate(struct karashi_ym2612 *chip, int16_t *stereo, size_t frames);
bool karashi_ym2612_save(const struct karashi_ym2612 *chip, struct karashi_state_writer *writer);
bool karashi_ym2612_load(struct karashi_ym2612 *chip, struct karashi_state_reader *reader);

#endif
