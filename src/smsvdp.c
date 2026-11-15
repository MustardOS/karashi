#include "smsvdp.h"

#include <string.h>

#define VRAM_MASK 0x3fff
#define STATUS_FRAME 0x80
#define STATUS_OVERFLOW 0x40
#define STATUS_COLLISION 0x20
#define SPRITES_PER_LINE 8
#define PRIORITY 0x80

static void update_colors(struct karashi_smsvdp *vdp);

static const karashi_pixel legacy_palette[16] = {
	KARASHI_PIXEL(0x00, 0x00, 0x00), KARASHI_PIXEL(0x00, 0x00, 0x00), KARASHI_PIXEL(0x00, 0xaa, 0x00), KARASHI_PIXEL(0x00, 0xff, 0x00),
	KARASHI_PIXEL(0x00, 0x00, 0x55), KARASHI_PIXEL(0x00, 0x00, 0xff), KARASHI_PIXEL(0x55, 0x00, 0x00), KARASHI_PIXEL(0x00, 0xff, 0xff),
	KARASHI_PIXEL(0xaa, 0x00, 0x00), KARASHI_PIXEL(0xff, 0x00, 0x00), KARASHI_PIXEL(0x55, 0x55, 0x00), KARASHI_PIXEL(0xff, 0xff, 0x00),
	KARASHI_PIXEL(0x00, 0x55, 0x00), KARASHI_PIXEL(0xff, 0x00, 0xff), KARASHI_PIXEL(0x55, 0x55, 0x55), KARASHI_PIXEL(0xff, 0xff, 0xff)
};

void karashi_smsvdp_reset(struct karashi_smsvdp *vdp, bool game_gear, bool pal) {
	memset(vdp, 0, sizeof(*vdp));
	karashi_tms9918_reset(&vdp->tms);
	vdp->tms.palette = legacy_palette;
	vdp->game_gear = game_gear;
	vdp->pal = pal;
	vdp->height = KARASHI_SMSVDP_HEIGHT;
	update_colors(vdp);
	vdp->line_counter = 0xff;
	vdp->extended[2] = 0xff;
}

uint8_t karashi_smsvdp_read_data(struct karashi_smsvdp *vdp) {
	return karashi_tms9918_read_data(&vdp->tms);
}

uint8_t karashi_smsvdp_read_status(struct karashi_smsvdp *vdp) {
	uint8_t value = (uint8_t) (vdp->tms.status | 0x1f);

	vdp->tms.status = 0;
	vdp->tms.control_pending = false;
	vdp->line_pending = false;
	return value;
}

uint8_t karashi_smsvdp_read_vcounter(const struct karashi_smsvdp *vdp) {
	unsigned lines = vdp->pal ? KARASHI_SEGA8_PAL_LINES : KARASHI_SEGA8_LINES;
	unsigned last;

	if (vdp->height == 224) last = vdp->pal ? 0x102 : 0xea;
	else if (vdp->height == 240) last = vdp->pal ? 0x10a : lines;
	else last = vdp->pal ? 0xf2 : 0xda;
	return (uint8_t) (vdp->line <= last ? vdp->line : vdp->line - (lines - 256));
}

uint8_t karashi_smsvdp_read_hcounter(const struct karashi_smsvdp *vdp) {
	return vdp->hcounter;
}

void karashi_smsvdp_latch_hcounter(struct karashi_smsvdp *vdp, unsigned line_cycle) {
	unsigned pixel = line_cycle * 3 / 2;

	vdp->hcounter = (uint8_t) (pixel < 0x128 ? pixel >> 1 : 0xe9 + ((pixel - 0x128) >> 1));
}

static void update_color(struct karashi_smsvdp *vdp, unsigned index) {
	unsigned value = vdp->cram[index];
	unsigned red;
	unsigned green;
	unsigned blue;

	if (vdp->game_gear) {
		red = (value & 15) * 17;
		green = ((value >> 4) & 15) * 17;
		blue = ((value >> 8) & 15) * 17;
	} else {
		red = (value & 3) * 85;
		green = ((value >> 2) & 3) * 85;
		blue = ((value >> 4) & 3) * 85;
	}
	vdp->colors[index] = KARASHI_PIXEL(red, green, blue);
}

static void update_colors(struct karashi_smsvdp *vdp) {
	unsigned index;

	for (index = 0; index < 32; index++) update_color(vdp, index);
}

