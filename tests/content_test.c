#include "content.h"
#include "state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_system(const char *name, enum karashi_system actual, enum karashi_system expected) {
	if (actual == expected) return;
	fprintf(stderr, "%s: expected %s, got %s\n", name, karashi_system_name(expected), karashi_system_name(actual));
	failures++;
}

int main(void) {
	unsigned char header[0x104] = {0};
	unsigned char sega8[0x8200] = {0};
	static const struct {
		uint32_t checksum;
		uint8_t image[4];
	} pal_cases[] = {
		{0x0047b615, {0xdc, 0x8a, 0x53, 0xef}},
		{0x1575581d, {0x2f, 0xe6, 0x19, 0x1e}},
		{0x1b10a951, {0xbc, 0xde, 0x2c, 0x67}},
		{0x29822980, {0x35, 0x37, 0x96, 0x73}},
		{0x2d48c1d3, {0xa1, 0x5c, 0x83, 0x5e}},
		{0x332a847d, {0x00, 0x19, 0x29, 0x2d}},
		{0x5b3b922c, {0x5c, 0x2a, 0xd3, 0x19}},
		{0x72420f38, {0x26, 0x70, 0xa5, 0x31}},
		{0x7e5839a0, {0x43, 0xe9, 0x8c, 0x0e}},
		{0x85cfc9c9, {0x11, 0x13, 0x30, 0x1f}},
		{0x8813514b, {0xd0, 0xa9, 0x96, 0x0d}},
		{0x9f951756, {0x39, 0x3f, 0xec, 0x02}},
		{0xa109a6fe, {0x1a, 0x2b, 0xa3, 0x08}},
		{0xa577ce46, {0x4b, 0x25, 0x96, 0xa4}},
		{0xb9664ae1, {0xc3, 0xfe, 0x06, 0x5d}},
		{0xc0e25d62, {0x72, 0x9f, 0x0c, 0x8b}},
		{0xc9dbf936, {0x74, 0xb5, 0x80, 0x16}},
		{0xca1d3752, {0x23, 0x4d, 0x38, 0xaa}},
		{0xd6f2bfca, {0xd2, 0xa6, 0x23, 0xd7}},
		{0xf8176918, {0x20, 0x51, 0x92, 0x74}}
	};
	static const unsigned char california_games_two_brazil_korea[] = {
		0xc7, 0xf3, 0xab, 0xc2
	};
	size_t index;
	memcpy(header + 0x100, "SEGA", 4);

	expect_system("sg", karashi_content_detect("game.sg", NULL, 0), KARASHI_SYSTEM_SG1000);
	expect_system("sc", karashi_content_detect("game.SC", NULL, 0), KARASHI_SYSTEM_SG1000);
	assert(karashi_content_is_sc3000("game.SC"));
	assert(!karashi_content_is_sc3000("game.sg"));
	expect_system("sms", karashi_content_detect("game.SMS", NULL, 0), KARASHI_SYSTEM_MASTER_SYSTEM);
	expect_system("gg", karashi_content_detect("game.gg", NULL, 0), KARASHI_SYSTEM_GAME_GEAR);
	memcpy(sega8 + 0x7ff0, "TMR SEGA", 8);
	sega8[0x7fff] = 0x40;
	expect_system("gg sms mode", karashi_content_detect("game.gg", sega8, 0x8000), KARASHI_SYSTEM_MASTER_SYSTEM);
	sega8[0x7fff] = 0x60;
	expect_system("gg native mode", karashi_content_detect("game.gg", sega8, 0x8000), KARASHI_SYSTEM_GAME_GEAR);
	memset(sega8, 0, sizeof(sega8));
	memcpy(sega8 + 0x1ff0, "TMR SEGA", 8);
	sega8[0x1fff] = 0x40;
	expect_system("gg sms mode 8k", karashi_content_detect("game.gg", sega8, 0x2000), KARASHI_SYSTEM_MASTER_SYSTEM);
	memset(sega8, 0, sizeof(sega8));
	memcpy(sega8 + 0x3ff0, "TMR SEGA", 8);
	sega8[0x3fff] = 0x40;
	expect_system("gg sms mode 16k", karashi_content_detect("game.gg", sega8, 0x4000), KARASHI_SYSTEM_MASTER_SYSTEM);
	memset(sega8, 0, sizeof(sega8));
	memcpy(sega8 + 0x81f0, "TMR SEGA", 8);
	sega8[0x81ff] = 0x30;
	expect_system("gg sms mode copier header", karashi_content_detect("game.gg", sega8, sizeof(sega8)),
	              KARASHI_SYSTEM_MASTER_SYSTEM);
	expect_system("md", karashi_content_detect("game.md", NULL, 0), KARASHI_SYSTEM_MEGA_DRIVE);
	expect_system("header", karashi_content_detect("game.bin", header, sizeof(header)), KARASHI_SYSTEM_MEGA_DRIVE);
	expect_system("unknown", karashi_content_detect("game.bin", NULL, 0), KARASHI_SYSTEM_UNKNOWN);

	if (karashi_content_region("/roms/Sonic (USA, Europe, Brazil).sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region("/roms/Sonic (World).sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region("/roms/Addams Family, The (Europe).sms", NULL, 0) != KARASHI_REGION_EXPORT_PAL) failures++;
	if (karashi_content_region("/roms/4 PAK All Action (Australia).sms", NULL, 0) != KARASHI_REGION_EXPORT_PAL) failures++;
	if (karashi_content_region("/roms/Aleste (Japan).sms", NULL, 0) != KARASHI_REGION_JAPAN) failures++;
	if (karashi_content_region("/roms/Game (Japan, Europe).sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region("/roms/Game (Korea) (Unl).sms", NULL, 0) != KARASHI_REGION_JAPAN) failures++;
	if (karashi_content_region("/roms/untagged.sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region(NULL, NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	for (index = 0; index < sizeof(pal_cases) / sizeof(pal_cases[0]); index++) {
		if (karashi_state_crc32(pal_cases[index].image, sizeof(pal_cases[index].image)) != pal_cases[index].checksum) failures++;
		if (karashi_content_region("untagged.sms", pal_cases[index].image,
		                           sizeof(pal_cases[index].image)) != KARASHI_REGION_EXPORT_PAL) failures++;
	}
	if (karashi_content_region("California Games II (Brazil, Korea).sms", california_games_two_brazil_korea,
	                           sizeof(california_games_two_brazil_korea)) != KARASHI_REGION_EXPORT_NTSC) failures++;

	if (failures) return 1;
	puts("content detection: pass");
	return 0;
}
