#include "content.h"
#include "libretro.h"
#include "md.h"
#include "sg1000.h"
#include "sms.h"
#include "state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KARASHI_WIDTH KARASHI_TMS9918_WIDTH
#define KARASHI_HEIGHT KARASHI_TMS9918_HEIGHT
#define KARASHI_MAX_HEIGHT KARASHI_SMSVDP_MAX_HEIGHT
#define KARASHI_SAMPLE_RATE KARASHI_SEGA8_SAMPLE_RATE
#define KARASHI_AUDIO_FRAMES 800
#define KARASHI_STATE_VERSION 19
#define KARASHI_STATE_CAPACITY (1536 * 1024)
#define KARASHI_COPIER_HEADER 512
#define KARASHI_MARS_ROM_CAPACITY 0x400000
#define KARASHI_CHEAT_SLOTS 64

struct karashi_cheat {
	uint32_t address;
	uint16_t value;
	uint16_t compare;
	uint8_t width;
	bool enabled;
	bool has_compare;
};

static retro_environment_t environment_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static karashi_pixel video_frame[KARASHI_WIDTH * KARASHI_HEIGHT];
static int16_t audio_frame[KARASHI_AUDIO_FRAMES * 2];
static uint8_t *content_data;
static const uint8_t *content_rom;
static size_t content_size;
static enum karashi_system content_system;
static struct karashi_sg1000 sg1000;
static struct karashi_sms sms;
static struct karashi_sg1000 sg1000_scratch;
static struct karashi_sms sms_scratch;
static struct karashi_md md;
static struct karashi_md md_scratch;
static size_t state_size;
static unsigned video_height = KARASHI_HEIGHT;
static unsigned video_width = KARASHI_MD_WIDTH;
static bool controller_enabled[KARASHI_MD_CONTROLLER_PORTS] = {true, true, true, true};
static enum karashi_sms_device sms_device[2];
static int paddle_position[2] = {128, 128};

#define KARASHI_DEVICE_PHASER RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0)
#define KARASHI_DEVICE_PADDLE RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)
#define KARASHI_DEVICE_SPORTS_PAD RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)
#define ANALOG_DEADZONE 4096
static struct karashi_cheat cheats[KARASHI_CHEAT_SLOTS];

enum {
	BIOS_MAIN = 0,
	BIOS_MARS_68K,
	BIOS_MARS_MASTER,
	BIOS_MARS_SLAVE,
	BIOS_SLOTS
};

static uint8_t *bios_data[BIOS_SLOTS];
static size_t bios_size[BIOS_SLOTS];
static retro_log_printf_t log_cb;

static bool sms_active(void);

static bool md_active(void) {
	return content_system == KARASHI_SYSTEM_MEGA_DRIVE || content_system == KARASHI_SYSTEM_32X;
}

static uint8_t *cheat_byte(uint32_t address) {
	if (content_system == KARASHI_SYSTEM_SG1000) {
		if (address < 0xc000 || address > 0xffff) return NULL;
		return sg1000.sc3000 ? &sg1000.work_ram[address & 0x07ff] : &sg1000.ram[address & 0x03ff];
	}
	if (sms_active()) return address >= 0xc000 && address <= 0xffff ? &sms.ram[address & 0x1fff] : NULL;
	if (!md_active() || address > 0xffffff) return NULL;
	if (address >= 0xe00000) return &md.work_ram[address & 0xffff];
	if (address >= 0xa00000 && address < 0xa02000) return &md.z80_ram[address & 0x1fff];
	if (md.sram_present && address >= md.sram_start && address <= md.sram_end) {
		uint32_t offset = address - md.sram_start;
		if (md.sram_mode == KARASHI_MD_SRAM_EVEN && (address & 1)) return NULL;
		if (md.sram_mode == KARASHI_MD_SRAM_ODD && !(address & 1)) return NULL;
		if (md.sram_mode != KARASHI_MD_SRAM_BOTH) offset >>= 1;
		return &md.sram[offset % md.sram_size];
	}
	return NULL;
}

static void apply_cheats(void) {
	unsigned index;

	for (index = 0; index < KARASHI_CHEAT_SLOTS; index++) {
		const struct karashi_cheat *cheat = &cheats[index];
		uint8_t *first;
		uint8_t *second;

		if (!cheat->enabled) continue;
		first = cheat_byte(cheat->address);
		second = cheat->width == 2 ? cheat_byte(cheat->address + 1) : NULL;
		if (!first || (cheat->width == 2 && !second)) continue;
		if (cheat->has_compare) {
			uint16_t current = cheat->width == 2 ? (uint16_t) ((uint16_t) *first << 8 | *second) : *first;
			if (current != cheat->compare) continue;
		}
		if (cheat->width == 2) {
			*first = (uint8_t) (cheat->value >> 8);
			*second = (uint8_t) cheat->value;
		} else *first = (uint8_t) cheat->value;
	}
}