void karashi_smsvdp_write_data(struct karashi_smsvdp *vdp, uint8_t value) {
	struct karashi_tms9918 *tms = &vdp->tms;

	if (vdp->code != 3) {
		karashi_tms9918_write_data(tms, value);
		return;
	}

	if (!vdp->game_gear) {
		vdp->cram[tms->address & 0x1f] = value & 0x3f;
		update_color(vdp, tms->address & 0x1f);
	} else if (tms->address & 1) {
		vdp->cram[(tms->address >> 1) & 0x1f] = (uint16_t) ((((uint16_t) value << 8) | vdp->cram_latch) & 0x0fff);
		update_color(vdp, (tms->address >> 1) & 0x1f);
	} else {
		vdp->cram_latch = value;
	}
	tms->address = (uint16_t) ((tms->address + 1) & VRAM_MASK);
	tms->read_buffer = value;
	tms->control_pending = false;
}

void karashi_smsvdp_write_control(struct karashi_smsvdp *vdp, uint8_t value) {
	struct karashi_tms9918 *tms = &vdp->tms;

	if (!tms->control_pending) {
		tms->control_first = value;
		tms->control_pending = true;
		tms->address = (uint16_t) ((tms->address & 0x3f00) | value);
		return;
	}

	tms->control_pending = false;
	vdp->code = value >> 6;
	tms->address = (uint16_t) ((((uint16_t) value & 0x3f) << 8) | tms->control_first);

	if (vdp->code == 0) {
		tms->read_buffer = tms->vram[tms->address];
		tms->address = (uint16_t) ((tms->address + 1) & VRAM_MASK);
	} else if (vdp->code == 2) {
		unsigned index = value & 0x0f;
		if (index < 8) tms->registers[index] = tms->control_first;
		else if (index < 11) vdp->extended[index - 8] = tms->control_first;
	}
}

bool karashi_smsvdp_interrupt(const struct karashi_smsvdp *vdp) {
	return ((vdp->tms.status & STATUS_FRAME) && (vdp->tms.registers[1] & 0x20))
	       || (vdp->line_pending && (vdp->tms.registers[0] & 0x10));
}

static const struct karashi_smsvdp_row *pattern_row(struct karashi_smsvdp *vdp, unsigned pattern, unsigned line) {
	struct karashi_smsvdp_row *row = &vdp->rows[((pattern << 3) | line) & (512 * 8 - 1)];
	const uint8_t *planes = vdp->tms.vram + ((pattern * 32 + line * 4) & VRAM_MASK);
	unsigned pixel;

	if (memcmp(row->source, planes, 4) == 0) return row;
	memcpy(row->source, planes, 4);
	for (pixel = 0; pixel < 8; pixel++) {
		unsigned bit = 7 - pixel;
		uint8_t value = (uint8_t) (((planes[0] >> bit) & 1) | ((planes[1] >> bit) & 1) << 1
		                           | ((planes[2] >> bit) & 1) << 2 | ((planes[3] >> bit) & 1) << 3);
		row->normal[pixel] = value;
		row->flipped[7 - pixel] = value;
	}
	return row;
}

static void background_line(struct karashi_smsvdp *vdp, unsigned line, uint8_t *indices) {
	const uint8_t *vram = vdp->tms.vram;
	const uint8_t *registers = vdp->tms.registers;
	unsigned hscroll = ((registers[0] & 0x40) && line < 16) ? 0 : vdp->extended[0];
	unsigned fine = hscroll & 7;
	unsigned coarse = hscroll >> 3;
	bool tall = vdp->height != KARASHI_SMSVDP_HEIGHT;
	unsigned name_base = tall ? (((registers[2] & 0x0c) << 10) | 0x0700) : ((registers[2] & 0x0e) << 10);
	unsigned scroll_height = tall ? 256 : 224;
	unsigned scrolled = (line + vdp->vscroll) % scroll_height;
	unsigned column;

	for (column = 0; column < 33; column++) {
		int left = (int) (column * 8 + fine) - 8;
		unsigned tile_column = (column + 31 - coarse) & 31;
		bool locked = (registers[0] & 0x80) && column >= 25;
		unsigned source_line = locked ? line : scrolled;
		unsigned name = (name_base + (source_line >> 3) * 64 + tile_column * 2) & VRAM_MASK;
		unsigned entry = vram[name] | (unsigned) vram[(name + 1) & VRAM_MASK] << 8;
		unsigned tile_line = (entry & 0x400) ? 7 - (source_line & 7) : source_line & 7;
		const struct karashi_smsvdp_row *cached = pattern_row(vdp, entry & 0x1ff, tile_line);
		const uint8_t *pixels = (entry & 0x200) ? cached->flipped : cached->normal;
		uint8_t palette = (entry & 0x800) ? 16 : 0;
		unsigned pixel;

		if (left >= 0 && left <= KARASHI_SMSVDP_WIDTH - 8 && !(entry & 0x1000)) {
			uint64_t packed;
			memcpy(&packed, pixels, sizeof(packed));
			packed |= palette * UINT64_C(0x0101010101010101);
			memcpy(indices + left, &packed, sizeof(packed));
			continue;
		}
		for (pixel = 0; pixel < 8; pixel++) {
			int x = left + (int) pixel;
			unsigned value = pixels[pixel];
			if (x < 0 || x >= KARASHI_SMSVDP_WIDTH) continue;
			indices[x] = (uint8_t) (palette | value | (((entry & 0x1000) && value) ? PRIORITY : 0));
		}
	}
}

