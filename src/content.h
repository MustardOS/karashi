#ifndef KARASHI_CONTENT_H
#define KARASHI_CONTENT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

enum karashi_system {
	KARASHI_SYSTEM_UNKNOWN = 0,
	KARASHI_SYSTEM_SG1000,
	KARASHI_SYSTEM_MASTER_SYSTEM,
	KARASHI_SYSTEM_GAME_GEAR,
	KARASHI_SYSTEM_MEGA_DRIVE,
	KARASHI_SYSTEM_32X
};

enum karashi_region {
	KARASHI_REGION_EXPORT_NTSC = 0,
	KARASHI_REGION_EXPORT_PAL,
	KARASHI_REGION_JAPAN
};

enum karashi_region karashi_content_region(const char *path, const uint8_t *data, size_t size);
enum karashi_system karashi_content_detect(const char *path, const uint8_t *data, size_t size);
bool karashi_content_is_sc3000(const char *path);
const char *karashi_system_name(enum karashi_system system);

#endif
