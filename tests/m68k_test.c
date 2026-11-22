#include "m68k.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
	FLAG_C = 0x0001,
	FLAG_V = 0x0002,
	FLAG_Z = 0x0004,
	FLAG_N = 0x0008,
	FLAG_X = 0x0010
};

struct fixture {
	uint8_t memory[1 << 20];
};

static uint8_t read_memory(void *context, uint32_t address) {
	struct fixture *fixture = context;
	return fixture->memory[address & (sizeof(fixture->memory) - 1)];
}

static void write_memory(void *context, uint32_t address, uint8_t value) {
	struct fixture *fixture = context;
	fixture->memory[address & (sizeof(fixture->memory) - 1)] = value;
}

static void put16(struct fixture *fixture, uint32_t address, uint16_t value) {
	fixture->memory[address] = (uint8_t) (value >> 8);
	fixture->memory[address + 1] = (uint8_t) value;
}

static void put32(struct fixture *fixture, uint32_t address, uint32_t value) {
	put16(fixture, address, (uint16_t) (value >> 16));
	put16(fixture, address + 2, (uint16_t) value);
}

static void initialise(struct karashi_m68k *cpu, struct fixture *fixture) {
	struct karashi_m68k_bus bus;

	memset(fixture, 0, sizeof(*fixture));
	memset(&bus, 0, sizeof(bus));
	put32(fixture, 0, 0x00080000);
	put32(fixture, 4, 0x00000100);
	bus.context = fixture;
	bus.read = read_memory;
	bus.write = write_memory;
	karashi_m68k_init(cpu, &bus);
}

static void test_reset_move_and_flags(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	assert(cpu.a[7] == 0x00080000);
	assert(cpu.pc == 0x100);
	assert(cpu.sr == 0x2700);
	put16(&fixture, 0x100, 0x70ff);
	put16(&fixture, 0x102, 0x223c);
	put32(&fixture, 0x104, 0x12345678);
	put16(&fixture, 0x108, 0x3401);
	put16(&fixture, 0x10a, 0x1601);
	assert(karashi_m68k_step(&cpu) == 4);
	assert(cpu.d[0] == UINT32_MAX);
	assert((cpu.sr & (FLAG_N | FLAG_Z | FLAG_V | FLAG_C)) == FLAG_N);
	assert(karashi_m68k_step(&cpu) == 12);
	assert(cpu.d[1] == 0x12345678);
	karashi_m68k_step(&cpu);
	assert(cpu.d[2] == 0x00005678);
	karashi_m68k_step(&cpu);
	assert(cpu.d[3] == 0x00000078);
}

static void test_effective_addresses(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.a[0] = 0x400;
	put16(&fixture, 0x100, 0x20bc);
	put32(&fixture, 0x102, 0x89abcdef);
	put16(&fixture, 0x106, 0x2258);
	put16(&fixture, 0x108, 0x3428);
	put16(&fixture, 0x10a, 0xfffe);
	put16(&fixture, 0x10c, 0x11c2);
	put16(&fixture, 0x10e, 0x0500);
	karashi_m68k_step(&cpu);
	assert(fixture.memory[0x400] == 0x89 && fixture.memory[0x403] == 0xef);
	karashi_m68k_step(&cpu);
	assert(cpu.a[1] == 0x89abcdef);
	assert(cpu.a[0] == 0x404);
	put16(&fixture, 0x402, 0xa55a);
	karashi_m68k_step(&cpu);
	assert((cpu.d[2] & 0xffff) == 0xa55a);
	karashi_m68k_step(&cpu);
	assert(fixture.memory[0x500] == 0x5a);
}

static void test_arithmetic_and_conditions(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x707f);
	put16(&fixture, 0x102, 0x0600);
	put16(&fixture, 0x104, 0x0001);
	put16(&fixture, 0x106, 0x6602);
	put16(&fixture, 0x108, 0x7201);
	put16(&fixture, 0x10a, 0x0440);
	put16(&fixture, 0x10c, 0x0080);
	put16(&fixture, 0x10e, 0x6702);
	put16(&fixture, 0x110, 0x7202);
	put16(&fixture, 0x112, 0x7203);
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	assert((cpu.d[0] & 0xff) == 0x80);
	assert((cpu.sr & (FLAG_N | FLAG_V | FLAG_C)) == (FLAG_N | FLAG_V));
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x10a);
	karashi_m68k_step(&cpu);
	assert((cpu.d[0] & 0xffff) == 0);
	assert((cpu.sr & FLAG_Z) != 0);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x112);
	karashi_m68k_step(&cpu);
	assert(cpu.d[1] == 3);
}