static void sprite_line(struct karashi_smsvdp *vdp, unsigned line, uint8_t *indices) {
	const uint8_t *vram = vdp->tms.vram;
	const uint8_t *registers = vdp->tms.registers;
	unsigned table = (registers[5] & 0x7e) << 7;
	unsigned height = (registers[1] & 0x02) ? 16 : 8;
	unsigned zoom = registers[1] & 0x01;
	bool occupied[KARASHI_SMSVDP_WIDTH];
	unsigned visible[SPRITES_PER_LINE];
	int tops[SPRITES_PER_LINE];
	unsigned count = 0;
	unsigned sprite;
	unsigned slot;

	for (sprite = 0; sprite < 64; sprite++) {
		unsigned raw_y = vram[(table + sprite) & VRAM_MASK];
		int top;
		if (raw_y == 0xd0 && vdp->height == KARASHI_SMSVDP_HEIGHT) break;
		top = (raw_y > (vdp->height == KARASHI_SMSVDP_HEIGHT ? 0xd0u : 0xf0u) ? (int) raw_y - 256 : (int) raw_y) + 1;
		if ((int) line < top || (int) line >= top + (int) (height << zoom)) continue;
		if (count == SPRITES_PER_LINE) {
			vdp->tms.status |= STATUS_OVERFLOW;
			break;
		}
		visible[count] = sprite;
		tops[count++] = top;
	}
	if (!count) return;
	memset(occupied, 0, sizeof(occupied));

	for (slot = 0; slot < count; slot++) {
		unsigned attribute = (table + 128 + visible[slot] * 2) & VRAM_MASK;
		int left = vram[attribute] - ((registers[0] & 0x08) ? 8 : 0);
		unsigned pattern = vram[(attribute + 1) & VRAM_MASK] | ((registers[6] & 0x04) ? 0x100 : 0);
		unsigned source_line = (unsigned) ((int) line - tops[slot]) >> zoom;
		const uint8_t *pixels;
		unsigned pixel;

		if (height == 16) pattern &= 0x1fe;
		pixels = pattern_row(vdp, (pattern + (source_line >> 3)) & 0x1ff, source_line & 7)->normal;
		for (pixel = 0; pixel < 8; pixel++) {
			unsigned value = pixels[pixel];
			unsigned copy;
			if (!value) continue;
			for (copy = 0; copy <= zoom; copy++) {
				int x = left + (int) ((pixel << zoom) + copy);
				if (x < 0 || x >= KARASHI_SMSVDP_WIDTH) continue;
				if (occupied[x]) {
					vdp->tms.status |= STATUS_COLLISION;
					continue;
				}
				occupied[x] = true;
				if (!(indices[x] & PRIORITY)) indices[x] = (uint8_t) (16 | value);
			}
		}
	}
}

