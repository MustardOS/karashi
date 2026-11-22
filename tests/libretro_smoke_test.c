#define _POSIX_C_SOURCE 200809L

#include "libretro.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned video_frames;
static size_t audio_frames;
static unsigned input_polls;
static unsigned keyboard_polls;
static unsigned expected_width = 256;
static unsigned expected_height = 192;
static bool described;
static bool controllers_described;
static bool options_described;
static uint64_t video_hash;
static uint64_t audio_hash;
static uint32_t input_ids[4];
static char system_directory[256];
static const char *region_option = "Auto";
static bool option_updated;
static unsigned av_updates;
static double updated_fps;

static const uint8_t z80_handoff[] = {
	0x21, 0x00, 0xc0, 0x36, 0xd3, 0x23, 0x36, 0x3e, 0x23, 0x36, 0xc3, 0x23, 0x36, 0x00, 0x23, 0x36, 0x00,
	0x3e, 0xab, 0xc3, 0x00, 0xc0
};

static const uint8_t tmss_handoff[] = {
	0x41, 0xf9, 0x00, 0x00, 0x02, 0x20, 0x43, 0xf9, 0x00, 0xff, 0x00, 0x00, 0x70, 0x08, 0x32, 0xd8,
	0x51, 0xc8, 0xff, 0xfc, 0x4e, 0xf9, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x13, 0xfc, 0x00, 0x01, 0x00, 0xa1, 0x41, 0x01, 0x2e, 0x78, 0x00, 0x00, 0x20, 0x78, 0x00, 0x04,
	0x4e, 0xd0
};

static void write_bios(const char *name, size_t size, size_t code_at, const uint8_t *code, size_t code_size) {
	char path[512];
	uint8_t *image = calloc(1, size);
	FILE *file;

	assert(image);
	memcpy(image + code_at, code, code_size);
	if (code == tmss_handoff) {
		image[1] = 0xff;
		image[2] = 0xff;
		image[6] = 0x02;
	}
	snprintf(path, sizeof(path), "%s/%s", system_directory, name);
	file = fopen(path, "wb");
	assert(file && fwrite(image, 1, size, file) == size);
	fclose(file);
	free(image);
}