static void free_bios(void) {
	unsigned index;

	for (index = 0; index < BIOS_SLOTS; index++) {
		free(bios_data[index]);
		bios_data[index] = NULL;
		bios_size[index] = 0;
	}
}

static bool read_system_file(unsigned slot, const char *name, size_t minimum, size_t maximum) {
	const char *directory = NULL;
	char path[4096];
	FILE *file;
	long length;

	if (!environment_cb || !environment_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &directory) || !directory) return false;
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	file = fopen(path, "rb");
	if (!file) return false;
	if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < (long) minimum || (size_t) length > maximum
	    || fseek(file, 0, SEEK_SET)) {
		fclose(file);
		return false;
	}
	free(bios_data[slot]);
	bios_data[slot] = malloc((size_t) length);
	if (!bios_data[slot] || fread(bios_data[slot], 1, (size_t) length, file) != (size_t) length) {
		fclose(file);
		free(bios_data[slot]);
		bios_data[slot] = NULL;
		return false;
	}
	fclose(file);
	bios_size[slot] = (size_t) length;
	return true;
}

static bool require_system_file(unsigned slot, const char *name, size_t minimum, size_t maximum) {
	if (read_system_file(slot, name, minimum, maximum)) return true;
	if (log_cb) log_cb(RETRO_LOG_ERROR, "[Karashi] Missing or invalid BIOS in the system folder: %s\n", name);
	return false;
}

static bool path_has_extension(const char *path, const char *extension) {
	const char *dot = path ? strrchr(path, '.') : NULL;
	size_t index;

	if (!dot) return false;
	for (index = 0; extension[index]; index++) {
		if (!dot[index + 1] || (dot[index + 1] | 0x20) != extension[index]) return false;
	}
	return !dot[index + 1];
}

static bool load_bios(const char *path, enum karashi_region region) {
	free_bios();
	switch (content_system) {
		case KARASHI_SYSTEM_32X:
			return require_system_file(BIOS_MAIN, "bios_MD.bin", 2048, 16384)
			       && require_system_file(BIOS_MARS_68K, "32X_G_BIOS.BIN", 256, 256)
			       && require_system_file(BIOS_MARS_MASTER, "32X_M_BIOS.BIN", 2048, 2048)
			       && require_system_file(BIOS_MARS_SLAVE, "32X_S_BIOS.BIN", 1024, 1024);
		case KARASHI_SYSTEM_MEGA_DRIVE: return require_system_file(BIOS_MAIN, "bios_MD.bin", 2048, 16384);
		case KARASHI_SYSTEM_GAME_GEAR: return require_system_file(BIOS_MAIN, "bios.gg", 1024, 1024);
		case KARASHI_SYSTEM_MASTER_SYSTEM: {
			const char *name = region == KARASHI_REGION_JAPAN ? "bios_J.sms"
			                   : region == KARASHI_REGION_EXPORT_PAL ? "bios_E.sms" : "bios_U.sms";
			if (path_has_extension(path, "gg")) return true;
			return read_system_file(BIOS_MAIN, name, 0x2000, 0x40000)
			       || require_system_file(BIOS_MAIN, "bios.sms", 0x2000, 0x40000);
		}
		default: return true;
	}
}

static enum karashi_region read_region(const char *path, const uint8_t *data, size_t size) {
	struct retro_variable variable = {"karashi_region", NULL};

	if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value) {
		if (strcmp(variable.value, "Export NTSC") == 0) return KARASHI_REGION_EXPORT_NTSC;
		if (strcmp(variable.value, "Export PAL") == 0) return KARASHI_REGION_EXPORT_PAL;
		if (strcmp(variable.value, "Japan NTSC") == 0) return KARASHI_REGION_JAPAN;
	}
	return karashi_content_region(path, data, size);
}

static enum karashi_md_region read_md_region(void) {
	struct retro_variable variable = {"karashi_region", NULL};

	if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value) {
		if (strcmp(variable.value, "Export NTSC") == 0) return KARASHI_MD_REGION_USA;
		if (strcmp(variable.value, "Export PAL") == 0) return KARASHI_MD_REGION_EUROPE;
		if (strcmp(variable.value, "Japan NTSC") == 0) return KARASHI_MD_REGION_JAPAN;
	}
	return KARASHI_MD_REGION_AUTO;
}

static enum karashi_md_pad read_md_pad_type(void) {
	struct retro_variable variable = {"karashi_md_pad", NULL};

