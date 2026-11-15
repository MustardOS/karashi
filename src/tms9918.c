#include "tms9918.h"

#include <string.h>

static const karashi_pixel palette[16] = {
	KARASHI_PIXEL(0x00, 0x00, 0x00), KARASHI_PIXEL(0x00, 0x00, 0x00), KARASHI_PIXEL(0x21, 0xc8, 0x42), KARASHI_PIXEL(0x5e, 0xdc, 0x78),
	KARASHI_PIXEL(0x54, 0x55, 0xed), KARASHI_PIXEL(0x7d, 0x76, 0xfc), KARASHI_PIXEL(0xd4, 0x52, 0x4d), KARASHI_PIXEL(0x42, 0xeb, 0xf5),
	KARASHI_PIXEL(0xfc, 0x55, 0x54), KARASHI_PIXEL(0xff, 0x79, 0x78), KARASHI_PIXEL(0xd4, 0xc1, 0x54), KARASHI_PIXEL(0xe6, 0xce, 0x80),
	KARASHI_PIXEL(0x21, 0xb0, 0x3b), KARASHI_PIXEL(0xc9, 0x5b, 0xba), KARASHI_PIXEL(0xcc, 0xcc, 0xcc), KARASHI_PIXEL(0xff, 0xff, 0xff)
};

void karashi_tms9918_reset(struct karashi_tms9918 *vdp) {
	memset(vdp, 0, sizeof(*vdp));
	vdp->palette = palette;
}

uint8_t karashi_tms9918_read_data(struct karashi_tms9918 *vdp) {
	uint8_t value = vdp->read_buffer;

	vdp->read_buffer = vdp->vram[vdp->address & 0x3fff];
	vdp->address = (uint16_t) ((vdp->address + 1) & 0x3fff);
	vdp->control_pending = false;
	return value;
}

uint8_t karashi_tms9918_read_status(struct karashi_tms9918 *vdp) {
	uint8_t value = vdp->status;

	vdp->status = 0;
	vdp->control_pending = false;
	return value;
}

void karashi_tms9918_write_data(struct karashi_tms9918 *vdp, uint8_t value) {
	vdp->vram[vdp->address & 0x3fff] = value;
	vdp->address = (uint16_t) ((vdp->address + 1) & 0x3fff);
	vdp->read_buffer = value;
	vdp->control_pending = false;
}

void karashi_tms9918_write_control(struct karashi_tms9918 *vdp, uint8_t value) {
	if (!vdp->control_pending) {
		vdp->control_first = value;
		vdp->control_pending = true;
		return;
	}

	vdp->control_pending = false;
	if (value & 0x80) {
		vdp->registers[value & 7] = vdp->control_first;
		return;
	}

	vdp->address = (uint16_t) ((((uint16_t) value & 0x3f) << 8) | vdp->control_first);
	if (!(value & 0x40)) {
		vdp->read_buffer = vdp->vram[vdp->address & 0x3fff];
		vdp->address = (uint16_t) ((vdp->address + 1) & 0x3fff);
	}
}

void karashi_tms9918_vblank(struct karashi_tms9918 *vdp) {
	vdp->status |= 0x80;
}

bool karashi_tms9918_interrupt(const struct karashi_tms9918 *vdp) {
	return (vdp->status & 0x80) && (vdp->registers[1] & 0x20);
}

static karashi_pixel color(const struct karashi_tms9918 *vdp, unsigned index) {
	return vdp->palette[index & 15];
}

enum display_mode {
	MODE_GRAPHICS_ONE,
	MODE_TEXT,
	MODE_GRAPHICS_TWO,
	MODE_MULTICOLOR
};

static enum display_mode mode(const struct karashi_tms9918 *vdp) {
	if (vdp->registers[1] & 0x10) return MODE_TEXT;
	if (vdp->registers[1] & 0x08) return MODE_MULTICOLOR;
	if (vdp->registers[0] & 0x02) return MODE_GRAPHICS_TWO;
	return MODE_GRAPHICS_ONE;
}

