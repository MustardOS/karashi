#include "sn76489.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void test_register_protocol(void) {
	struct karashi_sn76489 psg;

	karashi_sn76489_reset(&psg, 3579545);
	karashi_sn76489_write(&psg, 0x8a);
	karashi_sn76489_write(&psg, 0x15);
	assert(psg.tone[0] == 0x15a);
	karashi_sn76489_write(&psg, 0x94);
	assert(psg.volume[0] == 4);
	karashi_sn76489_write(&psg, 0xe7);
	assert(psg.noise == 7);
	assert(psg.lfsr == 0x8000);
}

static void test_silence_and_tone(void) {
	struct karashi_sn76489 psg;
	int16_t audio[512 * 2];
	bool changed = false;
	unsigned frame;

	karashi_sn76489_reset(&psg, 3579545);
	memset(audio, 0x7f, sizeof(audio));
	karashi_sn76489_generate(&psg, audio, 512, 48000);
	for (frame = 0; frame < 512 * 2; frame++) assert(audio[frame] == 0);
	karashi_sn76489_write(&psg, 0x80);
	karashi_sn76489_write(&psg, 0x20);
	karashi_sn76489_write(&psg, 0x90);
	karashi_sn76489_generate(&psg, audio, 512, 48000);
	for (frame = 1; frame < 512; frame++) {
		assert(audio[frame * 2] == audio[frame * 2 + 1]);
		if (audio[frame * 2] != audio[(frame - 1) * 2]) changed = true;
	}
	assert(changed);
}

static void test_determinism(void) {
	struct karashi_sn76489 first;
	struct karashi_sn76489 second;
	int16_t first_audio[256 * 2];
	int16_t second_audio[256 * 2];

	karashi_sn76489_reset(&first, 3579545);
	karashi_sn76489_write(&first, 0xe4);
	karashi_sn76489_write(&first, 0xf2);
	second = first;
	karashi_sn76489_generate(&first, first_audio, 256, 48000);
	karashi_sn76489_generate(&second, second_audio, 256, 48000);
	assert(memcmp(first_audio, second_audio, sizeof(first_audio)) == 0);
}

static void test_band_limit(void) {
	static int16_t audio[48000 * 2];
	struct karashi_sn76489 psg;
	unsigned crossings = 0;
	int peak = 0;
	unsigned frame;

	karashi_sn76489_reset(&psg, 3579545);
	karashi_sn76489_write(&psg, 0x80 | (254 & 15));
	karashi_sn76489_write(&psg, 254 >> 4);
	karashi_sn76489_write(&psg, 0x90);
	karashi_sn76489_generate(&psg, audio, 48000, 48000);
	for (frame = 1; frame < 48000; frame++) {
		if (audio[frame * 2 - 2] < 0 && audio[frame * 2] >= 0) crossings++;
	}
	assert(crossings >= 438 && crossings <= 442);

	karashi_sn76489_write(&psg, 0x82);
	karashi_sn76489_write(&psg, 0x00);
	karashi_sn76489_generate(&psg, audio, 4800, 48000);
	for (frame = 100; frame < 4800; frame++) {
		int value = audio[frame * 2] < 0 ? -audio[frame * 2] : audio[frame * 2];
		if (value > peak) peak = value;
	}
	assert(peak < 4096 / 3);

	karashi_sn76489_write(&psg, 0x81);
	karashi_sn76489_write(&psg, 0x00);
	karashi_sn76489_generate(&psg, audio, 64, 48000);
	for (frame = 0; frame < 64; frame++) assert(audio[frame * 2] == 4096);
	karashi_sn76489_write(&psg, 0x93);
	karashi_sn76489_generate(&psg, audio, 64, 48000);
	for (frame = 0; frame < 64; frame++) assert(audio[frame * 2] == 2053);

	karashi_sn76489_write(&psg, 0x9f);
	karashi_sn76489_write(&psg, 0xe4);
	karashi_sn76489_write(&psg, 0xf0);
	karashi_sn76489_generate(&psg, audio, 4800, 48000);
	peak = 0;
	for (frame = 1; frame < 4800; frame++) {
		if (audio[frame * 2] != audio[frame * 2 - 2]) peak++;
	}
	assert(peak > 300);
}

int main(void) {
	test_band_limit();
	test_register_protocol();
	test_silence_and_tone();
	test_determinism();
	puts("SN76489 registers, band-limited tone, noise, sample playback and deterministic audio: pass");
	return 0;
}