	if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value
	    && strcmp(variable.value, "6 button") == 0) return KARASHI_MD_PAD_SIX;
	return KARASHI_MD_PAD_THREE;
}

static void apply_md_pads(void) {
	enum karashi_md_pad type = read_md_pad_type();
	unsigned port;

	for (port = 0; port < KARASHI_MD_CONTROLLER_PORTS; port++) {
		karashi_md_set_pad(&md, port, controller_enabled[port] ? type : KARASHI_MD_PAD_NONE);
	}
}

static void deinterleave(uint8_t *data, size_t size) {
	uint8_t block[0x4000];
	size_t offset;
	unsigned index;

	for (offset = 0; offset + sizeof(block) <= size; offset += sizeof(block)) {
		memcpy(block, data + offset, sizeof(block));
		for (index = 0; index < 0x2000; index++) {
			data[offset + index * 2 + 1] = block[index];
			data[offset + index * 2] = block[0x2000 + index];
		}
	}
}

static void describe_options(void) {
	static const struct retro_variable variables[] = {
		{"karashi_region", "Region; Auto|Export NTSC|Export PAL|Japan NTSC"},
		{"karashi_md_pad", "Mega Drive controller; 3 button|6 button"},
		{NULL, NULL}
	};

	if (environment_cb) environment_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *) variables);
}

static bool sms_active(void) {
	return content_system == KARASHI_SYSTEM_MASTER_SYSTEM || content_system == KARASHI_SYSTEM_GAME_GEAR;
}

static bool pressed(unsigned port, unsigned id) {
	return input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id) != 0;
}

static int analog_axis(unsigned port, unsigned axis) {
	int value = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, axis);

	if (value > -ANALOG_DEADZONE && value < ANALOG_DEADZONE) return 0;
	return value;
}

static int motion(unsigned port, unsigned axis, unsigned less, unsigned more, unsigned mouse, int scale) {
	int value = analog_axis(port, axis) * scale / 32768;

	if (pressed(port, less)) value -= scale / 2;
	if (pressed(port, more)) value += scale / 2;
	return value + input_state_cb(port, RETRO_DEVICE_MOUSE, 0, mouse);
}

static void read_phaser(unsigned port) {
	int x = -1;
	int y = -1;
	int screen_x = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
	int screen_y = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
	bool trigger = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) != 0;

	if (!input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN)) {
		x = (screen_x + 0x7fff) * KARASHI_WIDTH / 0xfffe;
		y = (screen_y + 0x7fff) * (int) karashi_sms_height(&sms) / 0xfffe;
	}
	karashi_sms_set_phaser(&sms, port, x, y, trigger);
}

static void read_sms_devices(void) {
	unsigned port;

	for (port = 0; port < 2; port++) {
		switch (sms_device[port]) {
			case KARASHI_SMS_DEVICE_PHASER: read_phaser(port); break;
			case KARASHI_SMS_DEVICE_PADDLE:
				paddle_position[port] += motion(
					port, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
					RETRO_DEVICE_ID_MOUSE_X, 8
				);
				if (paddle_position[port] < 0) paddle_position[port] = 0;
				if (paddle_position[port] > 255) paddle_position[port] = 255;
				karashi_sms_set_paddle(
					&sms, port, (uint8_t) paddle_position[port], pressed(port, RETRO_DEVICE_ID_JOYPAD_B)
				);
				break;
			case KARASHI_SMS_DEVICE_SPORTS_PAD:
				karashi_sms_set_sports_pad(
					&sms, port,
					motion(
						port, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
						RETRO_DEVICE_ID_MOUSE_X, 8
					),
					motion(
						port, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
						RETRO_DEVICE_ID_MOUSE_Y, 8
					),
					(uint8_t) ((pressed(port, RETRO_DEVICE_ID_JOYPAD_B) ? 1 : 0) | (pressed(port, RETRO_DEVICE_ID_JOYPAD_A) ? 2 : 0))
				);
				break;
			default: break;
		}
	}
}

static uint8_t read_pad(unsigned port) {
	uint8_t state = 0;
	if (port >= 2 || !controller_enabled[port]) return 0;

	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_UP)) state |= KARASHI_SMS_PAD_UP;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_DOWN)) state |= KARASHI_SMS_PAD_DOWN;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_LEFT)) state |= KARASHI_SMS_PAD_LEFT;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_RIGHT)) state |= KARASHI_SMS_PAD_RIGHT;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_B)) state |= KARASHI_SMS_PAD_ONE;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_A)) state |= KARASHI_SMS_PAD_TWO;
	return state;
}

