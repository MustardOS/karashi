#include "ym2612.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_register(struct karashi_ym2612 *chip, unsigned part, uint8_t address, uint8_t value) {
	karashi_ym2612_write(chip, part * 2, address);
	karashi_ym2612_write(chip, part * 2 + 1, value);
}

static void simple_voice(struct karashi_ym2612 *chip, unsigned part, unsigned channel) {
	unsigned slot;

	for (slot = 0; slot < 4; slot++) {
		unsigned offset = slot * 4 + channel;
		write_register(chip, part, (uint8_t) (0x30 + offset), 0x01);
		write_register(chip, part, (uint8_t) (0x40 + offset), slot == 3 ? 0x00 : 0x7f);
		write_register(chip, part, (uint8_t) (0x50 + offset), 0x1f);
		write_register(chip, part, (uint8_t) (0x60 + offset), 0x00);
		write_register(chip, part, (uint8_t) (0x70 + offset), 0x00);
		write_register(chip, part, (uint8_t) (0x80 + offset), 0x0f);
	}
	write_register(chip, part, (uint8_t) (0xb0 + channel), 0x07);
	write_register(chip, part, (uint8_t) (0xb4 + channel), 0xc0);
	write_register(chip, part, (uint8_t) (0xa4 + channel), 0x24);
	write_register(chip, part, (uint8_t) (0xa0 + channel), 0x3b);
}

static unsigned crossings(const int16_t *samples, size_t frames, int *peak) {
	unsigned count = 0;
	size_t index;

	*peak = 0;
	for (index = 1; index < frames; index++) {
		if (samples[index * 2 - 2] < 0 && samples[index * 2] >= 0) count++;
		if (abs(samples[index * 2]) > *peak) *peak = abs(samples[index * 2]);
	}
	return count;
}

int main(void) {
	static struct karashi_ym2612 chip;
	static struct karashi_ym2612 restored;
	static int16_t audio[53267 * 2];
	static int16_t first[2048];
	static int16_t second[2048];
	static uint8_t state[4096];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;
	unsigned count;
	int peak;
	int release_peak;

	karashi_ym2612_reset(&chip);
	karashi_ym2612_write_at(&chip, 0, 0x22, 100);
	assert(!(karashi_ym2612_read_at(&chip, 100) & 0x80));
	karashi_ym2612_write_at(&chip, 1, 0x08, 100);
	assert(karashi_ym2612_read_at(&chip, 100) & 0x80);
	assert(karashi_ym2612_read_at(&chip, 1469) & 0x80);
	assert(!(karashi_ym2612_read_at(&chip, 1470) & 0x80));
	karashi_ym2612_reset(&chip);
	karashi_ym2612_generate(&chip, audio, 1000);
	(void) crossings(audio, 1000, &peak);
	assert(peak == 0);

	simple_voice(&chip, 0, 0);
	write_register(&chip, 0, 0x28, 0xf0);
	karashi_ym2612_generate(&chip, audio, 53267);
	count = crossings(audio, 53267, &peak);
	assert(count >= 438 && count <= 442);
	assert(peak > 4000 && peak < 6000);
	assert(audio[1001] == audio[1000]);

	write_register(&chip, 0, 0xb4, 0x80);
	karashi_ym2612_generate(&chip, audio, 64);
	assert(audio[41] == 0);

	write_register(&chip, 0, 0x28, 0x00);
	karashi_ym2612_generate(&chip, audio, 4000);
	(void) crossings(audio + 3000 * 2, 1000, &release_peak);
	assert(release_peak == 0);

	write_register(&chip, 0, 0x2b, 0x80);
	write_register(&chip, 0, 0x2a, 0xff);
	write_register(&chip, 1, 0xb6, 0xc0);
	karashi_ym2612_generate(&chip, audio, 4);
	assert(audio[0] > 5000 && audio[1] > 5000);
	write_register(&chip, 0, 0x2b, 0x00);

	write_register(&chip, 0, 0x24, 0xff);
	write_register(&chip, 0, 0x25, 0x02);
	write_register(&chip, 0, 0x26, 0xfe);
	write_register(&chip, 0, 0x27, 0x0f);
	karashi_ym2612_generate(&chip, NULL, 1);
	assert(karashi_ym2612_read(&chip) == 0);
	karashi_ym2612_generate(&chip, NULL, 1);
	assert(karashi_ym2612_read(&chip) == 1);
	karashi_ym2612_generate(&chip, NULL, 30);
	assert(karashi_ym2612_read(&chip) == 3);
	write_register(&chip, 0, 0x27, 0x3f);
	assert(karashi_ym2612_read(&chip) == 0);

	simple_voice(&chip, 1, 1);
	write_register(&chip, 1, 0xb1, 0x3c);
	write_register(&chip, 0, 0x22, 0x0f);
	write_register(&chip, 1, 0xb5, 0xf7);
	write_register(&chip, 0, 0x28, 0xf5);
	karashi_ym2612_generate(&chip, audio, 500);
	karashi_ym2612_write_at(&chip, 0, 0x22, 500);
	karashi_ym2612_write_at(&chip, 1, 0x0f, 500);
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_ym2612_save(&chip, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	memset(&restored, 0xa5, sizeof(restored));
	assert(karashi_state_reader_begin(&reader, state, size));
	assert(karashi_ym2612_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(karashi_ym2612_read_at(&restored, 1000) & 0x80);
	assert(!(karashi_ym2612_read_at(&restored, 1848) & 0x80));
	karashi_ym2612_generate(&chip, first, 1024);
	karashi_ym2612_generate(&restored, second, 1024);
	(void) crossings(first, 1024, &peak);
	assert(peak > 1000);
	assert(memcmp(first, second, sizeof(first)) == 0);
	puts("YM2612 pitch, level, pan, release, DAC, timers, LFO and state: pass");
	return 0;
}
