#include "sn76489.h"

#include <stdbool.h>
#include <string.h>

static const int16_t amplitude[16] = {
	4096, 3254, 2585, 2053, 1631, 1296, 1029, 817,
	649, 516, 410, 326, 259, 206, 164, 0
};

void karashi_sn76489_reset(struct karashi_sn76489 *psg, uint32_t clock_rate) {
	unsigned channel;

	memset(psg, 0, sizeof(*psg));
	psg->clock_rate = clock_rate;
	psg->lfsr = 0x8000;
	psg->stereo = 0xff;
	for (channel = 0; channel < 4; channel++) psg->volume[channel] = 15;
}

void karashi_sn76489_set_stereo(struct karashi_sn76489 *psg, uint8_t value) {
	psg->stereo = value;
}

void karashi_sn76489_write(struct karashi_sn76489 *psg, uint8_t value) {
	unsigned channel;
	bool volume;

	if (value & 0x80) {
		channel = (value >> 5) & 3;
		volume = (value & 0x10) != 0;
		psg->latched_channel = (uint8_t) channel;
		psg->latched_volume = volume;
		if (volume) psg->volume[channel] = value & 15;
		else if (channel == 3) {
			psg->noise = value & 7;
			psg->lfsr = 0x8000;
		} else psg->tone[channel] = (uint16_t) ((psg->tone[channel] & 0x3f0) | (value & 15));
		return;
	}

	channel = psg->latched_channel;
	if (psg->latched_volume) psg->volume[channel] = value & 15;
	else if (channel == 3) {
		psg->noise = value & 7;
		psg->lfsr = 0x8000;
	} else psg->tone[channel] = (uint16_t) ((psg->tone[channel] & 15) | ((value & 0x3f) << 4));
}

static void shift_noise(struct karashi_sn76489 *psg) {
	unsigned feedback;

	if (psg->noise & 4) feedback = (psg->lfsr ^ (psg->lfsr >> 3)) & 1;
	else feedback = psg->lfsr & 1;
	psg->lfsr = (uint16_t) ((psg->lfsr >> 1) | (feedback << 15));
}

static unsigned tone_period(const struct karashi_sn76489 *psg, unsigned channel) {
	if (channel < 3) return psg->tone[channel];
	if ((psg->noise & 3) == 3) return psg->tone[2] > 1 ? psg->tone[2] : 1;
	return 16u << (psg->noise & 3);
}

static unsigned high_ticks(struct karashi_sn76489 *psg, unsigned channel, unsigned ticks) {
	unsigned period = tone_period(psg, channel);
	uint8_t bit = (uint8_t) (1u << channel);
	unsigned high = 0;

	if (channel < 3 && period <= 1) return ticks;
	if (!psg->counter[channel] || psg->counter[channel] > period * 2u) psg->counter[channel] = period;
	while (ticks) {
		unsigned span = psg->counter[channel] < ticks ? psg->counter[channel] : ticks;
		bool level = channel < 3 ? (psg->outputs & bit) != 0 : (psg->lfsr & 1) != 0;
		if (level) high += span;
		ticks -= span;
		psg->counter[channel] -= span;
		if (!psg->counter[channel]) {
			psg->counter[channel] = period;
			if (channel < 3) psg->outputs ^= bit;
			else {
				psg->noise_edge ^= 1;
				if (psg->noise_edge) shift_noise(psg);
			}
		}
	}
	return high;
}

void karashi_sn76489_generate(struct karashi_sn76489 *psg, int16_t *stereo,
	size_t frames, uint32_t sample_rate) {
	uint32_t step;
	size_t frame;
	unsigned channel;

	if (psg->step_clock != psg->clock_rate || psg->step_rate != sample_rate) {
		psg->step = (uint32_t) (((uint64_t) psg->clock_rate << 16) / ((uint64_t) 16 * sample_rate));
		psg->step_clock = psg->clock_rate;
		psg->step_rate = sample_rate;
	}
	step = psg->step;
	for (frame = 0; frame < frames; frame++) {
		int left = 0;
		int right = 0;
		unsigned ticks;

		psg->tick_fraction += step;
		ticks = psg->tick_fraction >> 16;
		psg->tick_fraction &= 0xffff;
		for (channel = 0; channel < 4; channel++) {
			int level = 0;
			if (ticks) {
				int high = (int) high_ticks(psg, channel, ticks);
				int peak = amplitude[psg->volume[channel]];
				if (peak) level = peak * (2 * high - (int) ticks) / (int) ticks;
			}
			if (psg->stereo & (0x10 << channel)) left += level;
			if (psg->stereo & (0x01 << channel)) right += level;
		}
		stereo[frame * 2] = (int16_t) left;
		stereo[frame * 2 + 1] = (int16_t) right;
	}
}

bool karashi_sn76489_save(const struct karashi_sn76489 *psg, struct karashi_state_writer *writer) {
	unsigned channel;

	for (channel = 0; channel < 3; channel++) karashi_state_write_u16(writer, psg->tone[channel]);
	karashi_state_write_bytes(writer, psg->volume, sizeof(psg->volume));
	karashi_state_write_u8(writer, psg->noise);
	karashi_state_write_u8(writer, psg->latched_channel);
	karashi_state_write_u8(writer, psg->latched_volume);
	karashi_state_write_u8(writer, psg->stereo);
	for (channel = 0; channel < 4; channel++) karashi_state_write_u32(writer, psg->counter[channel]);
	karashi_state_write_u32(writer, psg->tick_fraction);
	karashi_state_write_u8(writer, psg->outputs);
	karashi_state_write_u8(writer, psg->noise_edge);
	return karashi_state_write_u16(writer, psg->lfsr);
}

bool karashi_sn76489_load(struct karashi_sn76489 *psg, struct karashi_state_reader *reader) {
	unsigned channel;

	for (channel = 0; channel < 3; channel++) karashi_state_read_u16(reader, &psg->tone[channel]);
	karashi_state_read_bytes(reader, psg->volume, sizeof(psg->volume));
	karashi_state_read_u8(reader, &psg->noise);
	karashi_state_read_u8(reader, &psg->latched_channel);
	karashi_state_read_u8(reader, &psg->latched_volume);
	karashi_state_read_u8(reader, &psg->stereo);
	for (channel = 0; channel < 4; channel++) karashi_state_read_u32(reader, &psg->counter[channel]);
	karashi_state_read_u32(reader, &psg->tick_fraction);
	karashi_state_read_u8(reader, &psg->outputs);
	karashi_state_read_u8(reader, &psg->noise_edge);
	if (!karashi_state_read_u16(reader, &psg->lfsr)) return false;
	psg->tick_fraction &= 0xffff;
	psg->outputs &= 7;
	psg->noise_edge &= 1;
	for (channel = 0; channel < 4; channel++) psg->counter[channel] &= 0xffff;
	for (channel = 0; channel < 3; channel++) psg->tone[channel] &= 0x3ff;
	for (channel = 0; channel < 4; channel++) psg->volume[channel] &= 15;
	psg->noise &= 7;
	psg->latched_channel &= 3;
	return true;
}