static void test_subroutine_stack_and_control_addresses(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x4eb9);
	put32(&fixture, 0x102, 0x00000120);
	put16(&fixture, 0x106, 0x4ef9);
	put32(&fixture, 0x108, 0x00000130);
	put16(&fixture, 0x120, 0x7433);
	put16(&fixture, 0x122, 0x4e75);
	put16(&fixture, 0x130, 0x4e71);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x120);
	assert(cpu.a[7] == 0x7fffc);
	assert(fixture.memory[0x7ffff] == 0x06);
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x106 && cpu.d[2] == 0x33);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x130);
}

static void test_shifts_and_dbcc(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x203c);
	put32(&fixture, 0x102, 0x800000ff);
	put16(&fixture, 0x106, 0xe280);
	put16(&fixture, 0x108, 0xec08);
	put16(&fixture, 0x10a, 0x7202);
	put16(&fixture, 0x10c, 0x51c9);
	put16(&fixture, 0x10e, 0xfffe);
	put16(&fixture, 0x110, 0x4e71);
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0xc000007f);
	assert((cpu.sr & (FLAG_N | FLAG_V | FLAG_C)) == (FLAG_N | FLAG_C));
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0xc0000001);
	assert((cpu.sr & (FLAG_N | FLAG_Z | FLAG_V | FLAG_C | FLAG_X)) == (FLAG_C | FLAG_X));
	karashi_m68k_step(&cpu);
	assert(cpu.d[1] == 2);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x10c && (cpu.d[1] & 0xffff) == 1);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x10c && (cpu.d[1] & 0xffff) == 0);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x110 && (cpu.d[1] & 0xffff) == 0xffff);
}

static void test_bit_operations(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x7003);
	put16(&fixture, 0x102, 0x7200);
	put16(&fixture, 0x104, 0x0380);
	put16(&fixture, 0x106, 0x08c0);
	put16(&fixture, 0x108, 0x0007);
	put16(&fixture, 0x10a, 0x0840);
	put16(&fixture, 0x10c, 0x0007);
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 2);
	assert((cpu.sr & FLAG_Z) == 0);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0x82);
	assert((cpu.sr & FLAG_Z) != 0);
	karashi_m68k_step(&cpu);
	assert((cpu.sr & FLAG_Z) == 0);
}

static void test_status_immediate_and_movem(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x007c);
	put16(&fixture, 0x102, 0x001f);
	put16(&fixture, 0x104, 0x023c);
	put16(&fixture, 0x106, 0x0004);
	put16(&fixture, 0x108, 0x48e7);
	put16(&fixture, 0x10a, 0xc080);
	put16(&fixture, 0x10c, 0x4cdf);
	put16(&fixture, 0x10e, 0x020c);
	put16(&fixture, 0x110, 0x4884);
	put16(&fixture, 0x112, 0x48c5);
	cpu.d[0] = 0x11223344;
	cpu.d[1] = 0x55667788;
	cpu.a[0] = 0x99aabbcc;
	karashi_m68k_step(&cpu);
	assert((cpu.sr & 0x001f) == 0x001f);
	karashi_m68k_step(&cpu);
	assert((cpu.sr & 0x001f) == 0x0004);
	karashi_m68k_step(&cpu);
	assert(cpu.a[7] == 0x7fff4);
	assert(fixture.memory[0x7fff4] == 0x11 && fixture.memory[0x7fff7] == 0x44);
	assert(fixture.memory[0x7fff8] == 0x55 && fixture.memory[0x7fffb] == 0x88);
	assert(fixture.memory[0x7fffc] == 0x99 && fixture.memory[0x7ffff] == 0xcc);
	karashi_m68k_step(&cpu);
	assert(cpu.d[2] == 0x11223344);
	assert(cpu.d[3] == 0x55667788);
	assert(cpu.a[1] == 0x99aabbcc);
	assert(cpu.a[7] == 0x80000);
	cpu.d[4] = 0x12340080;
	cpu.d[5] = 0x00008000;
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	assert(cpu.d[4] == 0x1234ff80);
	assert(cpu.d[5] == 0xffff8000);
}

