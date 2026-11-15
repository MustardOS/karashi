#include "libretro.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned frame_number;
static unsigned long checksum;
static bool environment(unsigned command, void *data) {
	if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY && getenv("KARASHI_SYSTEM_DIR")) {
		*(const char **) data = getenv("KARASHI_SYSTEM_DIR");
		return true;
	}
	return command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT;
}
static void video(const void *data, unsigned width, unsigned height, size_t pitch) { (void) width; (void) height; (void) pitch; checksum += *(const unsigned char *) data; }
static void audio(int16_t l, int16_t r) { (void) l; (void) r; }
static size_t audio_batch(const int16_t *data, size_t frames) { (void) data; return frames; }
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	unsigned phase = frame_number % 120;
	(void) index;
	if (port || device != RETRO_DEVICE_JOYPAD || frame_number < 150) return 0;
	if (id == RETRO_DEVICE_ID_JOYPAD_START) return phase < 6;
	if (id == RETRO_DEVICE_ID_JOYPAD_B) return phase >= 40 && phase < 46;
	if (id == RETRO_DEVICE_ID_JOYPAD_A) return phase >= 80 && phase < 86;
	return 0;
}
typedef void (*set_environment_t)(retro_environment_t);
typedef void (*set_video_t)(retro_video_refresh_t);
typedef void (*set_audio_t)(retro_audio_sample_t);
typedef void (*set_batch_t)(retro_audio_sample_batch_t);
typedef void (*set_poll_t)(retro_input_poll_t);
typedef void (*set_state_t)(retro_input_state_t);
typedef void (*void_t)(void);
typedef bool (*load_t)(const struct retro_game_info *);
#define SYM(type, name) type name = (type) dlsym(library, "retro_" #name)
int main(int argc, char **argv) {
	void *library = dlopen(argv[1], RTLD_NOW);
	struct retro_game_info game = {0};
	struct timespec a, b;
	FILE *file; long size; uint8_t *data; unsigned frames = (unsigned) atoi(argv[3]);
	if (argc < 4 || !library) return 2;
	{
		SYM(set_environment_t, set_environment); SYM(set_video_t, set_video_refresh); SYM(set_audio_t, set_audio_sample);
		SYM(set_batch_t, set_audio_sample_batch); SYM(set_poll_t, set_input_poll); SYM(set_state_t, set_input_state);
		SYM(void_t, init); SYM(load_t, load_game); SYM(void_t, run);
		file = fopen(argv[2], "rb"); if (!file) return 3;
		fseek(file, 0, SEEK_END); size = ftell(file); rewind(file);
		data = malloc((size_t) size); if (fread(data, 1, (size_t) size, file) != (size_t) size) return 3;
		set_environment(environment); set_video_refresh(video); set_audio_sample(audio);
		set_audio_sample_batch(audio_batch); set_input_poll(input_poll); set_input_state(input_state);
		init();
		game.path = argv[2]; game.data = data; game.size = (size_t) size;
		if (!load_game(&game)) return 4;
		clock_gettime(CLOCK_MONOTONIC, &a);
		for (frame_number = 0; frame_number < frames; frame_number++) run();
		clock_gettime(CLOCK_MONOTONIC, &b);
	}
	printf("%.3f ms per frame\n", ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / frames);
	return 0;
}
