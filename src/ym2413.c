#include "ym2413.h"

#include <math.h>
#include <string.h>

enum {
	STATE_DAMP,
	STATE_ATTACK,
	STATE_DECAY,
	STATE_SUSTAIN,
	STATE_RELEASE
};

#define LEVEL_MAX 127
#define DAMP_END 123
#define RHYTHM 0x20
#define HALF_PI_STEP (3.14159265358979323846 / 512.0)

const uint8_t karashi_ym2413_instruments[19][8] = {
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	{0x71, 0x61, 0x1e, 0x17, 0xd0, 0x78, 0x00, 0x17},
	{0x13, 0x41, 0x1a, 0x0d, 0xd8, 0xf7, 0x23, 0x13},
	{0x13, 0x01, 0x99, 0x00, 0xf2, 0xc4, 0x11, 0x23},
	{0x31, 0x61, 0x0e, 0x07, 0xa8, 0x64, 0x70, 0x27},
	{0x32, 0x21, 0x1e, 0x06, 0xe0, 0x76, 0x00, 0x28},
	{0x31, 0x22, 0x16, 0x05, 0xe0, 0x71, 0x00, 0x18},
	{0x21, 0x61, 0x1d, 0x07, 0x82, 0x81, 0x10, 0x07},
	{0x23, 0x21, 0x2d, 0x14, 0xa2, 0x72, 0x00, 0x07},
	{0x61, 0x61, 0x1b, 0x06, 0x64, 0x65, 0x10, 0x17},
	{0x41, 0x61, 0x0b, 0x18, 0x85, 0xf7, 0x71, 0x07},
	{0x13, 0x01, 0x83, 0x11, 0xfa, 0xe4, 0x10, 0x04},
	{0x17, 0xc1, 0x24, 0x07, 0xf8, 0xf8, 0x22, 0x12},
	{0x61, 0x50, 0x0c, 0x05, 0xc2, 0xf5, 0x20, 0x42},
	{0x01, 0x01, 0x55, 0x03, 0xc9, 0x95, 0x03, 0x02},
	{0x61, 0x41, 0x89, 0x03, 0xf1, 0xe4, 0x40, 0x13},
	{0x01, 0x01, 0x18, 0x0f, 0xdf, 0xf8, 0x6a, 0x6d},
	{0x01, 0x01, 0x00, 0x00, 0xc8, 0xd8, 0xa7, 0x48},
	{0x05, 0x01, 0x00, 0x00, 0xf8, 0xaa, 0x59, 0x55}
};

