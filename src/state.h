#ifndef KARASHI_STATE_H
#define KARASHI_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KARASHI_STATE_HEADER_SIZE 16

struct karashi_state_writer {
	uint8_t *data;
	size_t capacity;
	size_t position;
	bool failed;
};

struct karashi_state_reader {
	const uint8_t *data;
	size_t size;
	size_t position;
	uint16_t system;
	uint16_t version;
	bool failed;
};

uint32_t karashi_state_crc32(const uint8_t *data, size_t size);
bool karashi_state_writer_begin(
	struct karashi_state_writer *writer, void *data, size_t capacity, uint16_t system, uint16_t version
);
bool karashi_state_writer_finish(struct karashi_state_writer *writer, size_t *size);
bool karashi_state_write_u8(struct karashi_state_writer *writer, uint8_t value);
bool karashi_state_write_u16(struct karashi_state_writer *writer, uint16_t value);
bool karashi_state_write_u32(struct karashi_state_writer *writer, uint32_t value);
bool karashi_state_write_u64(struct karashi_state_writer *writer, uint64_t value);
bool karashi_state_write_bool(struct karashi_state_writer *writer, bool value);
bool karashi_state_write_bytes(struct karashi_state_writer *writer, const void *data, size_t size);

bool karashi_state_reader_begin(struct karashi_state_reader *reader, const void *data, size_t size);
bool karashi_state_read_u8(struct karashi_state_reader *reader, uint8_t *value);
bool karashi_state_read_u16(struct karashi_state_reader *reader, uint16_t *value);
bool karashi_state_read_u32(struct karashi_state_reader *reader, uint32_t *value);
bool karashi_state_read_u64(struct karashi_state_reader *reader, uint64_t *value);
bool karashi_state_read_bool(struct karashi_state_reader *reader, bool *value);
bool karashi_state_read_bytes(struct karashi_state_reader *reader, void *data, size_t size);
bool karashi_state_reader_finished(const struct karashi_state_reader *reader);

#endif