static void test_interrupt_and_rte(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put32(&fixture, (24 + 6) * 4, 0x00000200);
	put16(&fixture, 0x100, 0x46fc);
	put16(&fixture, 0x102, 0x2000);
	put16(&fixture, 0x104, 0x4e71);
	put16(&fixture, 0x200, 0x4e73);
	karashi_m68k_step(&cpu);
	assert(cpu.sr == 0x2000);
	karashi_m68k_irq(&cpu, 6);
	assert(karashi_m68k_step(&cpu) == 44);
	assert(cpu.pc == 0x200);
	assert(cpu.a[7] == 0x7fffa);
	assert(cpu.sr == 0x2600);
	assert(fixture.memory[0x7fffa] == 0x20 && fixture.memory[0x7fffb] == 0x00);
	assert(fixture.memory[0x7ffff] == 0x04);
	assert(karashi_m68k_step(&cpu) == 20);
	assert(cpu.pc == 0x104);
	assert(cpu.a[7] == 0x80000);
	assert(cpu.sr == 0x2000);
}

static void test_not_and_neg(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x4640);
	put16(&fixture, 0x102, 0x4400);
	cpu.d[0] = 0x12345678;
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0x1234a987);
	assert((cpu.sr & (FLAG_N | FLAG_Z | FLAG_V | FLAG_C)) == FLAG_N);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0x1234a979);
	assert((cpu.sr & (FLAG_N | FLAG_Z | FLAG_V | FLAG_C | FLAG_X)) == (FLAG_C | FLAG_X));
}

static void test_scc_usp_compare_and_multiply_divide(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x50c0);
	put16(&fixture, 0x102, 0x51c1);
	put16(&fixture, 0x104, 0x4e60);
	put16(&fixture, 0x106, 0x4e69);
	put16(&fixture, 0x108, 0xb088);
	put16(&fixture, 0x10a, 0xc0fc);
	put16(&fixture, 0x10c, 0x0007);
	put16(&fixture, 0x10e, 0x80fc);
	put16(&fixture, 0x110, 0x0006);
	put16(&fixture, 0x112, 0xc1fc);
	put16(&fixture, 0x114, 0xfffe);
	put16(&fixture, 0x116, 0x81fc);
	put16(&fixture, 0x118, 0xfffd);
	put16(&fixture, 0x11a, 0x81fc);
	put16(&fixture, 0x11c, 0xffff);
	cpu.d[0] = 0x12340000;
	cpu.a[0] = 42;
	cpu.a[1] = 0;
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0x123400ff);
	karashi_m68k_step(&cpu);
	assert(cpu.d[1] == 0);
	karashi_m68k_step(&cpu);
	assert(cpu.usp == 42);
	karashi_m68k_step(&cpu);
	assert(cpu.a[1] == 42);
	cpu.d[0] = 42;
	karashi_m68k_step(&cpu);
	assert((cpu.sr & FLAG_Z) != 0);
	cpu.d[0] = 6;
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 42);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 7);
	assert((cpu.sr & (FLAG_N | FLAG_Z | FLAG_V | FLAG_C)) == 0);
	cpu.d[0] = 3;
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == UINT32_C(0xfffffffa));
	cpu.d[0] = (uint32_t) -12;
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 4);
	cpu.d[0] = UINT32_C(0x80000000);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == UINT32_C(0x80000000));
	assert((cpu.sr & FLAG_V) != 0);
}

static void test_cmpm_trap_and_rtr(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0xb149);
	put16(&fixture, 0x102, 0x4e43);
	put32(&fixture, (32 + 3) * 4, 0x00000200);
	put16(&fixture, 0x200, 0x4e77);
	put16(&fixture, 0x300, 0x1234);
	put16(&fixture, 0x400, 0x1234);
	cpu.a[0] = 0x300;
	cpu.a[1] = 0x400;
	karashi_m68k_step(&cpu);
	assert(cpu.a[0] == 0x302 && cpu.a[1] == 0x402);
	assert((cpu.sr & FLAG_Z) != 0);
	assert(karashi_m68k_step(&cpu) == 34);
	assert(cpu.pc == 0x200 && cpu.a[7] == 0x7fffa);
	assert(karashi_m68k_step(&cpu) == 20);
	assert(cpu.pc == 0x104 && cpu.a[7] == 0x80000);
}

static void test_exg_and_pc_relative_movem(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0xc149);
	put16(&fixture, 0x102, 0xc188);
	put16(&fixture, 0x104, 0x4cba);
	put16(&fixture, 0x106, 0x0001);
	put16(&fixture, 0x108, 0x00f8);
	put16(&fixture, 0x200, 0x8001);
	cpu.d[0] = 0x11111111;
	cpu.a[0] = 0x22222222;
	cpu.a[1] = 0x33333333;
	karashi_m68k_step(&cpu);
	assert(cpu.a[0] == 0x33333333 && cpu.a[1] == 0x22222222);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0x33333333 && cpu.a[0] == 0x11111111);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 0xffff8001);
}