static void mode4_line(struct karashi_smsvdp *vdp, unsigned line, karashi_pixel *row) {
	const uint8_t *registers = vdp->tms.registers;
	uint8_t backdrop = (uint8_t) (16 | (registers[7] & 15));
	uint8_t indices[KARASHI_SMSVDP_WIDTH];
	unsigned x;

	if (registers[1] & 0x40) {
		background_line(vdp, line, indices);
		sprite_line(vdp, line, indices);
		if (registers[0] & 0x20) memset(indices, backdrop, 8);
	} else {
		memset(indices, backdrop, sizeof(indices));
	}

	for (x = 0; x < KARASHI_SMSVDP_WIDTH; x += 8) {
		uint64_t packed;
		memcpy(&packed, indices + x, sizeof(packed));
		row[x] = vdp->colors[packed & 0x1f];
		row[x + 1] = vdp->colors[(packed >> 8) & 0x1f];
		row[x + 2] = vdp->colors[(packed >> 16) & 0x1f];
		row[x + 3] = vdp->colors[(packed >> 24) & 0x1f];
		row[x + 4] = vdp->colors[(packed >> 32) & 0x1f];
		row[x + 5] = vdp->colors[(packed >> 40) & 0x1f];
		row[x + 6] = vdp->colors[(packed >> 48) & 0x1f];
		row[x + 7] = vdp->colors[(packed >> 56) & 0x1f];
	}
}

void karashi_smsvdp_begin_line(struct karashi_smsvdp *vdp, unsigned line, karashi_pixel *row) {
	vdp->line = (uint16_t) line;
	if (line == 0) {
		const uint8_t *registers = vdp->tms.registers;
		bool extended = (registers[0] & 0x06) == 0x06 && !vdp->game_gear;

		vdp->vscroll = vdp->extended[1];
		vdp->height = KARASHI_SMSVDP_HEIGHT;
		if (extended && (registers[1] & 0x18) == 0x10) vdp->height = 224;
		else if (extended && (registers[1] & 0x18) == 0x08) vdp->height = 240;
	}
	if (line >= vdp->height) return;

	if (vdp->tms.registers[0] & 0x04) mode4_line(vdp, line, row);
	else karashi_tms9918_render_line(&vdp->tms, line, row);
}

void karashi_smsvdp_end_line(struct karashi_smsvdp *vdp, unsigned line) {
	unsigned lines = vdp->pal ? KARASHI_SEGA8_PAL_LINES : KARASHI_SEGA8_LINES;
	unsigned next = line + 1 == lines ? 0 : line + 1;

	if (line == vdp->height) vdp->tms.status |= STATUS_FRAME;
	if (next > vdp->height) {
		vdp->line_counter = vdp->extended[2];
	} else if (vdp->line_counter == 0) {
		vdp->line_counter = vdp->extended[2];
		vdp->line_pending = true;
	} else {
		vdp->line_counter--;
	}
}

bool karashi_smsvdp_save(const struct karashi_smsvdp *vdp, struct karashi_state_writer *writer) {
	unsigned index;

	karashi_tms9918_save(&vdp->tms, writer);
	for (index = 0; index < 32; index++) karashi_state_write_u16(writer, vdp->cram[index]);
	karashi_state_write_bytes(writer, vdp->extended, sizeof(vdp->extended));
	karashi_state_write_u8(writer, vdp->code);
	karashi_state_write_u8(writer, vdp->cram_latch);
	karashi_state_write_u8(writer, vdp->line_counter);
	karashi_state_write_u8(writer, vdp->vscroll);
	karashi_state_write_u8(writer, vdp->hcounter);
	karashi_state_write_u16(writer, vdp->line);
	karashi_state_write_u16(writer, vdp->height);
	return karashi_state_write_bool(writer, vdp->line_pending);
}

bool karashi_smsvdp_load(struct karashi_smsvdp *vdp, struct karashi_state_reader *reader) {
	unsigned index;

	karashi_tms9918_load(&vdp->tms, reader);
	for (index = 0; index < 32; index++) karashi_state_read_u16(reader, &vdp->cram[index]);
	karashi_state_read_bytes(reader, vdp->extended, sizeof(vdp->extended));
	karashi_state_read_u8(reader, &vdp->code);
	karashi_state_read_u8(reader, &vdp->cram_latch);
	karashi_state_read_u8(reader, &vdp->line_counter);
	karashi_state_read_u8(reader, &vdp->vscroll);
	karashi_state_read_u8(reader, &vdp->hcounter);
	karashi_state_read_u16(reader, &vdp->line);
	karashi_state_read_u16(reader, &vdp->height);
	if (!karashi_state_read_bool(reader, &vdp->line_pending)) return false;
	for (index = 0; index < 32; index++) vdp->cram[index] &= 0x0fff;
	update_colors(vdp);
	vdp->code &= 3;
	return vdp->height == KARASHI_SMSVDP_HEIGHT || vdp->height == 224 || vdp->height == 240;
}
