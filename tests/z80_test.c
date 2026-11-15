#include "z80.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
	FLAG_C = 0x01,
	FLAG_N = 0x02,
	FLAG_PV = 0x04,
	FLAG_X = 0x08,
	FLAG_H = 0x10,
	FLAG_Y = 0x20,
	FLAG_Z = 0x40,
	FLAG_S = 0x80
};

struct fixture {
	uint8_t memory[65536];
	uint8_t input;
	uint16_t output_port;
	uint8_t output;
};

static uint8_t read_memory(void *context, uint16_t address) {
	struct fixture *fixture = context;
	return fixture->memory[address];
}

static void write_memory(void *context, uint16_t address, uint8_t value) {
	struct fixture *fixture = context;
	fixture->memory[address] = value;
}

static uint8_t read_port(void *context, uint16_t port) {
	struct fixture *fixture = context;
	(void) port;
	return fixture->input;
}

static void write_port(void *context, uint16_t port, uint8_t value) {
	struct fixture *fixture = context;
	fixture->output_port = port;
	fixture->output = value;
}

static void initialise(struct karashi_z80 *cpu, struct fixture *fixture) {
	struct karashi_z80_bus bus;

	memset(fixture, 0, sizeof(*fixture));
	bus.context = fixture;
	bus.read = read_memory;
	bus.write = write_memory;
	bus.io_read = read_port;
	bus.io_write = write_port;
	karashi_z80_init(cpu, &bus);
}

static bool parity(uint8_t value) {
	value ^= value >> 4;
	value &= 0x0f;
	return ((0x6996 >> value) & 1) == 0;
}

static uint8_t add_flags(uint8_t left, uint8_t right, unsigned carry) {
	unsigned wide = left + right + carry;
	uint8_t result = (uint8_t) wide;
	uint8_t flags = result & (FLAG_S | FLAG_Y | FLAG_X);

	if (result == 0) flags |= FLAG_Z;
	if ((left ^ right ^ result) & 0x10) flags |= FLAG_H;
	if ((~(left ^ right) & (left ^ result)) & 0x80) flags |= FLAG_PV;
	if (wide > 0xff) flags |= FLAG_C;
	return flags;
}

static uint8_t subtract_flags(uint8_t left, uint8_t right, unsigned carry) {
	unsigned subtrahend = right + carry;
	uint8_t result = (uint8_t) (left - subtrahend);
	uint8_t flags = (result & (FLAG_S | FLAG_Y | FLAG_X)) | FLAG_N;

	if (result == 0) flags |= FLAG_Z;
	if ((left ^ right ^ result) & 0x10) flags |= FLAG_H;
	if (((left ^ right) & (left ^ result)) & 0x80) flags |= FLAG_PV;
	if (left < subtrahend) flags |= FLAG_C;
	return flags;
}

static void test_load_arithmetic_and_halt(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x31, 0xfe, 0xff,
		0x3e, 0x7f,
		0xc6, 0x01,
		0x06, 0x02,
		0x80,
		0x76,
		0x00
	};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	assert(karashi_z80_step(&cpu) == 10);
	assert(cpu.sp == 0xfffe);
	assert(karashi_z80_step(&cpu) == 7);
	assert(karashi_z80_step(&cpu) == 7);
	assert(cpu.af == 0x8094);
	assert(karashi_z80_step(&cpu) == 7);
	assert(karashi_z80_step(&cpu) == 4);
	assert(cpu.af == 0x8280);
	assert(karashi_z80_step(&cpu) == 4);
	assert(cpu.halted);
	assert(cpu.pc == 11);
	assert(karashi_z80_step(&cpu) == 4);
	assert(cpu.pc == 11);
}

static void test_control_flow_and_stack(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x31, 0x00, 0x80,
		0xcd, 0x09, 0x00,
		0xc3, 0x0c, 0x00,
		0x3e, 0x42,
		0xc9,
		0x76
	};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 17);
	assert(cpu.pc == 9);
	assert(cpu.sp == 0x7ffe);
	assert(fixture.memory[0x7ffe] == 6);
	assert(fixture.memory[0x7fff] == 0);
	karashi_z80_step(&cpu);
	assert((cpu.af >> 8) == 0x42);
	assert(karashi_z80_step(&cpu) == 10);
	assert(cpu.pc == 6);
	assert(cpu.sp == 0x8000);
	karashi_z80_step(&cpu);
	assert(cpu.pc == 12);
	karashi_z80_step(&cpu);
	assert(cpu.halted);
}

