#include "libretro.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t frame[1024 * 512];
static unsigned frame_width, frame_height, frame_number;
static enum retro_pixel_format format = RETRO_PIXEL_FORMAT_0RGB1555;
static FILE *audio_file;

static bool environment(unsigned command, void *data) {
	if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) { format = *(enum retro_pixel_format *) data; return true; }
	if (command == RETRO_ENVIRONMENT_GET_CAN_DUPE) { *(bool *) data = true; return true; }
	if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY && getenv("KARASHI_SYSTEM_DIR")) { *(const char **) data = getenv("KARASHI_SYSTEM_DIR"); return true; }
	if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
		struct retro_variable *variable = data;
		if (variable->key && strcmp(variable->key, "karashi_region") == 0 && getenv("KARASHI_REGION")) { variable->value = getenv("KARASHI_REGION"); return true; }
		if (variable->key && strcmp(variable->key, "genesis_plus_gx_bios") == 0 && getenv("KARASHI_SYSTEM_DIR")) {
			variable->value = getenv("KARASHI_REFERENCE_BIOS") ? getenv("KARASHI_REFERENCE_BIOS") : "enabled";
			return true;
		}
		return false;
	}
	return false;
}
static void video(const void *data, unsigned width, unsigned height, size_t pitch) {
	unsigned x, y;
	if (!data) return;
	if (!width || !height || width > (sizeof frame / sizeof frame[0]) / height) {
		fputs("capture: frame dimensions exceed buffer capacity\n", stderr);
		exit(EXIT_FAILURE);
	}
	frame_width = width; frame_height = height;
	for (y = 0; y < height; y++) {
		const uint8_t *row = (const uint8_t *) data + y * pitch;
		for (x = 0; x < width; x++) {
			if (format == RETRO_PIXEL_FORMAT_XRGB8888) frame[y * width + x] = ((const uint32_t *) (const void *) row)[x] & 0xf8fcf8;
			else {
				unsigned p = ((const uint16_t *) (const void *) row)[x];
				unsigned r, g, b;
				if (format == RETRO_PIXEL_FORMAT_RGB565) { r = (p >> 11) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3; }
				else { r = ((p >> 10) & 31) << 3; g = ((p >> 5) & 31) << 3; b = (p & 31) << 3; }
				frame[y * width + x] = r << 16 | g << 8 | b;
			}
		}
	}
}
static void audio(int16_t l, int16_t r) { int16_t s[2] = {l, r}; if (audio_file) fwrite(s, 2, 2, audio_file); }
static size_t audio_batch(const int16_t *data, size_t frames) { if (audio_file) fwrite(data, 4, frames, audio_file); return frames; }
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

/* cap CORE ROM OUT.raw AUDIO.raw|- FRAME... : writes width,height,pixels per captured frame */
int main(int argc, char **argv) {
	void *library = dlopen(argv[1], RTLD_NOW);
	struct retro_game_info game = {0};
	FILE *file, *out;
	long size;
	uint8_t *data;
	int next = 5;
	if (argc < 6 || !library) { fprintf(stderr, "%s\n", dlerror()); return 2; }
	{
		SYM(set_environment_t, set_environment); SYM(set_video_t, set_video_refresh); SYM(set_audio_t, set_audio_sample);
		SYM(set_batch_t, set_audio_sample_batch); SYM(set_poll_t, set_input_poll); SYM(set_state_t, set_input_state);
		SYM(void_t, init); SYM(load_t, load_game); SYM(void_t, run);
		file = fopen(argv[2], "rb");
		if (!file) return 3;
		fseek(file, 0, SEEK_END); size = ftell(file); rewind(file);
		data = malloc((size_t) size);
		if (fread(data, 1, (size_t) size, file) != (size_t) size) return 3;
		fclose(file);
		out = fopen(argv[3], "wb");
		if (strcmp(argv[4], "-")) audio_file = fopen(argv[4], "wb");
		set_environment(environment); set_video_refresh(video); set_audio_sample(audio);
		set_audio_sample_batch(audio_batch); set_input_poll(input_poll); set_input_state(input_state);
		init();
		game.path = argv[2]; game.data = data; game.size = (size_t) size;
		if (!load_game(&game)) { puts("load failed"); return 4; }
		for (frame_number = 0; next < argc; frame_number++) {
			run();
			if (frame_number + 1 == (unsigned) atoi(argv[next])) {
				uint32_t header[2] = {frame_width, frame_height};
				fwrite(header, 4, 2, out);
				fwrite(frame, sizeof frame[0], (size_t) frame_width * frame_height, out);
				next++;
			}
		}
		fclose(out);
		if (audio_file) fclose(audio_file);
	}
	return 0;
}
