#ifndef KARASHI_SMS_H
#define KARASHI_SMS_H

#include "eeprom93.h"
#include "sega8.h"
#include "smsvdp.h"
#include "sn76489.h"
#include "ym2413.h"
#include "z80.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KARASHI_SMS_RAM_SIZE 8192
#define KARASHI_SMS_CART_RAM_SIZE 32768
#define KARASHI_SMS_ROM_BLOCKS 48
#define KARASHI_GG_WIDTH 160
#define KARASHI_GG_HEIGHT 144
#define KARASHI_GG_LEFT 48
#define KARASHI_GG_TOP 24

#define KARASHI_SMS_PAD_UP 0x01
#define KARASHI_SMS_PAD_DOWN 0x02
#define KARASHI_SMS_PAD_LEFT 0x04
#define KARASHI_SMS_PAD_RIGHT 0x08
#define KARASHI_SMS_PAD_ONE 0x10
#define KARASHI_SMS_PAD_TWO 0x20

enum karashi_sms_device {
	KARASHI_SMS_DEVICE_PAD = 0,
	KARASHI_SMS_DEVICE_PHASER,
	KARASHI_SMS_DEVICE_PADDLE,
	KARASHI_SMS_DEVICE_SPORTS_PAD
};

enum karashi_sms_mapper {
	KARASHI_SMS_MAPPER_SEGA = 0,
	KARASHI_SMS_MAPPER_CODEMASTERS,
	KARASHI_SMS_MAPPER_KOREAN,
	KARASHI_SMS_MAPPER_MSX
};

struct karashi_sms {
	struct karashi_z80 cpu;
	struct karashi_smsvdp vdp;
	struct karashi_sn76489 psg;
	const uint8_t *rom;
	size_t rom_size;
	const uint8_t *bios;
	size_t bios_size;
	const uint8_t *rom_map[KARASHI_SMS_ROM_BLOCKS];
	uint8_t ram[KARASHI_SMS_RAM_SIZE];
	uint8_t cart_ram[KARASHI_SMS_CART_RAM_SIZE];
	uint8_t pages[3];
	uint8_t ram_control;
	uint8_t memory_control;
	uint8_t io_control;
	uint8_t controller[2];
	uint8_t mapper;
	uint8_t base_mapper;
	uint8_t banks8[4];
	bool sega_paging_seen;
	uint8_t device[2];
	bool phaser_trigger[2];
	int16_t phaser_x[2];
	int16_t phaser_y[2];
	uint16_t phaser_latched;
	uint8_t paddle[2];
	bool paddle_button[2];
	uint8_t sports_buttons[2];
	int16_t sports_move[2][2];
	uint8_t sports_position[2][2];
	uint8_t sports_latch[2][2];
	uint8_t sports_index[2];
	uint64_t sports_edge[2];
	bool licensed;
	bool eeprom_present;
	bool eeprom_enabled;
	struct karashi_eeprom93 eeprom;
	bool fm_present;
	uint8_t fm_control;
	struct karashi_ym2413 fm;
	bool game_gear;
	bool japanese;
	bool pal;
	bool pause;
	bool pause_seen;
	uint64_t line_start;
	uint64_t line_event;
	uint64_t sound_clock;
	uint64_t fm_clock;
	unsigned line;
	bool line_event_pending;
	uint32_t audio_clock;
	unsigned audio_frames;
	unsigned fm_frames;
	karashi_pixel video[KARASHI_SMSVDP_WIDTH * KARASHI_SMSVDP_MAX_HEIGHT];
	int16_t audio[KARASHI_SEGA8_AUDIO_CAPACITY * 2];
	int32_t fm_audio[KARASHI_SEGA8_AUDIO_CAPACITY];
};

void karashi_sms_init(
	struct karashi_sms *machine, const uint8_t *rom, size_t rom_size, bool game_gear, bool japanese, bool pal
);
unsigned karashi_sms_height(const struct karashi_sms *machine);
double karashi_sms_frame_rate(const struct karashi_sms *machine);
void karashi_sms_set_bios(struct karashi_sms *machine, const uint8_t *bios, size_t bios_size);
void karashi_sms_set_region(struct karashi_sms *machine, bool japanese, bool pal);
void karashi_sms_reset(struct karashi_sms *machine);
void karashi_sms_set_controller(struct karashi_sms *machine, unsigned port, uint8_t state);
void karashi_sms_set_pause(struct karashi_sms *machine, bool pressed);
void karashi_sms_set_device(struct karashi_sms *machine, unsigned port, enum karashi_sms_device device);
void karashi_sms_set_phaser(struct karashi_sms *machine, unsigned port, int x, int y, bool trigger);
void karashi_sms_set_paddle(struct karashi_sms *machine, unsigned port, uint8_t position, bool button);
void karashi_sms_set_sports_pad(struct karashi_sms *machine, unsigned port, int x, int y, uint8_t buttons);
void karashi_sms_run_frame(struct karashi_sms *machine);
bool karashi_sms_save(const struct karashi_sms *machine, struct karashi_state_writer *writer);
bool karashi_sms_load(struct karashi_sms *machine, struct karashi_state_reader *reader);

#endif
