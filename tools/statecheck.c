#include "libretro.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * statecheck CORE ROM [WARMUP] [SPAN]
 * Saves a state after WARMUP frames, runs SPAN frames, restores and runs SPAN frames again.
 * Both runs must give the same video and audio.
 */

static uint64_t hash;
static unsigned frame_number;

static void mix(const void *data, size_t size) {
	const uint8_t *bytes = data;
	size_t index;

	for (index = 0; index < size; index++) hash = (hash ^ bytes[index]) * UINT64_C(1099511628211);
}

static bool environment(unsigned command, void *data) {
	if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY && getenv("KARASHI_SYSTEM_DIR")) {
		*(const char **) data = getenv("KARASHI_SYSTEM_DIR");
		return true;
	}
	if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
	if (command == RETRO_ENVIRONMENT_GET_CAN_DUPE) {
		*(bool *) data = true;
		return true;
	}
	return false;
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch) {
	unsigned y;

	if (!data || (getenv("STATECHECK") && strcmp(getenv("STATECHECK"), "audio") == 0)) return;
	for (y = 0; y < height; y++) mix((const uint8_t *) data + y * pitch, width * 2);
}

static void audio(int16_t left, int16_t right) {
	mix(&left, sizeof(left));
	mix(&right, sizeof(right));
}

static size_t audio_batch(const int16_t *data, size_t frames) {
	if (getenv("STATECHECK") && strcmp(getenv("STATECHECK"), "video") == 0) return frames;
	mix(data, frames * 4);
	return frames;
}

static void input_poll(void) {
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) device;
	(void) index;
	if (port) return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_START) return frame_number % 90 < 4;
	if (id == RETRO_DEVICE_ID_JOYPAD_RIGHT) return frame_number % 64 < 40;
	if (id == RETRO_DEVICE_ID_JOYPAD_B) return frame_number % 37 < 6;
	return 0;
}

int main(int argc, char **argv) {
	void *library;
	struct retro_game_info game;
	FILE *file;
	long size;
	uint8_t *data;
	uint8_t *state;
	size_t state_size;
	unsigned warmup = argc > 3 ? (unsigned) atoi(argv[3]) : 900;
	unsigned span = argc > 4 ? (unsigned) atoi(argv[4]) : 300;
	uint64_t first;
	unsigned frame;
	void (*run)(void);

	if (argc < 3) {
		fprintf(stderr, "Usage: %s CORE ROM [WARMUP] [SPAN]\n", argv[0]);
		return 2;
	}
	library = dlopen(argv[1], RTLD_NOW);
	if (!library) {
		fprintf(stderr, "Could not load core: %s\n", dlerror());
		return 2;
	}
	file = fopen(argv[2], "rb");
	if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) {
		fprintf(stderr, "Could not open ROM: %s\n", argv[2]);
		return 2;
	}
	data = malloc((size_t) size);
	if (!data || fread(data, 1, (size_t) size, file) != (size_t) size) {
		fprintf(stderr, "Could not read ROM: %s\n", argv[2]);
		return 2;
	}
	fclose(file);
	memset(&game, 0, sizeof(game));
	game.path = argv[2];
	game.data = data;
	game.size = (size_t) size;
	((void (*)(retro_environment_t)) dlsym(library, "retro_set_environment"))(environment);
	((void (*)(retro_video_refresh_t)) dlsym(library, "retro_set_video_refresh"))(video);
	((void (*)(retro_audio_sample_t)) dlsym(library, "retro_set_audio_sample"))(audio);
	((void (*)(retro_audio_sample_batch_t)) dlsym(library, "retro_set_audio_sample_batch"))(audio_batch);
	((void (*)(retro_input_poll_t)) dlsym(library, "retro_set_input_poll"))(input_poll);
	((void (*)(retro_input_state_t)) dlsym(library, "retro_set_input_state"))(input_state);
	((void (*)(void)) dlsym(library, "retro_init"))();
	if (!((bool (*)(const struct retro_game_info *)) dlsym(library, "retro_load_game"))(&game)) {
		fprintf(stderr, "Core rejected ROM: %s\n", argv[2]);
		return 2;
	}
	run = (void (*)(void)) dlsym(library, "retro_run");
	for (frame_number = 0; frame_number < warmup; frame_number++) run();
	state_size = ((size_t (*)(void)) dlsym(library, "retro_serialize_size"))();
	state = malloc(state_size);
	if (!state || !((bool (*)(void *, size_t)) dlsym(library, "retro_serialize"))(state, state_size)) {
		printf("save failed\n");
		return 1;
	}
	hash = UINT64_C(1469598103934665603);
	for (frame = 0; frame < span; frame++, frame_number++) run();
	first = hash;
	if (!((bool (*)(const void *, size_t)) dlsym(library, "retro_unserialize"))(state, state_size)) {
		printf("load failed\n");
		return 1;
	}
	frame_number = warmup;
	hash = UINT64_C(1469598103934665603);
	for (frame = 0; frame < span; frame++, frame_number++) run();
	printf("%s\n", first == hash ? "same" : "DIFFERENT");
	return first == hash ? 0 : 3;
}