static void remove_bios(const char *name) {
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", system_directory, name);
	unlink(path);
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size) {
	const uint8_t *bytes = data;
	if (!hash) hash = UINT64_C(1469598103934665603);
	while (size--) {
		hash ^= *bytes++;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static bool environment(unsigned command, void *data) {
	if (command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY) {
		*(const char **) data = system_directory;
		return system_directory[0] != 0;
	}
	if (command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) {
		assert(*(enum retro_pixel_format *) data == RETRO_PIXEL_FORMAT_RGB565);
		return true;
	}
	if (command == RETRO_ENVIRONMENT_SET_VARIABLES) {
		const struct retro_variable *variables = data;
		assert(variables && variables[0].key && strcmp(variables[0].key, "karashi_region") == 0);
		options_described = true;
		return true;
	}
	if (command == RETRO_ENVIRONMENT_GET_VARIABLE) {
		struct retro_variable *variable = data;
		if (variable && variable->key && strcmp(variable->key, "karashi_region") == 0) {
			variable->value = region_option;
			return true;
		}
		return false;
	}
	if (command == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE) {
		*(bool *) data = option_updated;
		option_updated = false;
		return true;
	}
	if (command == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO) {
		const struct retro_system_av_info *info = data;
		assert(info);
		updated_fps = info->timing.fps;
		av_updates++;
		return true;
	}
	if (command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS) {
		described = true;
		return true;
	}
	if (command == RETRO_ENVIRONMENT_SET_CONTROLLER_INFO) {
		const struct retro_controller_info *ports = data;
		assert(ports && ports[0].types && ports[0].num_types == 4);
		assert(ports[0].types[0].id == RETRO_DEVICE_JOYPAD);
		assert((ports[0].types[1].id & RETRO_DEVICE_MASK) == RETRO_DEVICE_LIGHTGUN);
		assert(ports[1].types && ports[1].num_types == 4);
		assert(ports[2].types && ports[2].num_types == 1);
		assert(ports[3].types && ports[3].num_types == 1);
		controllers_described = true;
		return true;
	}
	return false;
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch) {
	assert(data);
	assert(width == expected_width);
	assert(height == expected_height);
	assert(pitch >= width * sizeof(uint16_t) && pitch <= 320 * sizeof(uint16_t));
	video_hash = hash_bytes(video_hash, data, pitch * height);
	video_frames++;
}

static void audio(int16_t left, int16_t right) {
	(void) left;
	(void) right;
}

static size_t audio_batch(const int16_t *data, size_t frames) {
	assert(data);
	audio_hash = hash_bytes(audio_hash, data, frames * 2 * sizeof(*data));
	audio_frames += frames;
	return frames;
}

static void input_poll(void) {
	input_polls++;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (device == RETRO_DEVICE_KEYBOARD) {
		keyboard_polls++;
		return id == RETROK_1;
	}
	assert(device == RETRO_DEVICE_JOYPAD);
	assert(index == 0);
	if (port < 4 && id < 32) input_ids[port] |= UINT32_C(1) << id;
	return port == 0 && id == RETRO_DEVICE_ID_JOYPAD_B;
}

static void load_symbol(void *library, const char *name, void *destination, size_t size) {
	void *symbol = dlsym(library, name);
	const char *error = dlerror();

	if (error || !symbol) {
		fprintf(stderr, "Could not resolve %s: %s\n", name, error ? error : "missing symbol");
		exit(1);
	}
	assert(size == sizeof(symbol));
	memcpy(destination, &symbol, size);
}

int main(int argc, char **argv) {
	void *library;
	unsigned (*api_version)(void);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*init)(void);
	void (*deinit)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	unsigned (*get_region)(void);
	void (*set_controller_port_device)(unsigned, unsigned);
	bool (*load_game)(const struct retro_game_info *);
	void (*run)(void);
	void (*unload_game)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	void (*cheat_reset)(void);
	void (*cheat_set)(unsigned, bool, const char *);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
	static uint8_t cartridge[0x8000];
	uint8_t *state;
	uint8_t *ram;
	uint64_t expected_video_hash;
	uint64_t expected_audio_hash;
	size_t state_size;
	size_t audio_before;
	struct retro_system_info info;
	struct retro_system_av_info av_info;
	struct retro_game_info game;
	uint8_t diagnostic[] = {0x00};
	uint8_t sc3000_diagnostic[] = {
		0x3e, 0x92, 0xd3, 0xdf, 0x3e, 0x00, 0xd3, 0xde, 0xdb, 0xdc, 0x32, 0x00, 0xc0, 0x76, 0x18, 0xfd
	};

	if (argc != 2) {
		fprintf(stderr, "Usage: %s CORE\n", argv[0]);
		return 2;
	}
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!library) {
		fprintf(stderr, "Could not load core: %s\n", dlerror());
		return 1;
	}

	load_symbol(library, "retro_api_version", &api_version, sizeof(api_version));
	load_symbol(library, "retro_set_environment", &set_environment, sizeof(set_environment));
	load_symbol(library, "retro_set_video_refresh", &set_video_refresh, sizeof(set_video_refresh));
	load_symbol(library, "retro_set_audio_sample", &set_audio_sample, sizeof(set_audio_sample));
	load_symbol(library, "retro_set_audio_sample_batch", &set_audio_sample_batch, sizeof(set_audio_sample_batch));
	load_symbol(library, "retro_set_input_poll", &set_input_poll, sizeof(set_input_poll));
	load_symbol(library, "retro_set_input_state", &set_input_state, sizeof(set_input_state));
	load_symbol(library, "retro_init", &init, sizeof(init));
	load_symbol(library, "retro_deinit", &deinit, sizeof(deinit));
	load_symbol(library, "retro_get_system_info", &get_system_info, sizeof(get_system_info));
	load_symbol(library, "retro_get_system_av_info", &get_system_av_info, sizeof(get_system_av_info));
	load_symbol(library, "retro_get_region", &get_region, sizeof(get_region));
	load_symbol(
		library, "retro_set_controller_port_device", &set_controller_port_device, sizeof(set_controller_port_device)
	);
	load_symbol(library, "retro_load_game", &load_game, sizeof(load_game));
	load_symbol(library, "retro_run", &run, sizeof(run));
	load_symbol(library, "retro_unload_game", &unload_game, sizeof(unload_game));
	load_symbol(library, "retro_serialize_size", &serialize_size, sizeof(serialize_size));
	load_symbol(library, "retro_serialize", &serialize, sizeof(serialize));
	load_symbol(library, "retro_unserialize", &unserialize, sizeof(unserialize));
	load_symbol(library, "retro_cheat_reset", &cheat_reset, sizeof(cheat_reset));
	load_symbol(library, "retro_cheat_set", &cheat_set, sizeof(cheat_set));
	load_symbol(library, "retro_get_memory_data", &get_memory_data, sizeof(get_memory_data));
	load_symbol(library, "retro_get_memory_size", &get_memory_size, sizeof(get_memory_size));

	set_environment(environment);
	set_video_refresh(video);
	set_audio_sample(audio);
	set_audio_sample_batch(audio_batch);
	set_input_poll(input_poll);
	set_input_state(input_state);
	init();
	assert(api_version() == RETRO_API_VERSION);
	assert(options_described);
	assert(controllers_described);
	memset(&info, 0, sizeof(info));
	get_system_info(&info);
	assert(info.library_name && strcmp(info.library_name, "Karashi") == 0);
	assert(info.library_version && strcmp(info.library_version, "1.0.9") == 0);
	assert(info.valid_extensions && strstr(info.valid_extensions, "sg"));

	memset(&game, 0, sizeof(game));
	game.path = "karashi-diagnostic.sg";
	game.data = diagnostic;
	game.size = sizeof(diagnostic);
	assert(load_game(&game));
	set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	set_controller_port_device(1, RETRO_DEVICE_NONE);
	memset(&av_info, 0, sizeof(av_info));
	get_system_av_info(&av_info);
	assert(av_info.geometry.base_width == 256);
	assert(av_info.geometry.base_height == 192);
	assert(av_info.timing.fps > 59.9 && av_info.timing.fps < 59.95);
	run();
	assert(input_ids[0] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_B));
	assert(input_ids[0] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_A));
	assert(input_ids[0] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_START));
	assert(input_ids[1] == 0);
	set_controller_port_device(1, RETRO_DEVICE_JOYPAD);
	run();
	assert(input_ids[1] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_B));
	assert(input_ids[1] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_A));
	assert(video_frames == 2);
	assert(audio_frames >= 1601 && audio_frames <= 1603);
	assert(input_polls == 2);
	assert(described);
	assert(get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 1024);
	assert(get_memory_size(RETRO_MEMORY_SAVE_RAM) == 8192);
	ram = get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	assert(ram);
	cheat_set(0, true, "C010:5A");
	run();
	assert(ram[0x10] == 0x5a);
	cheat_set(0, false, NULL);
	ram[0x10] = 0;
	run();
	assert(ram[0x10] == 0);
	ram[0x11] = 0;
	cheat_set(1, true, "C011:66:00");
	run();
	assert(ram[0x11] == 0x66);
	cheat_set(1, true, "C011:77:00");
	run();
	assert(ram[0x11] == 0x66);
	cheat_reset();
	state_size = serialize_size();
	assert(state_size > 16384);
	state = malloc(state_size);
	assert(state && serialize(state, state_size));
	video_hash = 0;
	audio_hash = 0;
	run();
	run();
	expected_video_hash = video_hash;
	expected_audio_hash = audio_hash;
	assert(unserialize(state, state_size));
	video_hash = 0;
	audio_hash = 0;
	run();
	run();
	assert(video_hash == expected_video_hash);
	assert(audio_hash == expected_audio_hash);
	state[4]--;
	assert(!unserialize(state, state_size));
	state[4]++;
	state[state_size - 1] ^= 0x01;
	assert(!unserialize(state, state_size));
	free(state);
	unload_game();
	assert(serialize_size() == 0);
	game.path = "karashi-diagnostic.sc";
	game.data = sc3000_diagnostic;
	game.size = sizeof(sc3000_diagnostic);
	assert(load_game(&game));
	assert(get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 2048);
	run();
	ram = get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	assert(ram && ram[0] == 0xfe);
	assert(keyboard_polls > 0);
	unload_game();

	game.path = "karashi-diagnostic.sms";
	game.data = cartridge;
	game.size = sizeof(cartridge);
	snprintf(system_directory, sizeof(system_directory), "%s/karashi-bios-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	assert(mkdtemp(system_directory));
	assert(!load_game(&game));
	write_bios("bios_U.sms", 0x2000, 0, z80_handoff, sizeof(z80_handoff));
	write_bios("bios_E.sms", 0x2000, 0, z80_handoff, sizeof(z80_handoff));
	write_bios("bios.gg", 0x400, 0, z80_handoff, sizeof(z80_handoff));
	write_bios("bios_MD.bin", 0x800, 0x200, tmss_handoff, sizeof(tmss_handoff));
	assert(load_game(&game));
	get_system_av_info(&av_info);
	assert(av_info.geometry.base_width == 256);
	assert(av_info.timing.fps > 59.9 && av_info.timing.fps < 59.95);
	assert(get_region() == RETRO_REGION_NTSC);
	assert(get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 8192);
	assert(get_memory_size(RETRO_MEMORY_SAVE_RAM) == 32768);
	run();
	ram = get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	assert(ram);
	state_size = serialize_size();
	assert(state_size > 16384 + 8192 + 32768);
	state = malloc(state_size);
	assert(state && serialize(state, state_size));
	ram[0x100] = 0x5a;
	assert(unserialize(state, state_size));
	assert(ram[0x100] == 0x00);
	free(state);
	unload_game();
	game.path = "karashi-diagnostic (Europe).sms";
	assert(load_game(&game));
	get_system_av_info(&av_info);
	assert(av_info.timing.fps > 49.69 && av_info.timing.fps < 49.71);
	assert(get_region() == RETRO_REGION_PAL);
	region_option = "Export NTSC";
	option_updated = true;
	audio_before = audio_frames;
	run();
	assert(av_updates == 1 && updated_fps > 59.9 && updated_fps < 59.95);
	assert(get_region() == RETRO_REGION_NTSC && audio_frames - audio_before >= 800 && audio_frames - audio_before <= 802);
	region_option = "Auto";
	option_updated = true;
	audio_before = audio_frames;
	run();
	assert(av_updates == 2 && updated_fps > 49.69 && updated_fps < 49.71);
	assert(get_region() == RETRO_REGION_PAL && audio_frames - audio_before >= 965 && audio_frames - audio_before <= 967);
	unload_game();

	game.path = "karashi-diagnostic.gg";
	assert(load_game(&game));
	get_system_av_info(&av_info);
	assert(av_info.geometry.base_width == 160);
	assert(av_info.geometry.base_height == 144);
	expected_width = 160;
	expected_height = 144;
	run();
	assert(video_frames == 15);
	assert(serialize_size() == state_size);
	unload_game();

	memset(cartridge, 0, sizeof(cartridge));
	cartridge[2] = 0xff;
	cartridge[3] = 0x00;
	cartridge[6] = 0x01;
	cartridge[7] = 0x00;
	cartridge[0x100] = 0x4e;
	cartridge[0x101] = 0x71;
	cartridge[0x102] = 0x60;
	cartridge[0x103] = 0xfe;
	game.path = "karashi-diagnostic.md";
	assert(load_game(&game));
	set_controller_port_device(2, RETRO_DEVICE_JOYPAD);
	set_controller_port_device(3, RETRO_DEVICE_JOYPAD);
	get_system_av_info(&av_info);
	assert(av_info.geometry.base_width == 320);
	assert(av_info.geometry.aspect_ratio > 1.306f && av_info.geometry.aspect_ratio < 1.307f);
	assert(av_info.geometry.base_height == 224);
	assert(av_info.timing.fps > 59.9 && av_info.timing.fps < 60.1);
	expected_width = 256;
	expected_height = 224;
	run();
	assert(video_frames == 16);
	assert(input_ids[2] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_B));
	assert(input_ids[3] & (UINT32_C(1) << RETRO_DEVICE_ID_JOYPAD_B));
	assert(get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 65536);
	assert(get_memory_size(RETRO_MEMORY_SAVE_RAM) == 0);
	ram = get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
	assert(ram);
	cheat_set(0, true, "FF0100:1234");
	run();
	assert(ram[0x100] == 0x12 && ram[0x101] == 0x34);
	cheat_reset();
	state_size = serialize_size();
	assert(state_size > 128 * 1024);
	state = malloc(state_size);
	assert(state && serialize(state, state_size));
	video_hash = 0;
	audio_hash = 0;
	run();
	run();
	expected_video_hash = video_hash;
	expected_audio_hash = audio_hash;
	assert(unserialize(state, state_size));
	video_hash = 0;
	audio_hash = 0;
	run();
	run();
	assert(video_hash == expected_video_hash);
	assert(audio_hash == expected_audio_hash);
	assert(video_frames == 21);
	free(state);
	unload_game();
	deinit();
	dlclose(library);
	remove_bios("bios_U.sms");
	remove_bios("bios_E.sms");
	remove_bios("bios.gg");
	remove_bios("bios_MD.bin");
	rmdir(system_directory);
	puts("libretro lifecycle, required BIOS and frame delivery: pass");
	return 0;
}