static void test_negx_and_memory_shift(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x4044);
	put16(&fixture, 0x102, 0xe5f8);
	put16(&fixture, 0x104, 0x0300);
	put16(&fixture, 0x300, 0x8000);
	cpu.d[4] = 1;
	cpu.sr |= FLAG_X | FLAG_Z;
	karashi_m68k_step(&cpu);
	assert((cpu.d[4] & 0xffff) == 0xfffe);
	assert((cpu.sr & (FLAG_X | FLAG_N | FLAG_Z | FLAG_V | FLAG_C)) == (FLAG_X | FLAG_N | FLAG_C));
	karashi_m68k_step(&cpu);
	assert(fixture.memory[0x300] == 0x00 && fixture.memory[0x301] == 0x01);
	assert((cpu.sr & (FLAG_X | FLAG_N | FLAG_Z | FLAG_V | FLAG_C)) == (FLAG_X | FLAG_C));
}

static void test_alignment_illegal_and_state(void) {
	struct karashi_m68k cpu;
	struct karashi_m68k restored;
	struct fixture fixture;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	uint8_t state[512];
	size_t state_size;

	initialise(&cpu, &fixture);
	put32(&fixture, 3 * 4, 0x00000300);
	put32(&fixture, 4 * 4, 0x00000200);
	put32(&fixture, 11 * 4, 0x00000280);
	put16(&fixture, 0x100, 0xffff);
	assert(karashi_m68k_step(&cpu) == 34);
	assert(cpu.fault == KARASHI_M68K_FAULT_NONE);
	assert(cpu.pc == 0x280 && cpu.a[7] == 0x0007fffa && cpu.illegal_count == 1);
	assert(fixture.memory[0x7fffe] == 0x01 && fixture.memory[0x7ffff] == 0x00);
	cpu.pc = 0x100;
	put16(&fixture, 0x100, 0x4afc);
	karashi_m68k_step(&cpu);
	assert(cpu.pc == 0x200);
	cpu.pc = 0x101;
	assert(karashi_m68k_step(&cpu) == 50);
	assert(cpu.pc == 0x300 && cpu.address_error_count == 1);
	assert(cpu.fault == KARASHI_M68K_FAULT_NONE);
	put32(&fixture, 3 * 4, 0x00000301);
	cpu.pc = 0x101;
	karashi_m68k_step(&cpu);
	assert(cpu.fault == KARASHI_M68K_FAULT_ADDRESS);
	assert(karashi_m68k_step(&cpu) == 0);

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_m68k_save(&cpu, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	memset(&restored, 0, sizeof(restored));
	restored.bus = cpu.bus;
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_m68k_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.pc == cpu.pc);
	assert(restored.a[7] == cpu.a[7]);
	assert(restored.sr == cpu.sr);
	assert(restored.fault == cpu.fault);
}

static void test_prefetch_and_address_frame(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;
	uint32_t stack;

	initialise(&cpu, &fixture);
	put32(&fixture, 3 * 4, 0x00000300);
	put16(&fixture, 0x100, 0x20bc);
	put32(&fixture, 0x102, 0x4e714e71);
	put16(&fixture, 0x106, 0x7001);
	put16(&fixture, 0x108, 0x7202);
	cpu.a[0] = 0x106;
	karashi_m68k_step(&cpu);
	assert(fixture.memory[0x106] == 0x4e && fixture.memory[0x107] == 0x71);
	karashi_m68k_step(&cpu);
	karashi_m68k_step(&cpu);
	assert(cpu.d[0] == 1 && cpu.d[1] == 2);

	initialise(&cpu, &fixture);
	put32(&fixture, 3 * 4, 0x00000300);
	put16(&fixture, 0x100, 0x3010);
	cpu.a[0] = 0x301;
	assert(karashi_m68k_step(&cpu) == 50);
	stack = cpu.a[7];
	assert(stack == 0x0007fff2);
	assert(fixture.memory[stack] == 0x30 && fixture.memory[stack + 1] == 0x15);
	assert(fixture.memory[stack + 2] == 0x00 && fixture.memory[stack + 3] == 0x00);
	assert(fixture.memory[stack + 4] == 0x03 && fixture.memory[stack + 5] == 0x01);
	assert(fixture.memory[stack + 6] == 0x30 && fixture.memory[stack + 7] == 0x10);
	assert(fixture.memory[stack + 8] == 0x27 && fixture.memory[stack + 9] == 0x00);
	assert(fixture.memory[stack + 10] == 0x00 && fixture.memory[stack + 11] == 0x00);
	assert(fixture.memory[stack + 12] == 0x01 && fixture.memory[stack + 13] == 0x02);
}

