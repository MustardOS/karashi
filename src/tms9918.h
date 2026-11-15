#ifndef KARASHI_TMS9918_H
#define KARASHI_TMS9918_H

#include "pixel.h"
#include "state.h"

#include <stdbool.h>
#include <stdint.h>

#define KARASHI_TMS9918_WIDTH 256
#define KARASHI_TMS9918_HEIGHT 192

struct karashi_tms9918 {
	uint8_t vram[16384];
	uint8_t registers[8];
	uint16_t address;
	uint8_t read_buffer;
	uint8_t control_first;
	uint8_t status;
	bool control_pending;
	const karashi_pixel *palette;
};

void karashi_tms9918_reset(struct karashi_tms9918 *vdp);
uint8_t karashi_tms9918_read_data(struct karashi_tms9918 *vdp);
uint8_t karashi_tms9918_read_status(struct karashi_tms9918 *vdp);
void karashi_tms9918_write_data(struct karashi_tms9918 *vdp, uint8_t value);
void karashi_tms9918_write_control(struct karashi_tms9918 *vdp, uint8_t value);
void karashi_tms9918_vblank(struct karashi_tms9918 *vdp);
bool karashi_tms9918_interrupt(const struct karashi_tms9918 *vdp);
bool karashi_tms9918_save(const struct karashi_tms9918 *vdp, struct karashi_state_writer *writer);
bool karashi_tms9918_load(struct karashi_tms9918 *vdp, struct karashi_state_reader *reader);
void karashi_tms9918_render_line(struct karashi_tms9918 *vdp, unsigned line, karashi_pixel *row);
void karashi_tms9918_render(struct karashi_tms9918 *vdp, karashi_pixel *pixels, unsigned pitch);

#endif
