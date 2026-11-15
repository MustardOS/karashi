#include "content.h"

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
	static const unsigned char california_games_two_europe[] = {
		0x72, 0x9f, 0x0c, 0x8b
	};
	static const unsigned char california_games_two_brazil_korea[] = {
		0xc7, 0xf3, 0xab, 0xc2
	};
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
	if (karashi_content_region("/roms/Addams Family, The (Europe).sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region("/roms/Aleste (Japan).sms", NULL, 0) != KARASHI_REGION_JAPAN) failures++;
	if (karashi_content_region("/roms/Game (Japan, Europe).sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region("/roms/Game (Korea) (Unl).sms", NULL, 0) != KARASHI_REGION_JAPAN) failures++;
	if (karashi_content_region("/roms/untagged.sms", NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region(NULL, NULL, 0) != KARASHI_REGION_EXPORT_NTSC) failures++;
	if (karashi_content_region("California Games II (Europe).sms", california_games_two_europe,
	                           sizeof(california_games_two_europe)) != KARASHI_REGION_EXPORT_PAL) failures++;
	if (karashi_content_region("California Games II (Brazil, Korea).sms", california_games_two_brazil_korea,
	                           sizeof(california_games_two_brazil_korea)) != KARASHI_REGION_EXPORT_NTSC) failures++;

	if (failures) return 1;
	puts("content detection: pass");
	return 0;
}
