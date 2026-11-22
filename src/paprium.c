#include "md.h"

#include <string.h>

static unsigned word(const uint8_t *p) {
	return (unsigned)p[0] << 8 | p[1];
}

static uint32_t address32(const uint8_t *p) {
	return (uint32_t)word(p) << 16 | word(p + 2);
}

static void put(uint8_t *p, unsigned value) {
	p[0] = value >> 8;
	p[1] = value;
}

static size_t unpack(const struct karashi_md *md, uint32_t address, uint8_t *out, size_t capacity) {
	const uint8_t *in, *end = md->rom + md->rom_size;
	size_t used = 0;
	unsigned state = 0;
	if (address >= md->rom_size) return 0;
	in = md->rom + address;
	unsigned format = *in++;
	if (format != 0x80 && format != 0x81) return 0;
	while (in < end) {
		unsigned token = *in++;
		size_t literal = 0, match = 0, distance = 0;
		if (format == 0x80) {
			unsigned n = token & 63;
			if (!token) return used;
			if (n > capacity - used) return 0;
			if (token < 64) literal = n;
			else if (token >= 128 && token < 192) {
				if (in == end) return 0;
				distance = *in++;
				match = n;
			} else {
				if (token < 128 && in == end) return 0;
				memset(out + used, token < 128 ? *in++ : 0, n);
				used += n;
			}
		} else if (token >= 64 || (token < 16 && state)) {
			if (in == end) return 0;
			unsigned offset = *in++;
			literal = token & 3;
			if (token >= 64) {
				match = (token >> 5) + 1;
				distance = 1 + ((token >> 2) & 7) + 8 * offset;
			} else {
				match = state == 4 ? 3 : 2;
				distance = (state == 4 ? 2049 : 1) + (token >> 2) + 4 * offset;
			}
		} else {
			unsigned mask = token < 16 ? 15 : token < 32 ? 7 : 31;
			size_t length = token & mask;
			if (!length) {
				length = mask;
				while (in < end && !*in) {
					if (length > capacity) return 0;
					length += 255;
					in++;
				}
				if (in == end) return 0;
				length += *in++;
			}
			if (token < 16) literal = length + 3;
			else {
				if (end - in < 2) return 0;
				unsigned offset = in[0] | (unsigned)in[1] << 8;
				in += 2;
				literal = offset & 3;
				distance = offset >> 2;
				if (token < 32) {
					distance += (token & 8) << 11;
					if (!distance) return used;
					distance += 16384;
				} else distance++;
				match = length + 2;
			}
		}
		state = match ? literal : 4;
		if (match > capacity - used || (match && (!distance || distance > used))) return 0;
		while (match--) {
			out[used] = out[used - distance];
			used++;
		}
		if (literal > capacity - used || literal > (size_t)(end - in)) return 0;
		memcpy(out + used, in, literal);
		used += literal;
		in += literal;
	}
	return 0;
}

static void objects(struct karashi_md *md) {
	struct karashi_paprium *p = &md->paprium;
	p->object_size = unpack(md, p->object_table, p->objects, sizeof(p->objects));
}

void karashi_paprium_reset(struct karashi_md *md) {
	struct karashi_paprium *p = &md->paprium;
	memset(p, 0, sizeof(*p));
	memcpy(p->page, md->rom, sizeof(p->page));
	/* The cartridge firmware installs the main program entry in its shared page. */
	put(p->page + 0x1560, 0x4ef9);
	put(p->page + 0x1562, 1);
	put(p->page + 0x1564, 0x0100);
	p->page[0x1d1d] = 4;
	p->page[0x1d2c] = 0x67;
	p->object_table = address32(md->rom + 0x10014);
	uint32_t directory = address32(md->rom + 0xaf77c);
	if (directory <= md->rom_size - 0x784) p->tile_table = address32(md->rom + directory + 0x780);
	objects(md);
}

uint16_t karashi_paprium_read(struct karashi_md *md, unsigned address) {
	struct karashi_paprium *p = &md->paprium;
	switch (address) {
	case 0x1fe4: return 0xffbb;
	case 0x1fe6: return 0xbcff;
	case 0x1fea: return 0x7fff;
	case 0xc000: {
		unsigned chunk = p->mode == 7 ? 0x800 : 0x4000;
		if (chunk > p->remaining) chunk = p->remaining;
		memcpy(p->page + address, p->decoded + p->cursor, chunk);
		p->cursor += chunk;
		p->remaining -= chunk;
		break;
	}
	}
	return word(p->page + (address & 0xfffe));
}

static void transfer(uint8_t *entry, unsigned source, unsigned target, unsigned bytes) {
	put(entry, 0x8f02);
	put(entry + 2, 0x9300 | ((bytes >> 1) & 255));
	put(entry + 4, 0x9500 | ((source >> 1) & 255));
	put(entry + 6, 0x9400 | (bytes >> 9));
	put(entry + 8, 0x9700);
	put(entry + 10, 0x9600 | (source >> 9));
	put(entry + 12, 0x4000 | (target & 0x3fff));
	put(entry + 14, 0x80 | (target >> 14));
}