static void background_line(const struct karashi_tms9918 *vdp, unsigned y, karashi_pixel *row) {
	enum display_mode display_mode = mode(vdp);
	unsigned backdrop = vdp->registers[7] & 15;
	unsigned name_base = (vdp->registers[2] & 15) << 10;
	unsigned pattern_base = (vdp->registers[4] & 7) << 11;
	unsigned color_base = vdp->registers[3] << 6;
	unsigned tile_y = y >> 3;
	unsigned line = y & 7;
	unsigned x;

	for (x = 0; x < KARASHI_TMS9918_WIDTH; x++) row[x] = color(vdp, backdrop);
	if (!(vdp->registers[1] & 0x40)) return;

	if (display_mode == MODE_TEXT) {
		unsigned foreground = vdp->registers[7] >> 4;
		unsigned column;
		for (column = 0; column < 40; column++) {
			uint8_t name = vdp->vram[(name_base + tile_y * 40 + column) & 0x3fff];
			uint8_t pattern = vdp->vram[(pattern_base + name * 8 + line) & 0x3fff];
			unsigned pixel;
			for (pixel = 0; pixel < 6; pixel++) {
				row[8 + column * 6 + pixel] = color(vdp, (pattern & (0x80 >> pixel)) ? foreground : backdrop);
			}
		}
		return;
	}

	for (x = 0; x < 32; x++) {
		uint8_t name = vdp->vram[(name_base + tile_y * 32 + x) & 0x3fff];
		uint8_t pattern;
		uint8_t colors;
		unsigned pixel;

		if (display_mode == MODE_GRAPHICS_TWO) {
			unsigned offset = ((y & 0xc0) << 5) | (name << 3) | line;
			unsigned pattern_mask = ((vdp->registers[4] & 3) << 11) | 0x07ff;
			unsigned color_mask = ((vdp->registers[3] & 0x7f) << 6) | 0x003f;
			pattern = vdp->vram[((vdp->registers[4] & 4) << 11) | (offset & pattern_mask)];
			colors = vdp->vram[((vdp->registers[3] & 0x80) << 6) | (offset & color_mask)];
		} else if (display_mode == MODE_MULTICOLOR) {
			pattern = vdp->vram[(pattern_base + name * 8 + ((y >> 2) & 7)) & 0x3fff];
			colors = pattern;
		} else {
			pattern = vdp->vram[(pattern_base + name * 8 + line) & 0x3fff];
			colors = vdp->vram[(color_base + (name >> 3)) & 0x3fff];
		}

		for (pixel = 0; pixel < 8; pixel++) {
			unsigned selected;
			if (display_mode == MODE_MULTICOLOR) selected = pixel < 4 ? colors >> 4 : colors & 15;
			else selected = (pattern & (0x80 >> pixel)) ? colors >> 4 : colors & 15;
			if (!selected) selected = backdrop;
			row[x * 8 + pixel] = color(vdp, selected);
		}
	}
}

static bool sprite_row(const struct karashi_tms9918 *vdp, unsigned sprite, unsigned y, unsigned *source_row) {
	unsigned attribute_base = (vdp->registers[5] & 0x7f) << 7;
	uint8_t raw_y = vdp->vram[(attribute_base + sprite * 4) & 0x3fff];
	unsigned size = (vdp->registers[1] & 0x02) ? 16 : 8;
	unsigned magnification = (vdp->registers[1] & 0x01) ? 2 : 1;
	int top = raw_y >= 0xd1 ? (int) raw_y - 255 : (int) raw_y + 1;

	if ((int) y < top || (int) y >= top + (int) (size * magnification)) return false;
	*source_row = (unsigned) ((int) y - top) / magnification;
	return true;
}

