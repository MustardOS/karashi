#ifndef KARASHI_SMSVDP_H
#define KARASHI_SMSVDP_H

#include "sega8.h"
#include "state.h"
#include "tms9918.h"

#include <stdbool.h>
#include <stdint.h>

#define KARASHI_SMSVDP_WIDTH KARASHI_TMS9918_WIDTH
#define KARASHI_SMSVDP_HEIGHT KARASHI_TMS9918_HEIGHT
#define KARASHI_SMSVDP_MAX_HEIGHT 240

struct karashi_smsvdp_row {
	uint8_t source[4];
	uint8_t normal[8];
	uint8_t flipped[8];
};

struct karashi_smsvdp {
	struct karashi_tms9918 tms;
	struct karashi_smsvdp_row rows[512 * 8];
	karashi_pixel colors[32];
	uint16_t cram[32];
	uint8_t extended[3];
	uint8_t code;
	uint8_t cram_latch;
	uint8_t line_counter;
	uint8_t vscroll;
	uint8_t hcounter;
	uint16_t line;
	uint16_t height;
	bool line_pending;
	bool game_gear;
	bool pal;
};

void karashi_smsvdp_reset(struct karashi_smsvdp *vdp, bool game_gear, bool pal);
uint8_t karashi_smsvdp_read_data(struct karashi_smsvdp *vdp);
uint8_t karashi_smsvdp_read_status(struct karashi_smsvdp *vdp);
uint8_t karashi_smsvdp_read_vcounter(const struct karashi_smsvdp *vdp);
uint8_t karashi_smsvdp_read_hcounter(const struct karashi_smsvdp *vdp);
void karashi_smsvdp_write_data(struct karashi_smsvdp *vdp, uint8_t value);
void karashi_smsvdp_write_control(struct karashi_smsvdp *vdp, uint8_t value);
void karashi_smsvdp_latch_hcounter(struct karashi_smsvdp *vdp, unsigned line_cycle);
bool karashi_smsvdp_interrupt(const struct karashi_smsvdp *vdp);
void karashi_smsvdp_begin_line(struct karashi_smsvdp *vdp, unsigned line, karashi_pixel *row);
void karashi_smsvdp_end_line(struct karashi_smsvdp *vdp, unsigned line);
bool karashi_smsvdp_save(const struct karashi_smsvdp *vdp, struct karashi_state_writer *writer);
bool karashi_smsvdp_load(struct karashi_smsvdp *vdp, struct karashi_state_reader *reader);

#endif