/* Object directory pointers consist of two big-endian words, low word first. */
static uint32_t object_entry(const struct karashi_paprium *p, size_t offset) {
	if (offset > p->object_size || p->object_size - offset < 4) return 0;
	return word(p->objects + offset) | (uint32_t)word(p->objects + offset + 2) << 16;
}

static void draw(struct karashi_md *md, unsigned index) {
	struct karashi_paprium *p = &md->paprium;
	uint8_t *desc = p->page + 0xf80 + 16 * index;
	unsigned anim = word(desc), next = word(desc + 2), attr = word(desc + 8);
	uint32_t directory = object_entry(p, ((word(desc + 4) & 0x7fff) + 1) * 4);
	uint32_t frame = p->frame[index], sprite;
	bool reset = word(desc + 10) == 1;
	if (directory >= p->object_size) return;
	for (;;) {
		if (reset) {
			frame = object_entry(p, directory + anim * 4);
			put(desc + 10, 0);
		}
		if (!frame || frame >= p->object_size) {
			put(desc, 0);
			return;
		}
		uint32_t entry = object_entry(p, frame);
		sprite = entry & 0xffffff;
		if (!sprite || sprite + 2 > p->object_size) return;
		frame += 4;
		if (entry >> 31) break;
		if (next == 0xffff) frame = object_entry(p, frame) & 0xffffff;
		else if (!reset) {
			anim = next;
			reset = true;
			continue;
		}
		break;
	}
	p->frame[index] = frame;
	unsigned parts = p->objects[sprite];
	sprite += 2;
	int x = (int16_t)word(desc + 12), y = (int16_t)word(desc + 14);
	for (unsigned i = 0; i < parts && sprite + 8 <= p->object_size; i++, sprite += 8) {
		const uint8_t *part = p->objects + sprite;
		unsigned size = part[2] & 15, w = 1 + (size >> 2), h = 1 + (size & 3);
		unsigned tile = word(part + 4), flags = word(part + 6);
		unsigned size_bytes = w * h * 32, offset = (flags & 511) * 32;
		unsigned count = word(p->page + 0x1f18), dma = word(p->page + 0x1f16);
		bool flip = (attr & 0x800) != 0;
		x += flip ? -(int8_t)part[0] : (int8_t)part[0];
		y += (int8_t)part[1];
		int left = x - (flip ? (int)w * 8 : 0);
		if (!tile || count >= 80 || dma >= 64 || left >= 448 || left + (int)w * 8 < 128
		    || y >= 368 || y + (int)h * 8 < 128) continue;
		if (p->draw_source > sizeof(p->page) - size_bytes || p->draw_vram > 65536 - size_bytes) return;
		uint64_t ptr = (uint64_t)p->tile_table + tile * 4;
		if (ptr + 4 > md->rom_size) continue;
		ptr = (uint64_t)p->tile_table + address32(md->rom + ptr);
		if (ptr >= md->rom_size) continue;
		uint8_t tiles[65536];
		size_t bytes = unpack(md, ptr, tiles, sizeof(tiles));
		if (offset + size_bytes > bytes) continue;
		memcpy(p->page + p->draw_source, tiles + offset, size_bytes);
		transfer(p->page + 0x1400 + dma * 16, p->draw_source, p->draw_vram, size_bytes);
		uint8_t *sat = p->page + 0xb00 + count * 8;
		unsigned palette = (flags & 0x6000) ? flags : attr;
		put(sat, y & 1023);
		put(sat + 2, size << 8 | (count + 1));
		put(sat + 4, ((flags | attr) & 0x8000) | (palette & 0x6000) | ((flags ^ attr) & 0x1800) | (p->draw_vram >> 5));
		put(sat + 6, left & 511);
		put(p->page + 0x1f16, dma + 1);
		put(p->page + 0x1f18, count + 1);
		p->draw_source += size_bytes;
		p->draw_vram += size_bytes;
	}
}

static bool blank_save(const uint8_t *save) {
	if (save[0] != 0 && save[0] != 0xff) return false;
	for (unsigned i = 1; i < 4 * 0x780; i++)
		if (save[i] != save[0]) return false;
	return true;
}

