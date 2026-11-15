#ifndef KARASHI_SN76489_H
#define KARASHI_SN76489_H

#include "state.h"

#include <stddef.h>
#include <stdint.h>

struct karashi_sn76489 {
	uint16_t tone[3];
	uint8_t volume[4];
	uint8_t noise;
	uint8_t latched_channel;
	uint8_t latched_volume;
	uint8_t stereo;
	uint32_t counter[4];
	uint32_t tick_fraction;
	uint8_t outputs;
	uint8_t noise_edge;
	uint16_t lfsr;
	uint32_t clock_rate;
	uint32_t step;
	uint32_t step_clock;
	uint32_t step_rate;
};

void karashi_sn76489_reset(struct karashi_sn76489 *psg, uint32_t clock_rate);
void karashi_sn76489_write(struct karashi_sn76489 *psg, uint8_t value);
void karashi_sn76489_set_stereo(struct karashi_sn76489 *psg, uint8_t value);
bool karashi_sn76489_save(const struct karashi_sn76489 *psg, struct karashi_state_writer *writer);
bool karashi_sn76489_load(struct karashi_sn76489 *psg, struct karashi_state_reader *reader);
void karashi_sn76489_generate(struct karashi_sn76489 *psg, int16_t *stereo,
	size_t frames, uint32_t sample_rate);

#endif
