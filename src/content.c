#include "content.h"
#include "state.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static int extension_is(const char *path, const char *extension) {
	const char *dot;

	if (!path || !extension) return 0;
	dot = strrchr(path, '.');
	if (!dot) return 0;
	dot++;

	while (*dot && *extension) {
		if (tolower((unsigned char) *dot) != tolower((unsigned char) *extension)) return 0;
		dot++;
		extension++;
	}

	return *dot == '\0' && *extension == '\0';
}

static bool checksum_listed(uint32_t checksum, const uint32_t *list, size_t count) {
	size_t index;

	for (index = 0; index < count; index++) {
		if (list[index] == checksum) return true;
	}
	return false;
}

static int game_gear_runs_as_master_system(const uint8_t *data, size_t size) {
	/* Measured against the black-box reference: the cartridge pin, not the header, selects the mode. */
	static const uint32_t master_system[] = {0x01eab89d, 0xc8381def};
	static const uint32_t game_gear[] = {
		0x6afb8ec6, 0xcb3a1d23, 0x46ad6257, 0x9a8b2c16, 0xc2f7928b, 0x758a7123,
		0xe0e3fb6a, 0x816c0a1e, 0x15ad37a5, 0x0228769c, 0xca0e11cc, 0x840a8f8e
	};
	static const size_t offsets[] = {0x7ff0, 0x3ff0, 0x1ff0};
	size_t skip = size % 0x4000 == 512 ? 512 : 0;
	size_t index;
	uint32_t checksum;

	if (!data) return 0;
	checksum = karashi_state_crc32(data, size);
	if (checksum_listed(checksum, master_system, sizeof(master_system) / sizeof(master_system[0]))) return 1;
	if (checksum_listed(checksum, game_gear, sizeof(game_gear) / sizeof(game_gear[0]))) return 0;
	for (index = 0; index < sizeof(offsets) / sizeof(offsets[0]); index++) {
		size_t offset = skip + offsets[index];
		uint8_t system;

		if (offset + 16 > size || memcmp(data + offset, "TMR SEGA", 8) != 0) continue;
		system = data[offset + 15] & 0xf0;
		return system == 0x30 || system == 0x40;
	}
	return 0;
}

enum karashi_system karashi_content_detect(const char *path, const uint8_t *data, size_t size) {
	if (extension_is(path, "sg") || extension_is(path, "sg1000") || extension_is(path, "sc")) {
		return KARASHI_SYSTEM_SG1000;
	}
	if (extension_is(path, "sms")) return KARASHI_SYSTEM_MASTER_SYSTEM;
	if (extension_is(path, "gg")) {
		return game_gear_runs_as_master_system(data, size) ? KARASHI_SYSTEM_MASTER_SYSTEM : KARASHI_SYSTEM_GAME_GEAR;
	}

	if (extension_is(path, "md") || extension_is(path, "gen") || extension_is(path, "bin")) {
		if (data && size >= 0x104 && memcmp(data + 0x100, "SEGA", 4) == 0) return KARASHI_SYSTEM_MEGA_DRIVE;
		if (extension_is(path, "md") || extension_is(path, "gen")) return KARASHI_SYSTEM_MEGA_DRIVE;
	}
	if (extension_is(path, "32x")) return KARASHI_SYSTEM_32X;
	if (extension_is(path, "smd")) {
		return KARASHI_SYSTEM_MEGA_DRIVE;
	}

	return KARASHI_SYSTEM_UNKNOWN;
}

bool karashi_content_is_sc3000(const char *path) {
	return extension_is(path, "sc") != 0;
}

static bool tag_has(const char *tags, size_t length, const char *word) {
	size_t size = strlen(word);
	size_t position;

	for (position = 0; position + size <= length; position++) {
		if (memcmp(tags + position, word, size) == 0) return true;
	}
	return false;
}

static bool tags_have(const char *tags, size_t length, const char *const *words) {
	for (; *words; words++) {
		if (tag_has(tags, length, *words)) return true;
	}
	return false;
}

static bool needs_pal(const uint8_t *data, size_t size) {
	static const uint32_t checksums[] = {0x2d48c1d3, 0x7e5839a0, 0xc0e25d62};
	uint32_t checksum;
	size_t index;

	if (!data || !size) return false;
	checksum = karashi_state_crc32(data, size);
	for (index = 0; index < sizeof(checksums) / sizeof(checksums[0]); index++) {
		if (checksums[index] == checksum) return true;
	}
	return false;
}

enum karashi_region karashi_content_region(const char *path, const uint8_t *data, size_t size) {
	static const char *const ntsc[] = {"USA", "World", "Brazil", "Europe", NULL};
	static const char *const japan[] = {"Japan", "Korea", "Taiwan", NULL};
	const char *name;
	const char *open;
	const char *close;
	size_t length;

	if (needs_pal(data, size)) return KARASHI_REGION_EXPORT_PAL;
	if (!path) return KARASHI_REGION_EXPORT_NTSC;
	name = strrchr(path, '/');
	name = name ? name + 1 : path;
	open = strchr(name, '(');
	close = open ? strchr(open, ')') : NULL;
	if (!close) return KARASHI_REGION_EXPORT_NTSC;
	length = (size_t) (close - open);

	if (tags_have(open, length, ntsc)) return KARASHI_REGION_EXPORT_NTSC;
	if (tags_have(open, length, japan)) return KARASHI_REGION_JAPAN;
	return KARASHI_REGION_EXPORT_NTSC;
}

const char *karashi_system_name(enum karashi_system system) {
	switch (system) {
		case KARASHI_SYSTEM_SG1000: return "SG-1000";
		case KARASHI_SYSTEM_MASTER_SYSTEM: return "Master System";
		case KARASHI_SYSTEM_GAME_GEAR: return "Game Gear";
		case KARASHI_SYSTEM_MEGA_DRIVE: return "Mega Drive / Genesis";
		case KARASHI_SYSTEM_32X: return "32X";
		default: return "Unknown";
	}
}
