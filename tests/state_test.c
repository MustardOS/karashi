#include "state.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect_true(const char *name, int value) {
	if (value) return;
	fprintf(stderr, "%s: expected true\n", name);
	failures++;
}

static void expect_false(const char *name, int value) {
	if (!value) return;
	fprintf(stderr, "%s: expected false\n", name);
	failures++;
}

static void expect_u64(const char *name, uint64_t actual, uint64_t expected) {
	if (actual == expected) return;
	fprintf(stderr, "%s: expected %" PRIu64 ", got %" PRIu64 "\n", name, expected, actual);
	failures++;
}

int main(void) {
	uint8_t state[64];
	uint8_t bytes[3];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;
	uint8_t value8;
	uint16_t value16;
	uint32_t value32;
	uint64_t value64;

	expect_true("begin writer", karashi_state_writer_begin(&writer, state, sizeof(state), 4, 7));
	expect_true("write u8", karashi_state_write_u8(&writer, UINT8_C(0xa5)));
	expect_true("write u16", karashi_state_write_u16(&writer, UINT16_C(0x1234)));
	expect_true("write u32", karashi_state_write_u32(&writer, UINT32_C(0x89abcdef)));
	expect_true("write u64", karashi_state_write_u64(&writer, UINT64_C(0x0123456789abcdef)));
	expect_true("write bytes", karashi_state_write_bytes(&writer, "KRS", 3));
	expect_true("finish writer", karashi_state_writer_finish(&writer, &size));

	expect_true("begin reader", karashi_state_reader_begin(&reader, state, size));
	expect_u64("system", reader.system, 4);
	expect_u64("version", reader.version, 7);
	expect_true("read u8", karashi_state_read_u8(&reader, &value8));
	expect_true("read u16", karashi_state_read_u16(&reader, &value16));
	expect_true("read u32", karashi_state_read_u32(&reader, &value32));
	expect_true("read u64", karashi_state_read_u64(&reader, &value64));
	expect_true("read bytes", karashi_state_read_bytes(&reader, bytes, sizeof(bytes)));
	expect_u64("u8 value", value8, UINT8_C(0xa5));
	expect_u64("u16 value", value16, UINT16_C(0x1234));
	expect_u64("u32 value", value32, UINT32_C(0x89abcdef));
	expect_u64("u64 value", value64, UINT64_C(0x0123456789abcdef));
	expect_true("byte value", memcmp(bytes, "KRS", 3) == 0);
	expect_true("reader finished", karashi_state_reader_finished(&reader));

	state[size - 1] ^= 1;
	expect_false("crc rejects mutation", karashi_state_reader_begin(&reader, state, size));
	state[size - 1] ^= 1;
	expect_false("length rejects truncation", karashi_state_reader_begin(&reader, state, size - 1));

	if (failures) return 1;
	puts("state container: pass");
	return 0;
}