static void read_sc3000_keyboard(void) {
	static const struct {
		unsigned key;
		uint8_t column;
		uint8_t row;
	} keys[] = {
		{RETROK_1, 0, 0}, {RETROK_2, 1, 0}, {RETROK_3, 2, 0}, {RETROK_4, 3, 0},
		{RETROK_5, 4, 0}, {RETROK_6, 5, 0}, {RETROK_7, 6, 0},
		{RETROK_q, 0, 1}, {RETROK_w, 1, 1}, {RETROK_e, 2, 1}, {RETROK_r, 3, 1},
		{RETROK_t, 4, 1}, {RETROK_y, 5, 1}, {RETROK_u, 6, 1},
		{RETROK_a, 0, 2}, {RETROK_s, 1, 2}, {RETROK_d, 2, 2}, {RETROK_f, 3, 2},
		{RETROK_g, 4, 2}, {RETROK_h, 5, 2}, {RETROK_j, 6, 2},
		{RETROK_z, 0, 3}, {RETROK_x, 1, 3}, {RETROK_c, 2, 3}, {RETROK_v, 3, 3},
		{RETROK_b, 4, 3}, {RETROK_n, 5, 3}, {RETROK_m, 6, 3},
		{RETROK_F1, 0, 4}, {RETROK_SPACE, 1, 4}, {RETROK_CLEAR, 2, 4}, {RETROK_F2, 2, 4},
		{RETROK_DELETE, 3, 4}, {RETROK_BACKSPACE, 3, 4},
		{RETROK_COMMA, 0, 5}, {RETROK_PERIOD, 1, 5}, {RETROK_SLASH, 2, 5},
		{RETROK_DOWN, 4, 5}, {RETROK_LEFT, 5, 5}, {RETROK_RIGHT, 6, 5},
		{RETROK_k, 0, 6}, {RETROK_l, 1, 6}, {RETROK_SEMICOLON, 2, 6}, {RETROK_COLON, 3, 6},
		{RETROK_RIGHTBRACKET, 4, 6}, {RETROK_RETURN, 5, 6}, {RETROK_UP, 6, 6},
		{RETROK_i, 0, 7}, {RETROK_o, 1, 7}, {RETROK_p, 2, 7}, {RETROK_AT, 3, 7},
		{RETROK_LEFTBRACKET, 4, 7},
		{RETROK_8, 0, 8}, {RETROK_9, 1, 8}, {RETROK_0, 2, 8}, {RETROK_MINUS, 3, 8},
		{RETROK_CARET, 4, 8}, {RETROK_BACKSLASH, 5, 8}, {RETROK_BREAK, 6, 8},
		{RETROK_F4, 6, 9}, {RETROK_LCTRL, 6, 10}, {RETROK_RCTRL, 6, 10},
		{RETROK_F3, 5, 11}, {RETROK_LSHIFT, 6, 11}, {RETROK_RSHIFT, 6, 11}
	};
	size_t index;

	karashi_sg1000_clear_keys(&sg1000);
	for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
		if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, keys[index].key)) {
			karashi_sg1000_set_key(&sg1000, keys[index].column, keys[index].row, true);
		}
	}
}

static uint16_t read_md_pad(unsigned port) {
	uint16_t state = 0;
	if (port >= KARASHI_MD_CONTROLLER_PORTS || !controller_enabled[port]) return 0;

	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_UP)) state |= KARASHI_MD_PAD_UP;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_DOWN)) state |= KARASHI_MD_PAD_DOWN;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_LEFT)) state |= KARASHI_MD_PAD_LEFT;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_RIGHT)) state |= KARASHI_MD_PAD_RIGHT;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_Y)) state |= KARASHI_MD_PAD_A;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_B)) state |= KARASHI_MD_PAD_B;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_A)) state |= KARASHI_MD_PAD_C;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_START)) state |= KARASHI_MD_PAD_START;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_L)) state |= KARASHI_MD_PAD_X;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_X)) state |= KARASHI_MD_PAD_Y;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_R)) state |= KARASHI_MD_PAD_Z;
	if (pressed(port, RETRO_DEVICE_ID_JOYPAD_SELECT)) state |= KARASHI_MD_PAD_MODE;
	return state;
}

static void describe_input(void) {
	static const struct retro_input_descriptor sega8_descriptors[] = {
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 1"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 2"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Pause or Start"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 1"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 2"},
		{0, 0, 0, 0, NULL}
	};
	static const struct retro_input_descriptor md_descriptors[] = {
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button A"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button C"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Button X"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button Y"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button Z"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Mode"},
		{0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button A"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button C"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Button X"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button Y"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button Z"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Mode"},
		{1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button A"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button C"},
		{2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button A"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button C"},
		{3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		{0, 0, 0, 0, NULL}
	};

	if (environment_cb) {
		environment_cb(
			RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
			(void *) (md_active() ? md_descriptors : sega8_descriptors)
		);
	}
}

