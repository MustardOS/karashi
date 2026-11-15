#include "libretro.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ramdiff CORE REFERENCE ROM FRAMES [OFFSET]
 * Runs two Libretro cores on the same content without input and compares their system RAM each frame.
 * The reference core is a black box: only its Libretro memory interface is read.
 */

#define RAM_SIZE 65536

static unsigned input_frame;

struct core {
	void *library;
	void (*run)(void);
	void *(*memory)(unsigned);
	size_t (*memory_size)(unsigned);
	uint8_t *history;
	bool swapped;
};

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
	if (command == RETRO_ENVIRONMENT_GET_VARIABLE && getenv("KARASHI_SYSTEM_DIR")) {
		struct retro_variable *variable = data;
		if (variable->key && strcmp(variable->key, "genesis_plus_gx_bios") == 0) {
			variable->value = "enabled";
			return true;
		}
	}
	return false;
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch) {
	(void) data;
	(void) width;
	(void) height;
	(void) pitch;
}

static void audio(int16_t left, int16_t right) {
	(void) left;
	(void) right;
}

static size_t audio_batch(const int16_t *data, size_t frames) {
	(void) data;
	return frames;
}

static void input_poll(void) {
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	unsigned phase = input_frame % 120;

	(void) port;
	(void) device;
	(void) index;
	if (getenv("KARASHI_SCRIPTED_INPUT") && !port && device == RETRO_DEVICE_JOYPAD && input_frame >= 150) {
		if (id == RETRO_DEVICE_ID_JOYPAD_START) return phase < 6;
		if (id == RETRO_DEVICE_ID_JOYPAD_B) return phase >= 40 && phase < 46;
		if (id == RETRO_DEVICE_ID_JOYPAD_A) return phase >= 80 && phase < 86;
	}
	return 0;
}

static bool load(struct core *core, const char *path, const struct retro_game_info *game, unsigned frames) {
	void (*set_environment)(retro_environment_t);
	void (*init)(void);
	bool (*load_game)(const struct retro_game_info *);

	core->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!core->library) return false;
	set_environment = (void (*)(retro_environment_t)) dlsym(core->library, "retro_set_environment");
	init = (void (*)(void)) dlsym(core->library, "retro_init");
	load_game = (bool (*)(const struct retro_game_info *)) dlsym(core->library, "retro_load_game");
	core->run = (void (*)(void)) dlsym(core->library, "retro_run");
	core->memory = (void *(*)(unsigned)) dlsym(core->library, "retro_get_memory_data");
	core->memory_size = (size_t (*)(unsigned)) dlsym(core->library, "retro_get_memory_size");
	set_environment(environment);
	((void (*)(retro_video_refresh_t)) dlsym(core->library, "retro_set_video_refresh"))(video);
	((void (*)(retro_audio_sample_t)) dlsym(core->library, "retro_set_audio_sample"))(audio);
	((void (*)(retro_audio_sample_batch_t)) dlsym(core->library, "retro_set_audio_sample_batch"))(audio_batch);
	((void (*)(retro_input_poll_t)) dlsym(core->library, "retro_set_input_poll"))(input_poll);
	((void (*)(retro_input_state_t)) dlsym(core->library, "retro_set_input_state"))(input_state);
	init();
	if (!load_game(game)) return false;
	core->history = malloc((size_t) frames * RAM_SIZE);
	return core->history && core->memory_size(RETRO_MEMORY_SYSTEM_RAM) >= RAM_SIZE;
}

static void record(struct core *core, unsigned frames) {
	unsigned frame;

	input_frame = 0;
	for (frame = 0; frame < frames; frame++) {
		core->run();
		memcpy(core->history + (size_t) frame * RAM_SIZE, core->memory(RETRO_MEMORY_SYSTEM_RAM), RAM_SIZE);
		input_frame++;
	}
}

static unsigned differences(const struct core *ours, const struct core *theirs, unsigned a, unsigned b) {
	const uint8_t *left = ours->history + (size_t) a * RAM_SIZE;
	const uint8_t *right = theirs->history + (size_t) b * RAM_SIZE;
	unsigned count = 0;
	unsigned index;

	for (index = 0; index < RAM_SIZE; index++) {
		if (left[index] != right[theirs->swapped ? index ^ 1 : index]) count++;
	}
	return count;
}

int main(int argc, char **argv) {
	struct core ours;
	struct core theirs;
	struct retro_game_info game;
	FILE *file;
	long size;
	uint8_t *data;
	unsigned frames;
	unsigned frame;
	int offset = 0;
	bool fixed = argc > 5;
	unsigned shown = 0;
	static uint8_t known[RAM_SIZE];

	if (argc < 5) {
		fprintf(stderr, "usage: %s CORE REFERENCE ROM FRAMES [OFFSET]\n", argv[0]);
		return 2;
	}
	frames = (unsigned) atoi(argv[4]);
	if (fixed) offset = atoi(argv[5]);
	file = fopen(argv[3], "rb");
	if (!file || fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET)) return 1;
	data = malloc((size_t) size);
	if (!data || fread(data, 1, (size_t) size, file) != (size_t) size) return 1;
	fclose(file);
	memset(&game, 0, sizeof(game));
	game.path = argv[3];
	game.data = data;
	game.size = (size_t) size;
	memset(&ours, 0, sizeof(ours));
	memset(&theirs, 0, sizeof(theirs));
	if (!load(&ours, argv[1], &game, frames) || !load(&theirs, argv[2], &game, frames)) {
		fprintf(stderr, "could not load a core\n");
		return 1;
	}
	record(&ours, frames);
	record(&theirs, frames);
	theirs.swapped = true;
	if (differences(&ours, &theirs, frames / 4, frames / 4) > 32768) theirs.swapped = false;
	for (frame = 8; frame < frames - 8; frame++) {
		unsigned count = differences(&ours, &theirs, frame, (unsigned) ((int) frame + offset));
		int best = offset;
		int trial;

		if (count && !fixed) {
			for (trial = -8; trial <= 8; trial++) {
				unsigned other = differences(&ours, &theirs, frame, (unsigned) ((int) frame + trial));
				if (other < count) {
					count = other;
					best = trial;
				}
			}
		}
		if (best != offset) {
			printf("frame %u: reference offset %+d -> %+d\n", frame, offset, best);
			offset = best;
		}
		if (count) {
			const uint8_t *left = ours.history + (size_t) frame * RAM_SIZE;
			const uint8_t *right = theirs.history + (size_t) ((int) frame + offset) * RAM_SIZE;
			unsigned index;
			unsigned listed = 0;
			unsigned fresh = 0;
			for (index = 0; index < RAM_SIZE; index++) {
				uint8_t other = right[theirs.swapped ? index ^ 1 : index];
				if (left[index] == other || known[index]) continue;
				known[index] = 1;
				if (!fresh++) printf("frame %u: %u differ, new:", frame, count);
				if (listed++ < 14) printf(" %04x=%02x/%02x", index, left[index], other);
			}
			if (fresh) {
				printf(fresh > 14 ? " (+%u more)\n" : "\n", fresh - 14);
				if (++shown >= 40) break;
			}
		}
	}
	if (!shown) printf("no differences over %u frames at offset %+d\n", frames, offset);
	return 0;
}