static void sprite_line(struct karashi_tms9918 *vdp, unsigned y, karashi_pixel *row) {
	unsigned attribute_base = (vdp->registers[5] & 0x7f) << 7;
	unsigned pattern_base = (vdp->registers[6] & 7) << 11;
	unsigned size = (vdp->registers[1] & 0x02) ? 16 : 8;
	unsigned magnification = (vdp->registers[1] & 0x01) ? 2 : 1;
	bool occupied[KARASHI_TMS9918_WIDTH] = {false};
	unsigned visible[4];
	unsigned visible_count = 0;
	unsigned sprite;

	for (sprite = 0; sprite < 32; sprite++) {
		unsigned source_row;
		if (vdp->vram[(attribute_base + sprite * 4) & 0x3fff] == 0xd0) break;
		if (!sprite_row(vdp, sprite, y, &source_row)) continue;
		if (visible_count == 4) {
			if (!(vdp->status & 0x40)) vdp->status = (uint8_t) ((vdp->status & 0xa0) | 0x40 | sprite);
			break;
		}
		visible[visible_count++] = sprite;
	}

	while (visible_count) {
		unsigned source_row = 0;
		uint8_t attributes;
		uint8_t pattern_number;
		int left;
		unsigned source_x;

		sprite = visible[--visible_count];
		sprite_row(vdp, sprite, y, &source_row);
		left = vdp->vram[(attribute_base + sprite * 4 + 1) & 0x3fff];
		pattern_number = vdp->vram[(attribute_base + sprite * 4 + 2) & 0x3fff];
		attributes = vdp->vram[(attribute_base + sprite * 4 + 3) & 0x3fff];
		if (attributes & 0x80) left -= 32;
		if (size == 16) pattern_number &= 0xfc;
		for (source_x = 0; source_x < size; source_x++) {
			unsigned tile = pattern_number + ((source_x >> 3) << 1) + (source_row >> 3);
			uint8_t pattern = vdp->vram[(pattern_base + tile * 8 + (source_row & 7)) & 0x3fff];
			unsigned copy;
			if (!(pattern & (0x80 >> (source_x & 7)))) continue;
			for (copy = 0; copy < magnification; copy++) {
				int x = left + (int) (source_x * magnification + copy);
				if (x < 0 || x >= KARASHI_TMS9918_WIDTH) continue;
				if (occupied[x]) vdp->status |= 0x20;
				occupied[x] = true;
				if (attributes & 15) row[x] = color(vdp, attributes);
			}
		}
	}
}

void karashi_tms9918_render_line(struct karashi_tms9918 *vdp, unsigned line, karashi_pixel *row) {
	background_line(vdp, line, row);
	if ((vdp->registers[1] & 0x40) && mode(vdp) != MODE_TEXT) sprite_line(vdp, line, row);
}

void karashi_tms9918_render(struct karashi_tms9918 *vdp, karashi_pixel *pixels, unsigned pitch) {
	unsigned line;

	for (line = 0; line < KARASHI_TMS9918_HEIGHT; line++) karashi_tms9918_render_line(vdp, line, pixels + line * pitch);
}

bool karashi_tms9918_save(const struct karashi_tms9918 *vdp, struct karashi_state_writer *writer) {
	karashi_state_write_bytes(writer, vdp->vram, sizeof(vdp->vram));
	karashi_state_write_bytes(writer, vdp->registers, sizeof(vdp->registers));
	karashi_state_write_u16(writer, vdp->address);
	karashi_state_write_u8(writer, vdp->read_buffer);
	karashi_state_write_u8(writer, vdp->control_first);
	karashi_state_write_u8(writer, vdp->status);
	return karashi_state_write_bool(writer, vdp->control_pending);
}

bool karashi_tms9918_load(struct karashi_tms9918 *vdp, struct karashi_state_reader *reader) {
	karashi_state_read_bytes(reader, vdp->vram, sizeof(vdp->vram));
	karashi_state_read_bytes(reader, vdp->registers, sizeof(vdp->registers));
	karashi_state_read_u16(reader, &vdp->address);
	karashi_state_read_u8(reader, &vdp->read_buffer);
	karashi_state_read_u8(reader, &vdp->control_first);
	karashi_state_read_u8(reader, &vdp->status);
	if (!karashi_state_read_bool(reader, &vdp->control_pending)) return false;
	vdp->address &= 0x3fff;
	return true;
}