static const uint8_t multipliers[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
static const uint8_t key_scale[16] = {0, 48, 64, 74, 80, 86, 90, 94, 96, 100, 102, 104, 106, 108, 110, 112};
static const uint8_t step_pattern[4][8] = {
	{0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}
};
static const int8_t vibrato_shape[8] = {0, 1, 2, 1, 0, -1, -2, -1};

static uint16_t log_sine[256];
static uint16_t exponent[256];
static bool tables_ready;

struct operator_params {
	bool tremolo;
	bool vibrato;
	bool sustained;
	bool key_rate;
	bool rectify;
	uint8_t multiple;
	uint8_t key_level;
	uint8_t attack;
	uint8_t decay;
	uint8_t sustain_level;
	uint8_t release;
};

static void build_tables(void) {
	unsigned index;

	for (index = 0; index < 256; index++) {
		log_sine[index] = (uint16_t) lround(-log2(sin((index + 0.5) * HALF_PI_STEP)) * 256.0);
		exponent[index] = (uint16_t) (lround(pow(2.0, (255 - index) / 256.0) * 1024.0) - 1024);
	}
	tables_ready = true;
}

static const uint8_t *patch_for(const struct karashi_ym2413 *chip, unsigned channel) {
	unsigned instrument = chip->registers[0x30 + channel] >> 4;

	if ((chip->registers[0x0e] & RHYTHM) && channel >= 6) return karashi_ym2413_instruments[16 + channel - 6];
	return instrument ? karashi_ym2413_instruments[instrument] : chip->registers;
}

static struct operator_params params_for(const uint8_t *patch, unsigned carrier) {
	struct operator_params params;
	uint8_t flags = patch[carrier];

	params.tremolo = (flags & 0x80) != 0;
	params.vibrato = (flags & 0x40) != 0;
	params.sustained = (flags & 0x20) != 0;
	params.key_rate = (flags & 0x10) != 0;
	params.multiple = flags & 0x0f;
	params.key_level = patch[2 + carrier] >> 6;
	params.rectify = (patch[3] & (carrier ? 0x10 : 0x08)) != 0;
	params.attack = patch[4 + carrier] >> 4;
	params.decay = patch[4 + carrier] & 0x0f;
	params.sustain_level = patch[6 + carrier] >> 4;
	params.release = patch[6 + carrier] & 0x0f;
	return params;
}

static unsigned frequency(const struct karashi_ym2413 *chip, unsigned channel) {
	return chip->registers[0x10 + channel] | (chip->registers[0x20 + channel] & 1u) << 8;
}

static unsigned block(const struct karashi_ym2413 *chip, unsigned channel) {
	return (chip->registers[0x20 + channel] >> 1) & 7;
}

static bool slot_key(const struct karashi_ym2413 *chip, unsigned index) {
	static const uint8_t rhythm_bits[6] = {0x10, 0x10, 0x01, 0x08, 0x04, 0x02};
	unsigned channel = index >> 1;
	bool key = (chip->registers[0x20 + channel] & 0x10) != 0;

	if ((chip->registers[0x0e] & RHYTHM) && channel >= 6) key = key || (chip->registers[0x0e] & rhythm_bits[index - 12]);
	return key;
}

static unsigned base_rate(
	const struct karashi_ym2413 *chip, const struct karashi_ym2413_slot *slot, const struct operator_params *params,
	unsigned channel
) {
	switch (slot->state) {
		case STATE_DAMP: return 12;
		case STATE_ATTACK: return params->attack;
		case STATE_DECAY: return params->decay;
		case STATE_SUSTAIN: return params->sustained ? 0 : params->release;
		default:
			if (chip->registers[0x20 + channel] & 0x20) return 5;
			return params->sustained ? params->release : 7;
	}
}

static unsigned envelope_steps(uint32_t counter, unsigned rate, unsigned fraction) {
	unsigned shift;

	if (!rate) return 0;
	if (rate >= 15) return 2;
	if (rate == 14) return 1;
	if (rate == 13) return step_pattern[fraction][counter & 7];
	shift = 13 - rate;
	if (counter & ((1u << shift) - 1)) return 0;
	return step_pattern[fraction][(counter >> shift) & 7];
}

static void update_envelope(
	struct karashi_ym2413 *chip, struct karashi_ym2413_slot *slot, const struct operator_params *params, unsigned channel
) {
	unsigned scale = params->key_rate ? (block(chip, channel) << 1 | frequency(chip, channel) >> 8) : block(chip, channel) >> 1;
	unsigned base = base_rate(chip, slot, params, channel);
	unsigned rate = base ? base + (scale >> 2) : 0;
	unsigned steps;

	if (rate > 15) rate = 15;
	steps = envelope_steps(chip->counter, rate, scale & 3);
	if (slot->state == STATE_ATTACK) {
		if (rate == 15) slot->level = 0;
		else if (steps) {
			unsigned shift = rate >= 12 ? 16 - rate : 4;
			unsigned drop = (slot->level >> shift) + 1u;
			slot->level = (uint8_t) (slot->level > drop ? slot->level - drop : 0);
		}
		if (!slot->level) slot->state = STATE_DECAY;
		return;
	}
	if (steps) slot->level = (uint8_t) (slot->level + steps > LEVEL_MAX ? LEVEL_MAX : slot->level + steps);
	if (slot->state == STATE_DAMP && slot->level >= DAMP_END) {
		slot->phase = 0;
		slot->state = STATE_ATTACK;
		if (params->attack == 15) {
			slot->level = 0;
			slot->state = STATE_DECAY;
		}
	} else if (slot->state == STATE_DECAY && (slot->level >> 3) >= params->sustain_level) {
		slot->state = STATE_SUSTAIN;
	}
}

static void update_phase(
	struct karashi_ym2413 *chip, struct karashi_ym2413_slot *slot, const struct operator_params *params, unsigned channel
) {
	int number = (int) frequency(chip, channel);

	if (params->vibrato) number += (number >> 6) * vibrato_shape[(chip->counter >> 10) & 7] / 2;
	if (number < 0) number = 0;
	slot->phase = (slot->phase + (((uint32_t) number << block(chip, channel)) * multipliers[params->multiple] >> 2)) & 0x3ffff;
}

static unsigned tremolo_level(uint32_t counter) {
	unsigned index = (counter >> 6) % 210;

	return (index < 105 ? index : 209 - index) * 13 / 104;
}

static unsigned key_level(const struct karashi_ym2413 *chip, unsigned channel, unsigned setting) {
	int level = key_scale[frequency(chip, channel) >> 5] - 16 * (7 - (int) block(chip, channel));

	if (!setting || level <= 0) return 0;
	return (unsigned) level >> (3 - setting);
}

static int32_t wave(unsigned index, unsigned attenuation, bool rectify) {
	unsigned quarter = index & 0xff;
	unsigned level;
	int32_t amplitude;

	index &= 0x3ff;
	if (index & 0x100) quarter = 0xff - quarter;
	if (rectify && (index & 0x200)) return 0;
	level = log_sine[quarter] + (attenuation << 4);
	if ((level >> 8) >= 12) return 0;
	amplitude = (int32_t) ((exponent[level & 0xff] + 1024u) >> (level >> 8));
	return index & 0x200 ? -amplitude : amplitude;
}

static unsigned attenuation(
	const struct karashi_ym2413 *chip, const struct karashi_ym2413_slot *slot, const struct operator_params *params,
	unsigned channel, unsigned volume
) {
	unsigned total = slot->level + volume + key_level(chip, channel, params->key_level);

	if (params->tremolo) total += tremolo_level(chip->counter);
	return total > LEVEL_MAX ? LEVEL_MAX : total;
}

static void step_noise(struct karashi_ym2413 *chip, unsigned count) {
	while (count--) chip->noise = chip->noise & 1 ? (chip->noise >> 1) ^ 0x400181u : chip->noise >> 1;
}

void karashi_ym2413_reset(struct karashi_ym2413 *chip) {
	unsigned index;

	if (!tables_ready) build_tables();
	memset(chip, 0, sizeof(*chip));
	for (index = 0; index < 18; index++) {
		chip->slot[index].level = LEVEL_MAX;
		chip->slot[index].state = STATE_RELEASE;
	}
	chip->noise = 1;
}

void karashi_ym2413_write(struct karashi_ym2413 *chip, bool data, uint8_t value) {
	if (!data) chip->address = value & 0x3f;
	else chip->registers[chip->address] = value;
}

int32_t karashi_ym2413_sample(struct karashi_ym2413 *chip) {
	bool rhythm = (chip->registers[0x0e] & RHYTHM) != 0;
	unsigned channels = rhythm ? 6 : 9;
	int32_t mix = 0;
	unsigned channel;
	unsigned index;

	for (index = 0; index < 18; index++) {
		struct karashi_ym2413_slot *slot = &chip->slot[index];
		bool key = slot_key(chip, index);
		if (key && !slot->on) slot->state = STATE_DAMP;
		else if (!key && slot->on) slot->state = STATE_RELEASE;
		slot->on = key;
	}
	for (channel = 0; channel < 9; channel++) {
		const uint8_t *patch = patch_for(chip, channel);
		struct operator_params mod = params_for(patch, 0);
		struct operator_params car = params_for(patch, 1);
		struct karashi_ym2413_slot *modulator = &chip->slot[channel * 2];
		struct karashi_ym2413_slot *carrier = &chip->slot[channel * 2 + 1];
		update_envelope(chip, modulator, &mod, channel);
		update_envelope(chip, carrier, &car, channel);
		update_phase(chip, modulator, &mod, channel);
		update_phase(chip, carrier, &car, channel);
		if (channel < channels || channel == 6) {
			unsigned feedback = patch[3] & 7;
			int32_t input = feedback ? (modulator->output[0] + modulator->output[1]) >> (9 - feedback) : 0;
			int32_t modulation = wave(
				(modulator->phase >> 8) + (unsigned) input,
				attenuation(chip, modulator, &mod, channel, (unsigned) (patch[2] & 0x3f) << 1), mod.rectify
			);
			unsigned volume = (chip->registers[0x30 + channel] & 0x0f) << 3;
			int32_t output;
			modulator->output[1] = modulator->output[0];
			modulator->output[0] = (int16_t) modulation;
			output = wave((carrier->phase >> 8) + (unsigned) modulation, attenuation(chip, carrier, &car, channel, volume), car.rectify);
			mix += channel < channels ? output : output * 2;
		}
	}
	if (rhythm) {
		struct karashi_ym2413_slot *hat = &chip->slot[14];
		struct karashi_ym2413_slot *snare = &chip->slot[15];
		struct karashi_ym2413_slot *tom = &chip->slot[16];
		struct karashi_ym2413_slot *cymbal = &chip->slot[17];
		unsigned hat_phase = hat->phase >> 8;
		unsigned cymbal_phase = cymbal->phase >> 8;
		unsigned noise_bit;
		unsigned clang = (((hat_phase >> 2) ^ (hat_phase >> 7)) | ((hat_phase >> 3) ^ (cymbal_phase >> 5))
		                  | ((cymbal_phase >> 3) ^ (cymbal_phase >> 5))) & 1;
		const uint8_t *hat_patch = karashi_ym2413_instruments[17];
		const uint8_t *tom_patch = karashi_ym2413_instruments[18];
		struct operator_params hat_params = params_for(hat_patch, 0);
		struct operator_params snare_params = params_for(hat_patch, 1);
		struct operator_params tom_params = params_for(tom_patch, 0);
		struct operator_params cymbal_params = params_for(tom_patch, 1);
		step_noise(chip, 14);
		noise_bit = chip->noise & 1;
		mix += 2 * wave(
			clang ? (noise_bit ? 0x2d0 : 0x234) : (noise_bit ? 0x34 : 0xd0),
			attenuation(chip, hat, &hat_params, 7, (unsigned) (chip->registers[0x37] >> 4) << 3), false
		);
		mix += 2 * wave(
			((snare->phase >> 8) & 0x100 ? 0x200 : 0) | (noise_bit ? 0x100 : 0),
			attenuation(chip, snare, &snare_params, 7, (unsigned) (chip->registers[0x37] & 0x0f) << 3), false
		);
		step_noise(chip, 2);
		mix += 2 * wave(
			tom->phase >> 8, attenuation(chip, tom, &tom_params, 8, (unsigned) (chip->registers[0x38] >> 4) << 3), false
		);
		mix += 2 * wave(
			clang ? 0x300 : 0x100,
			attenuation(chip, cymbal, &cymbal_params, 8, (unsigned) (chip->registers[0x38] & 0x0f) << 3), false
		);
		step_noise(chip, 2);
	}
	chip->counter++;
	return mix;
}

bool karashi_ym2413_save(const struct karashi_ym2413 *chip, struct karashi_state_writer *writer) {
	unsigned index;

	karashi_state_write_bytes(writer, chip->registers, sizeof(chip->registers));
	karashi_state_write_u8(writer, chip->address);
	for (index = 0; index < 18; index++) {
		const struct karashi_ym2413_slot *slot = &chip->slot[index];
		karashi_state_write_u32(writer, slot->phase);
		karashi_state_write_u16(writer, (uint16_t) slot->output[0]);
		karashi_state_write_u16(writer, (uint16_t) slot->output[1]);
		karashi_state_write_u8(writer, slot->level);
		karashi_state_write_u8(writer, slot->state);
		karashi_state_write_bool(writer, slot->on);
	}
	karashi_state_write_u32(writer, chip->counter);
	return karashi_state_write_u32(writer, chip->noise);
}

bool karashi_ym2413_load(struct karashi_ym2413 *chip, struct karashi_state_reader *reader) {
	bool loaded = karashi_state_read_bytes(reader, chip->registers, sizeof(chip->registers));
	unsigned index;

	loaded = karashi_state_read_u8(reader, &chip->address) && loaded;
	for (index = 0; index < 18; index++) {
		struct karashi_ym2413_slot *slot = &chip->slot[index];
		uint16_t output[2] = {0, 0};
		loaded = karashi_state_read_u32(reader, &slot->phase) && loaded;
		loaded = karashi_state_read_u16(reader, &output[0]) && loaded;
		loaded = karashi_state_read_u16(reader, &output[1]) && loaded;
		loaded = karashi_state_read_u8(reader, &slot->level) && loaded;
		loaded = karashi_state_read_u8(reader, &slot->state) && loaded;
		loaded = karashi_state_read_bool(reader, &slot->on) && loaded;
		slot->output[0] = (int16_t) output[0];
		slot->output[1] = (int16_t) output[1];
		slot->phase &= 0x3ffff;
		if (slot->level > LEVEL_MAX) slot->level = LEVEL_MAX;
		if (slot->state > STATE_RELEASE) slot->state = STATE_RELEASE;
	}
	loaded = karashi_state_read_u32(reader, &chip->counter) && loaded;
	loaded = karashi_state_read_u32(reader, &chip->noise) && loaded;
	chip->address &= 0x3f;
	if (!chip->noise) chip->noise = 1;
	if (!tables_ready) build_tables();
	return loaded;
}