static void describe_controllers(void) {
	static const struct retro_controller_description types[] = {
		{"SEGA Controller", RETRO_DEVICE_JOYPAD},
		{"Light Phaser", KARASHI_DEVICE_PHASER},
		{"Paddle Control", KARASHI_DEVICE_PADDLE},
		{"Sports Pad", KARASHI_DEVICE_SPORTS_PAD}
	};
	static const struct retro_controller_info ports[] = {
		{types, 4},
		{types, 4},
		{types, 1},
		{types, 1},
		{NULL, 0}
	};

	if (environment_cb) environment_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *) ports);
}

static bool save_state(void *data, size_t capacity, size_t *size) {
	struct karashi_state_writer writer;

	if (!karashi_state_writer_begin(&writer, data, capacity, (uint16_t) content_system, KARASHI_STATE_VERSION)) {
		return false;
	}
	if (content_system == KARASHI_SYSTEM_SG1000) karashi_sg1000_save(&sg1000, &writer);
	else if (sms_active()) karashi_sms_save(&sms, &writer);
	else if (md_active()) karashi_md_save(&md, &writer);
	else return false;
	return karashi_state_writer_finish(&writer, size);
}

static bool load_state(const void *data, size_t size) {
	struct karashi_state_reader reader;

	if (!karashi_state_reader_begin(&reader, data, size)) return false;
	if (reader.version != KARASHI_STATE_VERSION || reader.system != (uint16_t) content_system) return false;

	if (content_system == KARASHI_SYSTEM_SG1000) {
		sg1000_scratch = sg1000;
		if (!karashi_sg1000_load(&sg1000_scratch, &reader) || !karashi_state_reader_finished(&reader)) return false;
		sg1000 = sg1000_scratch;
		return true;
	}
	if (sms_active()) {
		sms_scratch = sms;
		if (!karashi_sms_load(&sms_scratch, &reader) || !karashi_state_reader_finished(&reader)) return false;
		sms = sms_scratch;
		return true;
	}
	if (md_active()) {
		md_scratch = md;
		if (!karashi_md_load(&md_scratch, &reader) || !karashi_state_reader_finished(&reader)) return false;
		md = md_scratch;
		karashi_md_rebind(&md);
		return true;
	}
	return false;
}

static void measure_state(void) {
	void *buffer = malloc(KARASHI_STATE_CAPACITY);

	state_size = 0;
	if (buffer && !save_state(buffer, KARASHI_STATE_CAPACITY, &state_size)) state_size = 0;
	free(buffer);
}

static void unload_content(void) {
	free(content_data);
	content_data = NULL;
	content_rom = NULL;
	content_size = 0;
	content_system = KARASHI_SYSTEM_UNKNOWN;
	state_size = 0;
	free_bios();
}

static void fallback_video(const void *data, unsigned width, unsigned height, size_t pitch) {
	(void) data;
	(void) width;
	(void) height;
	(void) pitch;
}

static void fallback_audio(int16_t left, int16_t right) {
	(void) left;
	(void) right;
}

static size_t fallback_audio_batch(const int16_t *data, size_t frames) {
	(void) data;
	return frames;
}

static void fallback_input_poll(void) {
}

static int16_t fallback_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) port;
	(void) device;
	(void) index;
	(void) id;
	return 0;
}

