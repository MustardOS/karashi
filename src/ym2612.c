#include "ym2612.h"

#include <math.h>
#include <string.h>

/*
 * Written from the Yamaha YM2608 (OPNA) application manual, whose FM section the YM2612 shares:
 * register map, four-operator algorithms, detune steps, envelope rate scaling, LFO rates and depths.
 */

enum {
	STATE_ATTACK,
	STATE_DECAY,
	STATE_SUSTAIN,
	STATE_RELEASE
};

enum {
	LEVEL_MAX = 0x3ff,
	SINE_QUARTER = 256,
	POWER_STEPS = 13 * 256
};

static uint16_t log_sine[SINE_QUARTER];
static uint16_t power[POWER_STEPS];
static bool tables_ready;

/* Detune steps for key codes 0 to 31; one step is the manual's 0.053 Hz unit. */
static const uint8_t detune_steps[3][32] = {
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
	{1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
	{2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22}
};

/* Samples per LFO step for the eight LFO rates (3.98 Hz to 72.2 Hz at an 8 MHz clock, 128 steps). */
static const uint8_t lfo_period[8] = {108, 77, 71, 67, 62, 44, 8, 5};

/* Vibrato depth as a 16.16 frequency ratio for 3.4, 6.7, 10, 14, 20, 40 and 80 cents. */
static const uint16_t vibrato_ratio[8] = {0, 129, 254, 380, 532, 761, 1532, 3099};

/* Envelope steps spread over eight updates for the four fractional rates. */
static const uint8_t envelope_pattern[4] = {0xaa, 0xba, 0xee, 0xfe};
static const uint8_t fast_pattern[4] = {0x00, 0x88, 0xaa, 0xee};

static void build_tables(void) {
	unsigned index;

	if (tables_ready) return;
	for (index = 0; index < SINE_QUARTER; index++) {
		double value = sin((2.0 * index + 1.0) * 3.14159265358979323846 / 1024.0);
		log_sine[index] = (uint16_t) (-log2(value) * 256.0 + 0.5);
	}
	for (index = 0; index < POWER_STEPS; index++) {
		power[index] = (uint16_t) (pow(2.0, 13.0 - (double) index / 256.0) - 0.5);
	}
	tables_ready = true;
}

static unsigned key_code(unsigned frequency, unsigned block) {
	unsigned f11 = frequency >> 10 & 1;
	unsigned f10 = frequency >> 9 & 1;
	unsigned f9 = frequency >> 8 & 1;
	unsigned f8 = frequency >> 7 & 1;
	unsigned n3 = (f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8);
	return block << 2 | f11 << 1 | n3;
}

static void operator_frequency(
	const struct karashi_ym2612 *chip, const struct karashi_ym2612_channel *channel, unsigned index,
	unsigned *frequency, unsigned *block
) {
	unsigned number = (unsigned) (channel - chip->channel);

	*frequency = channel->frequency;
	*block = channel->block;
	if (number == 2 && chip->special_mode && index < 3) {
		*frequency = chip->special_frequency[index];
		*block = chip->special_block[index];
	}
}

static uint32_t phase_increment(const struct karashi_ym2612_operator *op, unsigned doubled, unsigned block) {
	uint32_t increment = (doubled << block) >> 2;
	unsigned steps = op->detune & 3 ? detune_steps[(op->detune & 3) - 1][op->key_code] : 0;

	if (op->detune & 4) increment -= steps;
	else increment += steps;
	increment &= 0x1ffff;
	increment = op->multiple ? increment * op->multiple : increment >> 1;
	return increment & 0xfffff;
}

static void update_channel(struct karashi_ym2612 *chip, struct karashi_ym2612_channel *channel) {
	unsigned index;

	for (index = 0; index < 4; index++) {
		struct karashi_ym2612_operator *op = &channel->op[index];
		unsigned frequency;
		unsigned block;
		operator_frequency(chip, channel, index, &frequency, &block);
		op->key_code = (uint8_t) key_code(frequency, block);
		op->increment = phase_increment(op, frequency * 2, block);
	}
}

static unsigned envelope_rate(const struct karashi_ym2612_operator *op, unsigned rate) {
	unsigned scaled;

	if (!rate) return 0;
	scaled = rate * 2 + (op->key_code >> (3 - op->key_scale));
	return scaled > 63 ? 63 : scaled;
}

static void key_on(struct karashi_ym2612_operator *op) {
	if (op->key) return;
	op->key = true;
	op->phase = 0;
	op->ssg_inverted = false;
	op->ssg_held = false;
	op->state = STATE_ATTACK;
	if (envelope_rate(op, op->attack) >= 62) {
		op->level = 0;
		op->state = STATE_DECAY;
	}
}

static void key_off(struct karashi_ym2612_operator *op) {
	if (!op->key) return;
	op->key = false;
	if ((op->ssg & 8) && op->ssg_inverted != ((op->ssg & 4) != 0)) {
		op->level = (uint16_t) ((0x200 - op->level) & LEVEL_MAX);
	}
	op->state = STATE_RELEASE;
}

static unsigned slot_operator(unsigned slot) {
	static const uint8_t order[4] = {0, 2, 1, 3};
	return order[slot & 3];
}

static void write_register(struct karashi_ym2612 *chip, unsigned part, unsigned address, uint8_t value) {
	struct karashi_ym2612_channel *channel;
	struct karashi_ym2612_operator *op;
	unsigned number;

	chip->registers[part][address] = value;
	if (address < 0x30) {
		if (part) return;
		switch (address) {
			case 0x22:
				chip->lfo_enabled = (value & 8) != 0;
				chip->lfo_rate = value & 7;
				if (!chip->lfo_enabled) chip->lfo_counter = chip->lfo_divider = 0;
				break;
			case 0x24: chip->timer_a = (uint16_t) ((chip->timer_a & 3) | (unsigned) value << 2); break;
			case 0x25: chip->timer_a = (uint16_t) ((chip->timer_a & 0x3fc) | (value & 3)); break;
			case 0x26: chip->timer_b = value; break;
			case 0x27:
				if ((value & 1) && !(chip->timer_control & 1)) chip->timer_a_counter = chip->timer_a;
				if ((value & 2) && !(chip->timer_control & 2)) {
					chip->timer_b_counter = chip->timer_b;
					chip->timer_b_divider = 0;
				}
				if (value & 0x10) chip->status &= (uint8_t) ~1;
				if (value & 0x20) chip->status &= (uint8_t) ~2;
				chip->timer_control = value;
				if (chip->special_mode != (value >> 6)) {
					chip->special_mode = value >> 6;
					update_channel(chip, &chip->channel[2]);
				}
				break;
			case 0x28:
				if ((value & 3) == 3) break;
				channel = &chip->channel[(value & 3) + (value & 4 ? 3 : 0)];
				for (number = 0; number < 4; number++) {
					if (value & (0x10 << number)) key_on(&channel->op[slot_operator(number)]);
					else key_off(&channel->op[slot_operator(number)]);
				}
				break;
			case 0x2a: chip->dac = value; break;
			case 0x2b: chip->dac_enabled = (value & 0x80) != 0; break;
			default: break;
		}
		return;
	}
	if ((address & 3) == 3) return;
	number = (address & 3) + part * 3;
	channel = &chip->channel[number];
	if (address < 0xa0) {
		op = &channel->op[slot_operator(address >> 2)];
		switch (address & 0xf0) {
			case 0x30:
				op->detune = (value >> 4) & 7;
				op->multiple = value & 15;
				update_channel(chip, channel);
				break;
			case 0x40: op->total_level = (uint16_t) ((value & 0x7f) << 3); break;
			case 0x50:
				op->key_scale = value >> 6;
				op->attack = value & 0x1f;
				break;
			case 0x60:
				op->tremolo = (value & 0x80) != 0;
				op->decay = value & 0x1f;
				break;
			case 0x70: op->sustain = value & 0x1f; break;
			case 0x80:
				op->sustain_level = (uint16_t) ((value >> 4) == 15 ? 0x3e0 : (value >> 4) << 5);
				op->release = value & 15;
				break;
			default: op->ssg = value & 15; break;
		}
		return;
	}
	switch (address & 0xfc) {
		case 0xa0:
			channel->frequency = (uint16_t) ((unsigned) (channel->frequency_latch & 7) << 8 | value);
			channel->block = (channel->frequency_latch >> 3) & 7;
			update_channel(chip, channel);
			break;
		case 0xa4: channel->frequency_latch = value; break;
		case 0xa8:
			if (part) break;
			number = (address & 3) == 0 ? 2 : (address & 3) == 1 ? 0 : 1;
			chip->special_frequency[number] = (uint16_t) ((unsigned) (chip->special_latch[number] & 7) << 8 | value);
			chip->special_block[number] = (chip->special_latch[number] >> 3) & 7;
			update_channel(chip, &chip->channel[2]);
			break;
		case 0xac:
			if (part) break;
			number = (address & 3) == 0 ? 2 : (address & 3) == 1 ? 0 : 1;
			chip->special_latch[number] = value;
			break;
		case 0xb0:
			channel->feedback = (value >> 3) & 7;
			channel->algorithm = value & 7;
			break;
		case 0xb4:
			channel->left = (value & 0x80) != 0;
			channel->right = (value & 0x40) != 0;
			channel->tremolo_depth = (value >> 4) & 3;
			channel->vibrato_depth = value & 7;
			break;
		default: break;
	}
}

void karashi_ym2612_reset(struct karashi_ym2612 *chip) {
	unsigned channel;
	unsigned index;

	build_tables();
	memset(chip, 0, sizeof(*chip));
	for (channel = 0; channel < 6; channel++) {
		chip->channel[channel].left = chip->channel[channel].right = true;
		for (index = 0; index < 4; index++) {
			chip->channel[channel].op[index].level = LEVEL_MAX;
			chip->channel[channel].op[index].state = STATE_RELEASE;
		}
		chip->registers[channel / 3][0xb4 + channel % 3] = 0xc0;
	}
}

void karashi_ym2612_write(struct karashi_ym2612 *chip, unsigned port, uint8_t value) {
	if (!(port & 1)) {
		chip->address = value;
		chip->part = (uint8_t) ((port >> 1) & 1);
		return;
	}
	write_register(chip, chip->part, chip->address, value);
}

void karashi_ym2612_write_at(
	struct karashi_ym2612 *chip, unsigned port, uint8_t value, uint64_t master_clock
) {
	karashi_ym2612_write(chip, port, value);
	if (port & 1) chip->busy_until = ((master_clock + 41) / 42 + 32) * 42;
}

uint8_t karashi_ym2612_read(const struct karashi_ym2612 *chip) {
	return chip->status;
}

uint8_t karashi_ym2612_read_at(const struct karashi_ym2612 *chip, uint64_t master_clock) {
	return (uint8_t) (chip->status | (master_clock < chip->busy_until ? 0x80 : 0));
}

static void step_timers(struct karashi_ym2612 *chip) {
	if (chip->timer_control & 1) {
		if (++chip->timer_a_counter >= 1024) {
			chip->timer_a_counter = chip->timer_a;
			if (chip->timer_control & 4) chip->status |= 1;
			if (chip->special_mode == 2) {
				unsigned index;
				for (index = 0; index < 4; index++) {
					struct karashi_ym2612_operator *op = &chip->channel[2].op[index];
					if (!op->key) {
						key_on(op);
						op->key = false;
					}
				}
			}
		}
	}
	if (chip->timer_control & 2) {
		if (++chip->timer_b_divider >= 16) {
			chip->timer_b_divider = 0;
			if (++chip->timer_b_counter == 0) {
				chip->timer_b_counter = chip->timer_b;
				if (chip->timer_control & 8) chip->status |= 2;
			}
		}
	}
}

static void step_envelope(struct karashi_ym2612 *chip, struct karashi_ym2612_operator *op) {
	unsigned counter = chip->envelope_counter;
	unsigned rate;
	unsigned shift;
	unsigned step;
	unsigned envelope_step;

	switch (op->state) {
		case STATE_ATTACK: rate = envelope_rate(op, op->attack); break;
		case STATE_DECAY: rate = envelope_rate(op, op->decay); break;
		case STATE_SUSTAIN: rate = envelope_rate(op, op->sustain); break;
		default: rate = envelope_rate(op, op->release * 2u + 1); break;
	}
	if ((op->ssg & 8) && op->level >= 0x200 && op->state != STATE_ATTACK) {
		if (!op->key) op->level = LEVEL_MAX;
		else if (!op->ssg_held) {
			if (op->ssg & 2) op->ssg_inverted = !op->ssg_inverted;
			if (op->ssg & 1) {
				op->ssg_held = true;
				op->level = 0x200;
			} else {
				if (!(op->ssg & 2)) op->phase = 0;
				op->state = STATE_ATTACK;
				if (envelope_rate(op, op->attack) >= 62) {
					op->level = 0;
					op->state = STATE_DECAY;
				}
			}
		}
	}
	shift = rate < 48 ? 11 - (rate >> 2) : 0;
	if (counter & ((1u << shift) - 1)) return;
	if (rate < 2) return;
	if (rate < 48) step = envelope_pattern[rate & 3] >> ((counter >> shift) & 7) & 1;
	else if (rate < 60) step = (1u + (fast_pattern[rate & 3] >> (counter & 7) & 1)) << ((rate >> 2) - 12);
	else step = 8;
	envelope_step = step;
	if (op->ssg & 8) step = op->level < 0x200 ? step * 4 : 0;
	switch (op->state) {
		case STATE_ATTACK:
			if (op->ssg & 8) step = envelope_step;
			if (rate < 62) op->level = (uint16_t) (op->level + ((int) (~op->level * step) >> 4));
			if (op->level == 0 || op->level > LEVEL_MAX || rate >= 62) {
				op->level = 0;
				op->state = STATE_DECAY;
			}
			break;
		case STATE_DECAY:
			op->level = (uint16_t) (op->level + step);
			if (op->level >= op->sustain_level) op->state = STATE_SUSTAIN;
			break;
		default:
			op->level = (uint16_t) (op->level + step);
			break;
	}
	if (op->level > LEVEL_MAX) op->level = LEVEL_MAX;
}

static int32_t operator_output(
	const struct karashi_ym2612_operator *op, uint32_t phase, int32_t modulation, unsigned tremolo
) {
	unsigned position = ((phase >> 10) + (uint32_t) modulation) & 0x3ff;
	unsigned level = op->level;
	unsigned attenuation;
	unsigned index;

	if ((op->ssg & 8) && op->state != STATE_RELEASE && op->ssg_inverted != ((op->ssg & 4) != 0)) {
		level = (0x200 - level) & LEVEL_MAX;
	}
	level += op->total_level;
	if (op->tremolo) level += tremolo;
	if (level >= LEVEL_MAX) return 0;
	index = position & 0x100 ? ~position & 0xff : position & 0xff;
	attenuation = log_sine[index] + (level << 2);
	if (attenuation >= POWER_STEPS) return 0;
	return position & 0x200 ? -(int32_t) power[attenuation] : (int32_t) power[attenuation];
}

static bool silent(const struct karashi_ym2612_channel *channel) {
	unsigned index;

	for (index = 0; index < 4; index++) {
		const struct karashi_ym2612_operator *op = &channel->op[index];
		if ((op->ssg & 8) || op->level + op->total_level < LEVEL_MAX) return false;
	}
	return true;
}

static int32_t channel_output(struct karashi_ym2612 *chip, struct karashi_ym2612_channel *channel) {
	struct karashi_ym2612_operator *op = channel->op;
	unsigned step = chip->lfo_counter;
	unsigned tremolo = 0;
	int32_t feedback = 0;
	int32_t s1;
	int32_t s2;
	int32_t s3;
	int32_t s4;
	int32_t result;
	unsigned index;

	if (chip->lfo_enabled && channel->tremolo_depth) {
		unsigned wave = step & 64 ? step & 63 : 63 - (step & 63);
		static const uint8_t shift[4] = {8, 3, 1, 0};
		tremolo = (wave << 1) >> shift[channel->tremolo_depth];
	}
	if (chip->lfo_enabled && channel->vibrato_depth) {
		unsigned position = (step >> 2) & 7;
		unsigned magnitude = step & 32 ? 7 - position : position;
		for (index = 0; index < 4; index++) {
			unsigned frequency;
			unsigned block;
			int32_t delta;
			operator_frequency(chip, channel, index, &frequency, &block);
			delta = (int32_t) (frequency * 2 * vibrato_ratio[channel->vibrato_depth] * magnitude / 7 >> 16);
			if (step & 64) delta = -delta;
			op[index].phase += phase_increment(&op[index], (unsigned) ((int32_t) (frequency * 2) + delta), block);
		}
	} else {
		for (index = 0; index < 4; index++) op[index].phase += op[index].increment;
	}
	for (index = 0; index < 4; index++) op[index].phase &= 0xfffff;
	if (silent(channel)) {
		op[0].previous = op[0].output;
		op[0].output = 0;
		return 0;
	}
	if (channel->feedback) feedback = (op[0].output + op[0].previous) >> (10 - channel->feedback);
	op[0].previous = op[0].output;
	s1 = op[0].output = operator_output(&op[0], op[0].phase, feedback, tremolo);
	switch (channel->algorithm) {
		case 0:
			s2 = operator_output(&op[1], op[1].phase, s1 >> 1, tremolo);
			s3 = operator_output(&op[2], op[2].phase, s2 >> 1, tremolo);
			result = operator_output(&op[3], op[3].phase, s3 >> 1, tremolo);
			break;
		case 1:
			s2 = operator_output(&op[1], op[1].phase, 0, tremolo);
			s3 = operator_output(&op[2], op[2].phase, (s1 + s2) >> 1, tremolo);
			result = operator_output(&op[3], op[3].phase, s3 >> 1, tremolo);
			break;
		case 2:
			s2 = operator_output(&op[1], op[1].phase, 0, tremolo);
			s3 = operator_output(&op[2], op[2].phase, s2 >> 1, tremolo);
			result = operator_output(&op[3], op[3].phase, (s1 + s3) >> 1, tremolo);
			break;
		case 3:
			s2 = operator_output(&op[1], op[1].phase, s1 >> 1, tremolo);
			s3 = operator_output(&op[2], op[2].phase, 0, tremolo);
			result = operator_output(&op[3], op[3].phase, (s2 + s3) >> 1, tremolo);
			break;
		case 4:
			s2 = operator_output(&op[1], op[1].phase, s1 >> 1, tremolo);
			s3 = operator_output(&op[2], op[2].phase, 0, tremolo);
			s4 = operator_output(&op[3], op[3].phase, s3 >> 1, tremolo);
			result = s2 + s4;
			break;
		case 5:
			s2 = operator_output(&op[1], op[1].phase, s1 >> 1, tremolo);
			s3 = operator_output(&op[2], op[2].phase, s1 >> 1, tremolo);
			s4 = operator_output(&op[3], op[3].phase, s1 >> 1, tremolo);
			result = s2 + s3 + s4;
			break;
		case 6:
			s2 = operator_output(&op[1], op[1].phase, s1 >> 1, tremolo);
			s3 = operator_output(&op[2], op[2].phase, 0, tremolo);
			s4 = operator_output(&op[3], op[3].phase, 0, tremolo);
			result = s2 + s3 + s4;
			break;
		default:
			s2 = operator_output(&op[1], op[1].phase, 0, tremolo);
			s3 = operator_output(&op[2], op[2].phase, 0, tremolo);
			s4 = operator_output(&op[3], op[3].phase, 0, tremolo);
			result = s1 + s2 + s3 + s4;
			break;
	}
	if (result > 8191) result = 8191;
	else if (result < -8192) result = -8192;
	return result;
}

void karashi_ym2612_generate(struct karashi_ym2612 *chip, int16_t *stereo, size_t frames) {
	size_t frame;
	unsigned number;
	unsigned index;

	for (frame = 0; frame < frames; frame++) {
		int32_t left = 0;
		int32_t right = 0;

		step_timers(chip);
		if (chip->lfo_enabled && ++chip->lfo_divider >= lfo_period[chip->lfo_rate]) {
			chip->lfo_divider = 0;
			chip->lfo_counter = (chip->lfo_counter + 1) & 127;
		}
		if (++chip->envelope_divider >= 3) {
			chip->envelope_divider = 0;
			chip->envelope_counter = (uint16_t) ((chip->envelope_counter + 1) & 0xfff);
			if (!chip->envelope_counter) chip->envelope_counter = 1;
			for (number = 0; number < 6; number++) {
				for (index = 0; index < 4; index++) step_envelope(chip, &chip->channel[number].op[index]);
			}
		}
		for (number = 0; number < 6; number++) {
			struct karashi_ym2612_channel *channel = &chip->channel[number];
			int32_t output = channel_output(chip, channel);
			if (number == 5 && chip->dac_enabled) output = ((int32_t) chip->dac - 128) << 6;
			output &= ~31;
			if (channel->left) left += output;
			if (channel->right) right += output;
		}
		if (stereo) {
			left = left * 2 / 3;
			right = right * 2 / 3;
			stereo[frame * 2] = (int16_t) (left > 32767 ? 32767 : left < -32768 ? -32768 : left);
			stereo[frame * 2 + 1] = (int16_t) (right > 32767 ? 32767 : right < -32768 ? -32768 : right);
		}
	}
}

bool karashi_ym2612_save(const struct karashi_ym2612 *chip, struct karashi_state_writer *writer) {
	unsigned number;
	unsigned index;

	karashi_state_write_bytes(writer, chip->registers, sizeof(chip->registers));
	for (number = 0; number < 6; number++) {
		for (index = 0; index < 4; index++) {
			const struct karashi_ym2612_operator *op = &chip->channel[number].op[index];
			karashi_state_write_u32(writer, op->phase);
			karashi_state_write_u32(writer, (uint32_t) op->output);
			karashi_state_write_u32(writer, (uint32_t) op->previous);
			karashi_state_write_u16(writer, op->level);
			karashi_state_write_u8(writer, op->state);
			karashi_state_write_bool(writer, op->key);
			karashi_state_write_bool(writer, op->ssg_inverted);
			karashi_state_write_bool(writer, op->ssg_held);
		}
		karashi_state_write_u8(writer, chip->channel[number].frequency_latch);
		karashi_state_write_u16(writer, chip->channel[number].frequency);
		karashi_state_write_u8(writer, chip->channel[number].block);
	}
	for (index = 0; index < 3; index++) {
		karashi_state_write_u8(writer, chip->special_latch[index]);
		karashi_state_write_u16(writer, chip->special_frequency[index]);
		karashi_state_write_u8(writer, chip->special_block[index]);
	}
	karashi_state_write_u16(writer, chip->timer_a_counter);
	karashi_state_write_u16(writer, chip->envelope_counter);
	karashi_state_write_u64(writer, chip->busy_until);
	karashi_state_write_u8(writer, chip->timer_b_counter);
	karashi_state_write_u8(writer, chip->timer_b_divider);
	karashi_state_write_u8(writer, chip->status);
	karashi_state_write_u8(writer, chip->address);
	karashi_state_write_u8(writer, chip->part);
	karashi_state_write_u8(writer, chip->lfo_counter);
	karashi_state_write_u8(writer, chip->lfo_divider);
	return karashi_state_write_u8(writer, chip->envelope_divider);
}

bool karashi_ym2612_load(struct karashi_ym2612 *chip, struct karashi_state_reader *reader) {
	static const uint8_t global[] = {0x22, 0x24, 0x25, 0x26, 0x2a, 0x2b};
	uint8_t registers[2][256];
	bool loaded = true;
	unsigned part;
	unsigned address;
	unsigned number;
	unsigned index;

	if (!karashi_state_read_bytes(reader, registers, sizeof(registers))) return false;
	karashi_ym2612_reset(chip);
	for (index = 0; index < sizeof(global); index++) write_register(chip, 0, global[index], registers[0][global[index]]);
	chip->timer_control = registers[0][0x27];
	chip->registers[0][0x27] = registers[0][0x27];
	chip->special_mode = registers[0][0x27] >> 6;
	for (part = 0; part < 2; part++) {
		for (address = 0x30; address < 0xa0; address++) write_register(chip, part, address, registers[part][address]);
		for (address = 0xa4; address < 0xa8; address++) write_register(chip, part, address, registers[part][address]);
		for (address = 0xa0; address < 0xa4; address++) write_register(chip, part, address, registers[part][address]);
		for (address = 0xac; address < 0xb0; address++) write_register(chip, part, address, registers[part][address]);
		for (address = 0xa8; address < 0xac; address++) write_register(chip, part, address, registers[part][address]);
		for (address = 0xb0; address < 0xb8; address++) write_register(chip, part, address, registers[part][address]);
	}
	for (number = 0; number < 6; number++) {
		uint32_t value;
		for (index = 0; index < 4; index++) {
			struct karashi_ym2612_operator *op = &chip->channel[number].op[index];
			loaded = karashi_state_read_u32(reader, &op->phase) && loaded;
			loaded = karashi_state_read_u32(reader, &value) && loaded;
			op->output = (int32_t) value;
			loaded = karashi_state_read_u32(reader, &value) && loaded;
			op->previous = (int32_t) value;
			loaded = karashi_state_read_u16(reader, &op->level) && loaded;
			loaded = karashi_state_read_u8(reader, &op->state) && loaded;
			loaded = karashi_state_read_bool(reader, &op->key) && loaded;
			loaded = karashi_state_read_bool(reader, &op->ssg_inverted) && loaded;
			loaded = karashi_state_read_bool(reader, &op->ssg_held) && loaded;
			op->phase &= 0xfffff;
			op->level &= LEVEL_MAX;
			op->state &= 3;
			if (op->output > 8191 || op->output < -8192) op->output = 0;
			if (op->previous > 8191 || op->previous < -8192) op->previous = 0;
		}
		loaded = karashi_state_read_u8(reader, &chip->channel[number].frequency_latch) && loaded;
		loaded = karashi_state_read_u16(reader, &chip->channel[number].frequency) && loaded;
		loaded = karashi_state_read_u8(reader, &chip->channel[number].block) && loaded;
		chip->channel[number].frequency &= 0x7ff;
		chip->channel[number].block &= 7;
	}
	for (index = 0; index < 3; index++) {
		loaded = karashi_state_read_u8(reader, &chip->special_latch[index]) && loaded;
		loaded = karashi_state_read_u16(reader, &chip->special_frequency[index]) && loaded;
		loaded = karashi_state_read_u8(reader, &chip->special_block[index]) && loaded;
		chip->special_frequency[index] &= 0x7ff;
		chip->special_block[index] &= 7;
	}
	for (number = 0; number < 6; number++) update_channel(chip, &chip->channel[number]);
	loaded = karashi_state_read_u16(reader, &chip->timer_a_counter) && loaded;
	loaded = karashi_state_read_u16(reader, &chip->envelope_counter) && loaded;
	loaded = karashi_state_read_u64(reader, &chip->busy_until) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->timer_b_counter) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->timer_b_divider) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->status) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->address) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->part) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->lfo_counter) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->lfo_divider) && loaded;
	loaded = karashi_state_read_u8(reader, &chip->envelope_divider) && loaded;
	chip->timer_a_counter &= 0x3ff;
	chip->envelope_counter &= 0xfff;
	chip->timer_b_divider &= 15;
	chip->status &= 3;
	chip->part &= 1;
	chip->lfo_counter &= 127;
	chip->envelope_divider %= 3;
	return loaded;
}
