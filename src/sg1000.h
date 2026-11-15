#ifndef KARASHI_SG1000_H
#define KARASHI_SG1000_H

#include "sega8.h"
#include "sn76489.h"
#include "tms9918.h"
#include "z80.h"

#include <stddef.h>
#include <stdint.h>

#define KARASHI_SG1000_CART_RAM_SIZE 8192

struct karashi_sg1000 {
	struct karashi_z80 cpu;
	struct karashi_tms9918 vdp;
	struct karashi_sn76489 psg;
	const uint8_t *rom;
	size_t rom_size;
	uint8_t ram[1024];
	uint8_t cart_ram[KARASHI_SG1000_CART_RAM_SIZE];
	uint8_t expansion_ram[KARASHI_SG1000_CART_RAM_SIZE];
	uint8_t work_ram[KARASHI_SG1000_CART_RAM_SIZE];
	bool msx_layout;
	bool sc3000;
	uint16_t keyboard[8];
	uint8_t ppi_port_c;
	uint8_t ppi_control;
	uint8_t controller[2];
	bool pause;
	bool pause_seen;
	uint32_t audio_clock;
	unsigned audio_frames;
	karashi_pixel video[KARASHI_TMS9918_WIDTH * KARASHI_TMS9918_HEIGHT];
	int16_t audio[KARASHI_SEGA8_AUDIO_CAPACITY * 2];
};

void karashi_sg1000_init(struct karashi_sg1000 *machine, const uint8_t *rom, size_t rom_size, bool sc3000);
void karashi_sg1000_reset(struct karashi_sg1000 *machine);
void karashi_sg1000_set_controller(struct karashi_sg1000 *machine, unsigned port, uint8_t state);
void karashi_sg1000_set_pause(struct karashi_sg1000 *machine, bool pressed);
void karashi_sg1000_clear_keys(struct karashi_sg1000 *machine);
void karashi_sg1000_set_key(struct karashi_sg1000 *machine, unsigned column, unsigned row, bool pressed);
void karashi_sg1000_run_frame(struct karashi_sg1000 *machine);
bool karashi_sg1000_save(const struct karashi_sg1000 *machine, struct karashi_state_writer *writer);
bool karashi_sg1000_load(struct karashi_sg1000 *machine, struct karashi_state_reader *reader);

#endif
