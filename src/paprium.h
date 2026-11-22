#ifndef KARASHI_PAPRIUM_H
#define KARASHI_PAPRIUM_H

#include "state.h"

struct karashi_md;
struct karashi_paprium {
	uint8_t page[65536], decoded[65536], texture[2048];
	uint32_t cursor, remaining, object_table, tile_table, frame[256], draw_source, draw_vram;
	uint8_t control, mode, initialized_banks;
	bool restart_pending;
	/* Rebuilt from the cartridge on reset and state restore. */
	uint8_t objects[524288];
	size_t object_size;
};

void karashi_paprium_reset(struct karashi_md *md);
uint16_t karashi_paprium_read(struct karashi_md *md, unsigned address);
void karashi_paprium_command(struct karashi_md *md, uint16_t command);
void karashi_paprium_save(const struct karashi_md *md, struct karashi_state_writer *writer);
bool karashi_paprium_load(struct karashi_md *md, struct karashi_state_reader *reader);
#endif