void karashi_paprium_command(struct karashi_md *md, uint16_t command) {
	struct karashi_paprium *p = &md->paprium;
	uint8_t *args = p->page + 0x1e10;
	unsigned arg = command & 255, a = word(args), b = word(args + 2);
	unsigned count = word(p->page + 0x1f18);
	put(p->page + 0x1fea, command & 0x7fff);
	switch (command >> 8) {
	case 0x84:
		memcpy(p->page + 0x8000, md->rom + 0x8000, 0x8000);
		/* On first use the game formats four banks and selects the mini-game.
		   Supply the cartridge's subsequent boot with the initialized save. */
		p->restart_pending = p->initialized_banks == 15;
		break;
	case 0xad: draw(md, arg); break;
	case 0xae:
		p->draw_source = 0x2000;
		p->draw_vram = 0x200;
		memcpy(p->page + 0x1f20, p->page + 0xb00, 112);
		transfer(p->page + 0x1400, 0xb00, 0xf000, 640);
		put(p->page + 0x1f16, 1);
		break;
	case 0xaf:
		if (count && count <= 80) p->page[0xb03 + (count - 1) * 8] = 0;
		if (arg == 2) put(p->page + 0x1f1c, 1);
		/* fall through */
	case 0xb1: if (!count) memset(p->page + 0xb00, 0, 8); break;
	case 0xb0: memset(p->page + 0x1f20, 0, 112); break;
	case 0xc6:
		p->object_table = address32(args + 20);
		p->tile_table = address32(args + 24);
		objects(md);
		break;
	case 0xda:
		p->mode = arg;
		p->cursor = a;
		p->remaining = unpack(md, address32(args + 2), p->decoded + a, sizeof(p->decoded) - a);
		break;
	case 0xdb:
		p->cursor = b;
		p->remaining = word(args + 4);
		if (p->remaining > sizeof(p->decoded) - b) p->remaining = sizeof(p->decoded) - b;
		break;
	case 0xdf:
		if (arg >= 1 && arg <= 4 && a <= 65536 - 0x780)
			memcpy(p->page + a, md->sram + (arg - 1) * 0x780, 0x780);
		break;
	case 0xe0:
		if (arg >= 1 && arg <= 4 && a == 0xbeef && b <= 65536 - 0x780) {
			if (p->initialized_banks || blank_save(md->sram)) p->initialized_banks |= 1u << (arg - 1);
			memcpy(md->sram + (arg - 1) * 0x780, p->page + b, 0x780);
		}
		break;
	case 0xf4: unpack(md, address32(args), p->texture, sizeof(p->texture)); break;
	case 0xf5: {
		unsigned scale = word(args + 4);
		if (!scale) break;
		for (unsigned x = 0; x < 128; x++) {
			unsigned sx = x >= a ? word(args + 6) + (x - a) * scale / 64 : 64;
			for (unsigned y = 0; y < 64; y += 2) {
				unsigned pixel = 0;
				if (sx < 64 && x < b) {
					unsigned pair = p->texture[sx * 32 + y / 2];
					pixel = pair >> 4;
				}
				unsigned dst = (x & 4 ? 0x600 : 0x200) + x / 8 * 64 + (x & 2) / 2 + y;
				if (x & 1) p->page[dst] |= pixel;
				else p->page[dst] = pixel << 4;
			}
		}
		break;
	}
	}
}

void karashi_paprium_save(const struct karashi_md *md, struct karashi_state_writer *s) {
	const struct karashi_paprium *p = &md->paprium;
	karashi_state_write_bytes(s, p->page, sizeof(p->page));
	karashi_state_write_bytes(s, p->decoded, sizeof(p->decoded));
	karashi_state_write_bytes(s, p->texture, sizeof(p->texture));
	karashi_state_write_u32(s, p->cursor);
	karashi_state_write_u32(s, p->remaining);
	karashi_state_write_u32(s, p->object_table);
	karashi_state_write_u32(s, p->tile_table);
	karashi_state_write_u32(s, p->draw_source);
	karashi_state_write_u32(s, p->draw_vram);
	karashi_state_write_u8(s, p->control);
	karashi_state_write_u8(s, p->mode);
	karashi_state_write_u8(s, p->initialized_banks);
	karashi_state_write_bool(s, p->restart_pending);
	for (unsigned i = 0; i < 256; i++)
		karashi_state_write_u32(s, p->frame[i]);
}

bool karashi_paprium_load(struct karashi_md *md, struct karashi_state_reader *s) {
	struct karashi_paprium *p = &md->paprium;
	karashi_state_read_bytes(s, p->page, sizeof(p->page));
	karashi_state_read_bytes(s, p->decoded, sizeof(p->decoded));
	karashi_state_read_bytes(s, p->texture, sizeof(p->texture));
	karashi_state_read_u32(s, &p->cursor);
	karashi_state_read_u32(s, &p->remaining);
	karashi_state_read_u32(s, &p->object_table);
	karashi_state_read_u32(s, &p->tile_table);
	karashi_state_read_u32(s, &p->draw_source);
	karashi_state_read_u32(s, &p->draw_vram);
	karashi_state_read_u8(s, &p->control);
	karashi_state_read_u8(s, &p->mode);
	karashi_state_read_u8(s, &p->initialized_banks);
	karashi_state_read_bool(s, &p->restart_pending);
	for (unsigned i = 0; i < 256; i++)
		karashi_state_read_u32(s, &p->frame[i]);
	if (s->failed || p->cursor > sizeof(p->decoded) || p->remaining > sizeof(p->decoded) - p->cursor) return false;
	objects(md);
	return true;
}