static void test_relative_jumps(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x18, 0x02,
		0x76, 0x76,
		0x06, 0x03,
		0x10, 0xfe,
		0xaf,
		0x20, 0x7f,
		0x28, 0x01,
		0x76,
		0x18, 0xf0
	};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	assert(karashi_z80_step(&cpu) == 12);
	assert(cpu.pc == 4);
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 13);
	assert(cpu.pc == 6);
	assert(karashi_z80_step(&cpu) == 13);
	assert(karashi_z80_step(&cpu) == 8);
	assert(cpu.pc == 8);
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 7);
	assert(cpu.pc == 11);
	assert(karashi_z80_step(&cpu) == 12);
	assert(cpu.pc == 14);
	assert(karashi_z80_step(&cpu) == 12);
	assert(cpu.pc == 0);
}

static void test_cb_space(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x06, 0x81,
		0xcb, 0x00,
		0xcb, 0x78,
		0xcb, 0xb8,
		0xcb, 0xf8,
		0x21, 0x00, 0x40,
		0xcb, 0x46
	};

	initialise(&cpu, &fixture);
	fixture.memory[0x4000] = 1;
	memcpy(fixture.memory, program, sizeof(program));
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 8);
	assert((cpu.bc >> 8) == 0x03);
	assert((cpu.af & 0xff) == (FLAG_PV | FLAG_C));
	karashi_z80_step(&cpu);
	assert((cpu.af & 0xff) == (FLAG_Z | FLAG_H | FLAG_PV | FLAG_C));
	karashi_z80_step(&cpu);
	assert((cpu.bc >> 8) == 0x03);
	karashi_z80_step(&cpu);
	assert((cpu.bc >> 8) == 0x83);
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 12);
	assert((cpu.af & 0xff) == (FLAG_H | FLAG_C));
}

static void test_io(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x3e, 0x5a,
		0xd3, 0x34,
		0xdb, 0x12
	};

	initialise(&cpu, &fixture);
	fixture.input = 0xa5;
	memcpy(fixture.memory, program, sizeof(program));
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	assert(fixture.output_port == 0x5a34);
	assert(fixture.output == 0x5a);
	karashi_z80_step(&cpu);
	assert((cpu.af >> 8) == 0xa5);
}

static void test_interrupts(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {0xfb, 0x00, 0x00};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	cpu.sp = 0x9000;
	karashi_z80_irq(&cpu, true);
	karashi_z80_step(&cpu);
	assert(cpu.pc == 1);
	assert(karashi_z80_step(&cpu) == 4);
	assert(cpu.pc == 2);
	assert(karashi_z80_step(&cpu) == 13);
	assert(cpu.pc == 0x38);
	assert(cpu.sp == 0x8ffe);
	assert(fixture.memory[0x8ffe] == 2);

	karashi_z80_reset(&cpu);
	cpu.pc = 0x1234;
	cpu.sp = 0x9000;
	cpu.iff1 = true;
	karashi_z80_nmi(&cpu);
	assert(karashi_z80_step(&cpu) == 11);
	assert(cpu.pc == 0x66);
	assert(cpu.iff2);
	assert(!cpu.iff1);
}

static void test_exhaustive_arithmetic(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	unsigned left;
	unsigned right;
	unsigned carry;

	initialise(&cpu, &fixture);
	for (left = 0; left < 256; left++) {
		for (right = 0; right < 256; right++) {
			for (carry = 0; carry < 2; carry++) {
				cpu.pc = 0;
				cpu.af = (uint16_t) (left << 8 | carry);
				fixture.memory[0] = carry ? 0xce : 0xc6;
				fixture.memory[1] = (uint8_t) right;
				karashi_z80_step(&cpu);
				assert((cpu.af >> 8) == (uint8_t) (left + right + carry));
				assert((cpu.af & 0xff) == add_flags((uint8_t) left, (uint8_t) right, carry));

				cpu.pc = 0;
				cpu.af = (uint16_t) (left << 8 | carry);
				fixture.memory[0] = carry ? 0xde : 0xd6;
				karashi_z80_step(&cpu);
				assert((cpu.af >> 8) == (uint8_t) (left - right - carry));
				assert((cpu.af & 0xff) == subtract_flags((uint8_t) left, (uint8_t) right, carry));
			}
		}
	}
}

static void test_logic_flags(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {0x3e, 0xaa, 0xe6, 0x0f, 0xee, 0x0a, 0xf6, 0x80};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	assert(cpu.af == 0x0a1c);
	karashi_z80_step(&cpu);
	assert(cpu.af == 0x0044);
	karashi_z80_step(&cpu);
	assert((cpu.af >> 8) == 0x80);
	assert((cpu.af & 0xff) == (FLAG_S | (parity(0x80) ? FLAG_PV : 0)));
}