static void test_chk_timing_paths(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put32(&fixture, 6 * 4, 0x00000300);
	put16(&fixture, 0x100, 0x4380);
	cpu.d[0] = 0x5836;
	cpu.d[1] = 0xa167;
	assert(karashi_m68k_step(&cpu) == 38);
	assert((cpu.sr & FLAG_N) != 0);

	initialise(&cpu, &fixture);
	put32(&fixture, 6 * 4, 0x00000300);
	put16(&fixture, 0x100, 0x4380);
	cpu.d[0] = 0xdeba;
	cpu.d[1] = 0x8dc1;
	assert(karashi_m68k_step(&cpu) == 40);
	assert((cpu.sr & FLAG_N) != 0);

	initialise(&cpu, &fixture);
	put32(&fixture, 6 * 4, 0x00000300);
	put16(&fixture, 0x100, 0x4380);
	cpu.d[0] = 100;
	cpu.d[1] = 200;
	assert(karashi_m68k_step(&cpu) == 38);
	assert((cpu.sr & FLAG_N) == 0);

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x4380);
	cpu.d[0] = 200;
	cpu.d[1] = 100;
	assert(karashi_m68k_step(&cpu) == 10);
}

static void test_stop_scheduling(void) {
	struct karashi_m68k cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x4e72); /* STOP with interrupt mask 4 */
	put16(&fixture, 0x102, 0x2400);
	put32(&fixture, (24 + 6) * 4, 0x200);
	karashi_m68k_set_irq(&cpu, 4);
	karashi_m68k_run(&cpu, 4);
	assert(cpu.stopped && cpu.cycles == 4);
	karashi_m68k_run(&cpu, 101);
	assert(cpu.stopped && cpu.cycles == 104 && cpu.pc == 0x104);
	karashi_m68k_set_irq(&cpu, 6);
	karashi_m68k_run(&cpu, 105);
	assert(!cpu.stopped && cpu.cycles == 148 && cpu.pc == 0x200);

	/* STOP can unmask an IRQ that was pending before the instruction. */
	karashi_m68k_reset(&cpu);
	karashi_m68k_set_irq(&cpu, 6);
	karashi_m68k_run(&cpu, 48);
	assert(!cpu.stopped && cpu.cycles == 48 && cpu.pc == 0x200);
}

static void test_run_loop_skip(void) {
	struct karashi_m68k cpu;
	struct karashi_m68k reference;
	struct fixture fixture;
	struct fixture reference_fixture;

	initialise(&cpu, &fixture);
	initialise(&reference, &reference_fixture);
	put16(&fixture, 0x100, 0x51c8);
	put16(&fixture, 0x102, 0xfffc);
	put16(&reference_fixture, 0x100, 0x51c8);
	put16(&reference_fixture, 0x102, 0xfffc);
	cpu.bus.read_map[0] = fixture.memory;
	reference.bus.read_map[0] = reference_fixture.memory;
	cpu.d[0] = reference.d[0] = 1000;
	while (reference.cycles < 5000) karashi_m68k_step(&reference);
	karashi_m68k_run(&cpu, 5000);
	assert(cpu.cycles == reference.cycles);
	assert(cpu.pc == reference.pc);
	assert(cpu.d[0] == reference.d[0]);
	assert(cpu.sr == reference.sr);

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0x60fe);
	cpu.bus.read_map[0] = fixture.memory;
	karashi_m68k_run(&cpu, 1000000);
	assert(cpu.pc == 0x100);
	assert(cpu.cycles == 1000000);
}

int main(void) {
	test_reset_move_and_flags();
	test_effective_addresses();
	test_arithmetic_and_conditions();
	test_subroutine_stack_and_control_addresses();
	test_shifts_and_dbcc();
	test_bit_operations();
	test_status_immediate_and_movem();
	test_interrupt_and_rte();
	test_not_and_neg();
	test_scc_usp_compare_and_multiply_divide();
	test_cmpm_trap_and_rtr();
	test_exg_and_pc_relative_movem();
	test_negx_and_memory_shift();
	test_alignment_illegal_and_state();
	test_prefetch_and_address_frame();
	test_chk_timing_paths();
	test_stop_scheduling();
	test_run_loop_skip();
	puts("68000 reset, effective addresses, arithmetic, control flow and state: pass");
	return 0;
}