static void diagnostic_frame(void) {
	unsigned x;
	unsigned y;
	karashi_pixel accent = KARASHI_PIXEL(0xc6, 0xa1, 0x3b);

	switch (content_system) {
		case KARASHI_SYSTEM_SG1000: accent = KARASHI_PIXEL(0x56, 0xa0, 0xd3); break;
		case KARASHI_SYSTEM_MASTER_SYSTEM: accent = KARASHI_PIXEL(0xd9, 0x4a, 0x4a); break;
		case KARASHI_SYSTEM_GAME_GEAR: accent = KARASHI_PIXEL(0x48, 0xb8, 0x6a); break;
		case KARASHI_SYSTEM_MEGA_DRIVE: accent = KARASHI_PIXEL(0x8a, 0x69, 0xd4); break;
		default: break;
	}

	for (y = 0; y < KARASHI_HEIGHT; y++) {
		for (x = 0; x < KARASHI_WIDTH; x++) {
			const int stripe = ((x / 20) + (y / 20)) & 1;
			video_frame[y * KARASHI_WIDTH + x] = stripe ? accent : KARASHI_PIXEL(0x14, 0x14, 0x18);
		}
	}
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t callback) {
	struct retro_log_callback logging;

	environment_cb = callback;
	if (callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
	describe_options();
	describe_controllers();
}

void retro_set_video_refresh(retro_video_refresh_t callback) {
	video_cb = callback ? callback : fallback_video;
}

void retro_set_audio_sample(retro_audio_sample_t callback) {
	audio_cb = callback ? callback : fallback_audio;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) {
	audio_batch_cb = callback ? callback : fallback_audio_batch;
}

void retro_set_input_poll(retro_input_poll_t callback) {
	input_poll_cb = callback ? callback : fallback_input_poll;
}

void retro_set_input_state(retro_input_state_t callback) {
	input_state_cb = callback ? callback : fallback_input_state;
}

void retro_init(void) {
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;

	if (!video_cb) video_cb = fallback_video;
	if (!audio_cb) audio_cb = fallback_audio;
	if (!audio_batch_cb) audio_batch_cb = fallback_audio_batch;
	if (!input_poll_cb) input_poll_cb = fallback_input_poll;
	if (!input_state_cb) input_state_cb = fallback_input_state;
	controller_enabled[0] = true;
	controller_enabled[1] = true;
	controller_enabled[2] = true;
	controller_enabled[3] = true;
	sms_device[0] = KARASHI_SMS_DEVICE_PAD;
	sms_device[1] = KARASHI_SMS_DEVICE_PAD;
	if (environment_cb) environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
}

void retro_deinit(void) {
	unload_content();
}

void retro_get_system_info(struct retro_system_info *info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "Karashi";
	info->library_version = "1.0.8";
	info->valid_extensions = "bin|gen|md|smd|32x|sms|gg|sg|sg1000|sc";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
	bool game_gear = content_system == KARASHI_SYSTEM_GAME_GEAR;
	bool mega_drive = md_active();

	memset(info, 0, sizeof(*info));
	info->geometry.base_width = mega_drive ? video_width : game_gear ? KARASHI_GG_WIDTH : KARASHI_WIDTH;
	info->geometry.base_height = game_gear ? KARASHI_GG_HEIGHT : video_height;
	info->geometry.max_width = mega_drive ? KARASHI_MD_WIDTH : KARASHI_WIDTH;
	info->geometry.max_height = mega_drive ? KARASHI_MD_MAX_HEIGHT : KARASHI_MAX_HEIGHT;
	info->geometry.aspect_ratio = game_gear ? 10.0f / 9.0f : 4.0f / 3.0f;
	info->timing.fps = mega_drive ? karashi_md_frame_rate(&md)
	                   : sms_active() ? karashi_sms_frame_rate(&sms)
	                                  : (double) KARASHI_SEGA8_CPU_CLOCK / KARASHI_SEGA8_FRAME_CYCLES;
	info->timing.sample_rate = mega_drive ? KARASHI_MD_SAMPLE_RATE : KARASHI_SAMPLE_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (port < KARASHI_MD_CONTROLLER_PORTS) controller_enabled[port] = device == RETRO_DEVICE_JOYPAD;
	if (port < 2) {
		if (device == KARASHI_DEVICE_PHASER) sms_device[port] = KARASHI_SMS_DEVICE_PHASER;
		else if (device == KARASHI_DEVICE_PADDLE) sms_device[port] = KARASHI_SMS_DEVICE_PADDLE;
		else if (device == KARASHI_DEVICE_SPORTS_PAD) sms_device[port] = KARASHI_SMS_DEVICE_SPORTS_PAD;
		else sms_device[port] = KARASHI_SMS_DEVICE_PAD;
		if (sms_active()) karashi_sms_set_device(&sms, port, sms_device[port]);
	}
	if (md_active()) apply_md_pads();
}

void retro_reset(void) {
	if (content_system == KARASHI_SYSTEM_SG1000) karashi_sg1000_reset(&sg1000);
	else if (sms_active()) karashi_sms_reset(&sms);
	else if (md_active()) karashi_md_reset(&md);
}

void retro_run(void) {
	bool updated = false;

	apply_cheats();
	input_poll_cb();
	if (content_system == KARASHI_SYSTEM_SG1000) {
		karashi_sg1000_set_controller(&sg1000, 0, read_pad(0));
		karashi_sg1000_set_controller(&sg1000, 1, read_pad(1));
		if (sg1000.sc3000) read_sc3000_keyboard();
		karashi_sg1000_set_pause(&sg1000, pressed(0, RETRO_DEVICE_ID_JOYPAD_START));
		karashi_sg1000_run_frame(&sg1000);
		video_cb(sg1000.video, KARASHI_WIDTH, KARASHI_HEIGHT, KARASHI_WIDTH * sizeof(sg1000.video[0]));
		audio_batch_cb(sg1000.audio, sg1000.audio_frames);
		return;
	}
	if (sms_active()) {
		karashi_sms_set_controller(&sms, 0, read_pad(0));
		karashi_sms_set_controller(&sms, 1, read_pad(1));
		read_sms_devices();
		karashi_sms_set_pause(&sms, pressed(0, RETRO_DEVICE_ID_JOYPAD_START));
		karashi_sms_run_frame(&sms);
		if (content_system == KARASHI_SYSTEM_GAME_GEAR) {
			video_cb(
				sms.video + KARASHI_GG_TOP * KARASHI_WIDTH + KARASHI_GG_LEFT, KARASHI_GG_WIDTH, KARASHI_GG_HEIGHT,
				KARASHI_WIDTH * sizeof(sms.video[0])
			);
		} else {
			if (karashi_sms_height(&sms) != video_height) {
				struct retro_system_av_info info;
				video_height = karashi_sms_height(&sms);
				retro_get_system_av_info(&info);
				if (environment_cb) environment_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
			}
			video_cb(sms.video, KARASHI_WIDTH, video_height, KARASHI_WIDTH * sizeof(sms.video[0]));
		}
		audio_batch_cb(sms.audio, sms.audio_frames);
		return;
	}
	if (md_active()) {
		unsigned port;

		for (port = 0; port < KARASHI_MD_CONTROLLER_PORTS; port++) {
			karashi_md_set_controller(&md, port, read_md_pad(port));
		}
		if (environment_cb && environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) apply_md_pads();
		karashi_md_run_frame(&md);
		if (md.width != video_width || md.height != video_height) {
			struct retro_system_av_info info;
			video_width = md.width;
			video_height = md.height;
			retro_get_system_av_info(&info);
			if (environment_cb) environment_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
		}
		video_cb(md.video, md.width, md.height, KARASHI_MD_WIDTH * sizeof(md.video[0]));
		audio_batch_cb(md.audio, md.audio_frames);
		return;
	}
	(void) input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
	diagnostic_frame();
	video_cb(video_frame, KARASHI_WIDTH, KARASHI_HEIGHT, KARASHI_WIDTH * sizeof(video_frame[0]));
	audio_batch_cb(audio_frame, KARASHI_AUDIO_FRAMES);
}

size_t retro_serialize_size(void) {
	return state_size;
}

bool retro_serialize(void *data, size_t size) {
	size_t written;

	return state_size && size >= state_size && save_state(data, state_size, &written) && written == state_size;
}

bool retro_unserialize(const void *data, size_t size) {
	return state_size && size >= state_size && load_state(data, state_size);
}

void retro_cheat_reset(void) {
	memset(cheats, 0, sizeof(cheats));
}

void retro_cheat_set(unsigned index, bool enabled, const char *code) {
	struct karashi_cheat *cheat;
	char *end;
	unsigned long address;
	unsigned long value;
	unsigned long compare = 0;
	bool has_compare = false;

	if (index >= KARASHI_CHEAT_SLOTS) return;
	cheat = &cheats[index];
	memset(cheat, 0, sizeof(*cheat));
	if (!enabled || !code) return;
	address = strtoul(code, &end, 16);
	if (end == code || *end != ':') return;
	code = end + 1;
	value = strtoul(code, &end, 16);
	if (end == code || value > 0xffff) return;
	if (*end == ':') {
		code = end + 1;
		compare = strtoul(code, &end, 16);
		if (end == code || compare > 0xffff) return;
		has_compare = true;
	}
	while (*end == ' ' || *end == '\t') end++;
	if (*end || address > 0xffffff) return;
	cheat->address = (uint32_t) address;
	cheat->value = (uint16_t) value;
	cheat->compare = (uint16_t) compare;
	cheat->width = value > 0xff || compare > 0xff ? 2 : 1;
	cheat->enabled = true;
	cheat->has_compare = has_compare;
}

bool retro_load_game(const struct retro_game_info *game) {
	uint8_t *copy;
	size_t capacity;
	enum karashi_region region;

	if (!game || !game->data || game->size == 0) return false;
	retro_cheat_reset();
	capacity = game->size;
	if (karashi_content_detect(game->path, game->data, game->size) == KARASHI_SYSTEM_32X
	    && capacity < KARASHI_MARS_ROM_CAPACITY) capacity = KARASHI_MARS_ROM_CAPACITY;
	copy = malloc(capacity);
	if (!copy) return false;
	memcpy(copy, game->data, game->size);
	memset(copy + game->size, 0xff, capacity - game->size);

	content_system = karashi_content_detect(game->path, copy, game->size);
	region = read_region(game->path, copy, game->size);
	if (content_system == KARASHI_SYSTEM_UNKNOWN || !load_bios(game->path, region)) {
		free(copy);
		free_bios();
		content_system = KARASHI_SYSTEM_UNKNOWN;
		return false;
	}

	free(content_data);
	content_data = copy;
	content_rom = copy;
	content_size = game->size;
	if (sms_active() && content_size % 0x4000 == KARASHI_COPIER_HEADER) {
		content_rom += KARASHI_COPIER_HEADER;
		content_size -= KARASHI_COPIER_HEADER;
	}

	if (content_system == KARASHI_SYSTEM_SG1000) {
		karashi_sg1000_init(&sg1000, content_rom, content_size, karashi_content_is_sc3000(game->path));
	}
	else if (sms_active()) {
		region = read_region(game->path, content_rom, content_size);
		karashi_sms_init(
			&sms, content_rom, content_size, content_system == KARASHI_SYSTEM_GAME_GEAR,
			region == KARASHI_REGION_JAPAN, region == KARASHI_REGION_EXPORT_PAL
		);
		karashi_sms_set_bios(&sms, bios_data[BIOS_MAIN], bios_size[BIOS_MAIN]);
		karashi_sms_set_device(&sms, 0, sms_device[0]);
		karashi_sms_set_device(&sms, 1, sms_device[1]);
	} else if (md_active()) {
		if (content_size % 0x4000 == KARASHI_COPIER_HEADER && content_size > 0x280 && copy[8] == 0xaa && copy[9] == 0xbb) {
			content_rom += KARASHI_COPIER_HEADER;
			content_size -= KARASHI_COPIER_HEADER;
			deinterleave(copy + KARASHI_COPIER_HEADER, content_size);
		}
		karashi_md_init(&md, content_rom, content_size);
		if (content_system == KARASHI_SYSTEM_32X) {
			karashi_md_attach_mars(
				&md, bios_data[BIOS_MARS_68K], bios_data[BIOS_MARS_MASTER], bios_size[BIOS_MARS_MASTER],
				bios_data[BIOS_MARS_SLAVE], bios_size[BIOS_MARS_SLAVE]
			);
			karashi_mars_set_rom_capacity(&md.mars, KARASHI_MARS_ROM_CAPACITY);
		}
		karashi_md_set_tmss(&md, bios_data[BIOS_MAIN], bios_size[BIOS_MAIN]);
		karashi_md_set_region(&md, read_md_region());
		apply_md_pads();
	}
	video_height = md_active() ? md.height : KARASHI_HEIGHT;
	video_width = KARASHI_MD_WIDTH;
	describe_input();
	measure_state();
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num_info) {
	(void) type;
	(void) info;
	(void) num_info;
	return false;
}

void retro_unload_game(void) {
	unload_content();
}

unsigned retro_get_region(void) {
	if (md_active()) return md.pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
	return sms_active() && sms.pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id) {
	if (content_system == KARASHI_SYSTEM_SG1000) {
		if (id == RETRO_MEMORY_SAVE_RAM) return sg1000.cart_ram;
		return id == RETRO_MEMORY_SYSTEM_RAM ? sg1000.sc3000 ? sg1000.work_ram : sg1000.ram : NULL;
	}
	if (md_active()) {
		if (id == RETRO_MEMORY_SAVE_RAM) return md.eeprom_present || md.sram_persistent ? md.sram : NULL;
		return id == RETRO_MEMORY_SYSTEM_RAM ? md.work_ram : NULL;
	}
	if (!sms_active()) return NULL;
	if (id == RETRO_MEMORY_SAVE_RAM) return sms.cart_ram;
	return id == RETRO_MEMORY_SYSTEM_RAM ? sms.ram : NULL;
}

size_t retro_get_memory_size(unsigned id) {
	if (content_system == KARASHI_SYSTEM_SG1000) {
		if (id == RETRO_MEMORY_SAVE_RAM) return sizeof(sg1000.cart_ram);
		return id == RETRO_MEMORY_SYSTEM_RAM ? sg1000.sc3000 ? 2048 : sizeof(sg1000.ram) : 0;
	}
	if (md_active()) {
		if (id == RETRO_MEMORY_SAVE_RAM) {
			if (md.eeprom_present) return md.eeprom_size;
			return md.sram_persistent ? md.sram_size : 0;
		}
		return id == RETRO_MEMORY_SYSTEM_RAM ? sizeof(md.work_ram) : 0;
	}
	if (!sms_active()) return 0;
	if (id == RETRO_MEMORY_SAVE_RAM) return sizeof(sms.cart_ram);
	return id == RETRO_MEMORY_SYSTEM_RAM ? sizeof(sms.ram) : 0;
}