static void test_ed_arithmetic_and_control(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x21, 0xff, 0x7f,
		0x01, 0x00, 0x00,
		0x37,
		0xed, 0x4a,
		0x3e, 0x80,
		0xed, 0x44,
		0xed, 0x47,
		0xed, 0x57,
		0xed, 0x5e
	};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 15);
	assert(cpu.hl == 0x8000);
	assert((cpu.af & 0xff) == (FLAG_S | FLAG_H | FLAG_PV));
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 8);
	assert(cpu.af == 0x8087);
	karashi_z80_step(&cpu);
	assert(cpu.i == 0x80);
	cpu.iff2 = true;
	karashi_z80_step(&cpu);
	assert(cpu.af == 0x8085);
	karashi_z80_step(&cpu);
	assert(cpu.interrupt_mode == 2);
}

static void test_ed_nibbles_and_blocks(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0x21, 0x00, 0x40,
		0x11, 0x00, 0x50,
		0x01, 0x03, 0x00,
		0xed, 0xb0,
		0x21, 0x00, 0x60,
		0x3e, 0x12,
		0xed, 0x67,
		0xed, 0x6f
	};

	initialise(&cpu, &fixture);
	fixture.memory[0x4000] = 0x11;
	fixture.memory[0x4001] = 0x22;
	fixture.memory[0x4002] = 0x33;
	fixture.memory[0x6000] = 0x34;
	memcpy(fixture.memory, program, sizeof(program));
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 21);
	assert(karashi_z80_step(&cpu) == 21);
	assert(karashi_z80_step(&cpu) == 16);
	assert(cpu.bc == 0);
	assert(cpu.hl == 0x4003);
	assert(cpu.de == 0x5003);
	assert(memcmp(&fixture.memory[0x4000], &fixture.memory[0x5000], 3) == 0);
	karashi_z80_step(&cpu);
	karashi_z80_step(&cpu);
	assert(karashi_z80_step(&cpu) == 18);
	assert((cpu.af >> 8) == 0x14);
	assert(fixture.memory[0x6000] == 0x23);
	assert(karashi_z80_step(&cpu) == 18);
	assert((cpu.af >> 8) == 0x12);
	assert(fixture.memory[0x6000] == 0x34);
}

static void test_index_registers_and_memory(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0xdd, 0x21, 0x00, 0x40,
		0xdd, 0x36, 0x02, 0x81,
		0xdd, 0x34, 0x02,
		0xdd, 0x46, 0x02,
		0xdd, 0xcb, 0x02, 0x06,
		0xdd, 0xcb, 0x02, 0xc0,
		0xdd, 0xcb, 0x02, 0x7e
	};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	assert(karashi_z80_step(&cpu) == 14);
	assert(cpu.ix == 0x4000);
	assert(karashi_z80_step(&cpu) == 19);
	assert(fixture.memory[0x4002] == 0x81);
	assert(karashi_z80_step(&cpu) == 23);
	assert(fixture.memory[0x4002] == 0x82);
	assert(karashi_z80_step(&cpu) == 19);
	assert((cpu.bc >> 8) == 0x82);
	assert(karashi_z80_step(&cpu) == 23);
	assert(fixture.memory[0x4002] == 0x05);
	assert((cpu.af & 0xff) == (FLAG_PV | FLAG_C));
	assert(karashi_z80_step(&cpu) == 23);
	assert((cpu.bc >> 8) == 0x05);
	assert(karashi_z80_step(&cpu) == 20);
	assert((cpu.af & 0xff) == (FLAG_Z | FLAG_H | FLAG_PV | FLAG_C));
}

static void test_index_prefix_selection_and_fallback(void) {
	struct karashi_z80 cpu;
	struct fixture fixture;
	const uint8_t program[] = {
		0xdd, 0x3e, 0x42,
		0xdd, 0xfd, 0x21, 0x34, 0x12,
		0xfd, 0x26, 0xab,
		0xfd, 0x2e, 0xcd,
		0xdd, 0xed, 0x44
	};

	initialise(&cpu, &fixture);
	memcpy(fixture.memory, program, sizeof(program));
	assert(karashi_z80_step(&cpu) == 11);
	assert((cpu.af >> 8) == 0x42);
	assert(karashi_z80_step(&cpu) == 18);
	assert(cpu.iy == 0x1234);
	assert(karashi_z80_step(&cpu) == 11);
	assert(cpu.iy == 0xab34);
	assert(karashi_z80_step(&cpu) == 11);
	assert(cpu.iy == 0xabcd);
	assert(karashi_z80_step(&cpu) == 12);
	assert(cpu.af == 0xbebb);
}

int main(void) {
	test_load_arithmetic_and_halt();
	test_control_flow_and_stack();
	test_relative_jumps();
	test_cb_space();
	test_io();
	test_interrupts();
	test_exhaustive_arithmetic();
	test_logic_flags();
	test_ed_arithmetic_and_control();
	test_ed_nibbles_and_blocks();
	test_index_registers_and_memory();
	test_index_prefix_selection_and_fallback();
	puts("z80 base, CB, ED, DD and FD instruction spaces: pass");
	return 0;
}
