#include "state.h"

#include <limits.h>
#include <string.h>

static const uint8_t state_magic[4] = {'K', 'R', 'S', 'H'};

static uint16_t read_u16le(const uint8_t *data) {
	return (uint16_t) data[0] | (uint16_t) data[1] << 8;
}

static uint32_t read_u32le(const uint8_t *data) {
	return (uint32_t) data[0] | (uint32_t) data[1] << 8 | (uint32_t) data[2] << 16 | (uint32_t) data[3] << 24;
}

static void write_u16le(uint8_t *data, uint16_t value) {
	data[0] = (uint8_t) value;
	data[1] = (uint8_t) (value >> 8);
}

static void write_u32le(uint8_t *data, uint32_t value) {
	data[0] = (uint8_t) value;
	data[1] = (uint8_t) (value >> 8);
	data[2] = (uint8_t) (value >> 16);
	data[3] = (uint8_t) (value >> 24);
}

uint32_t karashi_state_crc32(const uint8_t *data, size_t size) {
	static uint32_t table[256];
	uint32_t crc = UINT32_MAX;
	size_t i;

	if (!table[1]) {
		uint32_t entry;
		unsigned bit;
		for (i = 0; i < 256; i++) {
			entry = (uint32_t) i;
			for (bit = 0; bit < 8; bit++) entry = entry >> 1 ^ (UINT32_C(0xedb88320) & (uint32_t) -(int32_t) (entry & 1));
			table[i] = entry;
		}
	}

	for (i = 0; i < size; i++) crc = crc >> 8 ^ table[(crc ^ data[i]) & 0xff];
	return ~crc;
}

static bool writer_reserve(struct karashi_state_writer *writer, size_t size) {
	if (!writer || writer->failed || writer->position > writer->capacity || size > writer->capacity - writer->position) {
		if (writer) writer->failed = true;
		return false;
	}
	return true;
}

static bool reader_reserve(struct karashi_state_reader *reader, size_t size) {
	if (!reader || reader->failed || reader->position > reader->size || size > reader->size - reader->position) {
		if (reader) reader->failed = true;
		return false;
	}
	return true;
}

bool karashi_state_writer_begin(
	struct karashi_state_writer *writer, void *data, size_t capacity, uint16_t system, uint16_t version
) {
	if (!writer || !data || capacity < KARASHI_STATE_HEADER_SIZE) return false;
	writer->data = data;
	writer->capacity = capacity;
	writer->position = KARASHI_STATE_HEADER_SIZE;
	writer->failed = false;
	memcpy(writer->data, state_magic, sizeof(state_magic));
	write_u16le(writer->data + 4, version);
	write_u16le(writer->data + 6, system);
	write_u32le(writer->data + 8, 0);
	write_u32le(writer->data + 12, 0);
	return true;
}

bool karashi_state_writer_finish(struct karashi_state_writer *writer, size_t *size) {
	size_t payload_size;

	if (!writer || writer->failed || writer->position < KARASHI_STATE_HEADER_SIZE) return false;
	payload_size = writer->position - KARASHI_STATE_HEADER_SIZE;
	if (payload_size > UINT32_MAX) return false;
	write_u32le(writer->data + 8, (uint32_t) payload_size);
	write_u32le(
		writer->data + 12, karashi_state_crc32(writer->data + KARASHI_STATE_HEADER_SIZE, payload_size)
	);
	if (size) *size = writer->position;
	return true;
}

bool karashi_state_write_u8(struct karashi_state_writer *writer, uint8_t value) {
	if (!writer_reserve(writer, 1)) return false;
	writer->data[writer->position++] = value;
	return true;
}

bool karashi_state_write_u16(struct karashi_state_writer *writer, uint16_t value) {
	if (!writer_reserve(writer, 2)) return false;
	write_u16le(writer->data + writer->position, value);
	writer->position += 2;
	return true;
}

bool karashi_state_write_u32(struct karashi_state_writer *writer, uint32_t value) {
	if (!writer_reserve(writer, 4)) return false;
	write_u32le(writer->data + writer->position, value);
	writer->position += 4;
	return true;
}

bool karashi_state_write_u64(struct karashi_state_writer *writer, uint64_t value) {
	return karashi_state_write_u32(writer, (uint32_t) value)
	       && karashi_state_write_u32(writer, (uint32_t) (value >> 32));
}

bool karashi_state_write_bool(struct karashi_state_writer *writer, bool value) {
	return karashi_state_write_u8(writer, value ? 1 : 0);
}

bool karashi_state_write_bytes(struct karashi_state_writer *writer, const void *data, size_t size) {
	if ((size && !data) || !writer_reserve(writer, size)) return false;
	memcpy(writer->data + writer->position, data, size);
	writer->position += size;
	return true;
}

bool karashi_state_reader_begin(struct karashi_state_reader *reader, const void *data, size_t size) {
	const uint8_t *bytes = data;
	uint32_t payload_size;
	uint32_t payload_crc;

	if (!reader || !data || size < KARASHI_STATE_HEADER_SIZE) return false;
	if (memcmp(bytes, state_magic, sizeof(state_magic)) != 0) return false;
	payload_size = read_u32le(bytes + 8);
	payload_crc = read_u32le(bytes + 12);
	if ((size_t) payload_size != size - KARASHI_STATE_HEADER_SIZE) return false;
	if (karashi_state_crc32(bytes + KARASHI_STATE_HEADER_SIZE, payload_size) != payload_crc) return false;

	reader->data = bytes;
	reader->size = size;
	reader->position = KARASHI_STATE_HEADER_SIZE;
	reader->version = read_u16le(bytes + 4);
	reader->system = read_u16le(bytes + 6);
	reader->failed = false;
	return true;
}

bool karashi_state_read_u8(struct karashi_state_reader *reader, uint8_t *value) {
	if (!value || !reader_reserve(reader, 1)) return false;
	*value = reader->data[reader->position++];
	return true;
}

bool karashi_state_read_u16(struct karashi_state_reader *reader, uint16_t *value) {
	if (!value || !reader_reserve(reader, 2)) return false;
	*value = read_u16le(reader->data + reader->position);
	reader->position += 2;
	return true;
}

bool karashi_state_read_u32(struct karashi_state_reader *reader, uint32_t *value) {
	if (!value || !reader_reserve(reader, 4)) return false;
	*value = read_u32le(reader->data + reader->position);
	reader->position += 4;
	return true;
}

bool karashi_state_read_u64(struct karashi_state_reader *reader, uint64_t *value) {
	uint32_t low;
	uint32_t high;

	if (!value || !karashi_state_read_u32(reader, &low) || !karashi_state_read_u32(reader, &high)) return false;
	*value = (uint64_t) low | (uint64_t) high << 32;
	return true;
}

bool karashi_state_read_bool(struct karashi_state_reader *reader, bool *value) {
	uint8_t byte;

	if (!value || !karashi_state_read_u8(reader, &byte) || byte > 1) {
		if (reader) reader->failed = true;
		return false;
	}
	*value = byte != 0;
	return true;
}

bool karashi_state_read_bytes(struct karashi_state_reader *reader, void *data, size_t size) {
	if ((size && !data) || !reader_reserve(reader, size)) return false;
	memcpy(data, reader->data + reader->position, size);
	reader->position += size;
	return true;
}

bool karashi_state_reader_finished(const struct karashi_state_reader *reader) {
	return reader && !reader->failed && reader->position == reader->size;
}
