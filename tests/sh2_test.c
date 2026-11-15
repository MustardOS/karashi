#include "sh2.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fixture {
	uint8_t memory[4096];
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

static uint32_t get32(const struct fixture *fixture, uint32_t address) {
	return (uint32_t) fixture->memory[address] << 24
	       | (uint32_t) fixture->memory[address + 1] << 16
	       | (uint32_t) fixture->memory[address + 2] << 8
	       | fixture->memory[address + 3];
}

static void initialise(struct karashi_sh2 *cpu, struct fixture *fixture) {
	struct karashi_sh2_bus bus;

	memset(fixture, 0, sizeof(*fixture));
	memset(&bus, 0, sizeof(bus));
	put32(fixture, 0, 0x100);
	put32(fixture, 4, 0x800);
	bus.context = fixture;
	bus.read = read_memory;
	bus.write = write_memory;
	karashi_sh2_init(cpu, &bus);
}

static void test_reset_arithmetic_and_compare(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	assert(cpu.pc == 0x100 && cpu.r[15] == 0x800 && cpu.sr == 0xf0);
	put16(&fixture, 0x100, 0xe0ff);
	put16(&fixture, 0x102, 0xe105);
	put16(&fixture, 0x104, 0x301c);
	put16(&fixture, 0x106, 0x7102);
	put16(&fixture, 0x108, 0x3010);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.r[0] == 4 && cpu.r[1] == 7);
	assert((cpu.sr & 1) == 0);
}

static void test_memory_and_sign_extension(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0x300;
	cpu.r[2] = 0x89abcdef;
	put16(&fixture, 0x100, 0x2122);
	put16(&fixture, 0x102, 0x6312);
	put16(&fixture, 0x104, 0x6414);
	fixture.memory[0x300] = 0x80;
	karashi_sh2_step(&cpu);
	assert(fixture.memory[0x300] == 0x89 && fixture.memory[0x303] == 0xef);
	karashi_sh2_step(&cpu);
	assert(cpu.r[3] == 0x89abcdef);
	cpu.r[1] = 0x300;
	karashi_sh2_step(&cpu);
	assert(cpu.r[1] == 0x301 && cpu.r[4] == 0xffffff89);
}

static void test_boot_window_and_cache_alias(void) {
	struct karashi_sh2 cpu;
	struct karashi_sh2_bus bus;
	struct fixture fixture;
	uint8_t boot[2048];

	memset(&fixture, 0, sizeof(fixture));
	memset(&bus, 0, sizeof(bus));
	memset(boot, 0, sizeof(boot));
	boot[2] = 1;
	boot[4] = 0;
	boot[5] = 0;
	boot[6] = 8;
	boot[7] = 0;
	boot[0x100] = 0x62;
	boot[0x101] = 0x11;
	boot[0x102] = 0x62;
	boot[0x103] = 0x11;
	boot[0x20] = 0x56;
	boot[0x21] = 0x78;
	bus.context = &fixture;
	bus.read = read_memory;
	bus.write = write_memory;
	bus.boot = boot;
	bus.boot_mask = sizeof(boot) - 1;
	karashi_sh2_init(&cpu, &bus);
	assert(cpu.pc == 0x100 && cpu.r[15] == 0x800);
	cpu.cache_ram[0] = 0x12;
	cpu.cache_ram[1] = 0x34;
	cpu.r[1] = 0xc0000000;
	karashi_sh2_step(&cpu);
	assert(cpu.r[2] == 0x1234);
	cpu.r[1] = 0x20000020;
	karashi_sh2_step(&cpu);
	assert(cpu.r[2] == 0x5678);
}

static void test_delayed_branch_and_subroutine(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0xe001);
	put16(&fixture, 0x102, 0xa001);
	put16(&fixture, 0x104, 0x7001);
	put16(&fixture, 0x106, 0x7004);
	put16(&fixture, 0x108, 0xb002);
	put16(&fixture, 0x10a, 0x7002);
	put16(&fixture, 0x10c, 0x0009);
	put16(&fixture, 0x110, 0x000b);
	put16(&fixture, 0x112, 0x7008);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x104);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x108 && cpu.r[0] == 2);
	karashi_sh2_step(&cpu);
	assert(cpu.pr == 0x10c && cpu.pc == 0x10a);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x110 && cpu.r[0] == 4);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x10c && cpu.r[0] == 12);
}

static void test_string_multiply_and_tas(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0x11223344;
	cpu.r[2] = 0x55226677;
	put16(&fixture, 0x100, 0x212c);
	put16(&fixture, 0x102, 0x212d);
	put16(&fixture, 0x104, 0x212e);
	cpu.r[3] = 0x380;
	put16(&fixture, 0x106, 0x431b);
	karashi_sh2_step(&cpu);
	assert(cpu.sr & 1);
	karashi_sh2_step(&cpu);
	assert(cpu.r[1] == 0x66771122);
	karashi_sh2_step(&cpu);
	assert(cpu.macl == 0x1122u * 0x6677u);
	karashi_sh2_step(&cpu);
	assert((cpu.sr & 1) && fixture.memory[0x380] == 0x80);
}

static void test_conditional_branches(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put16(&fixture, 0x100, 0xe000);
	put16(&fixture, 0x102, 0x8800);
	put16(&fixture, 0x104, 0x8901);
	put16(&fixture, 0x106, 0x7001);
	put16(&fixture, 0x108, 0x7002);
	put16(&fixture, 0x10a, 0x8d01);
	put16(&fixture, 0x10c, 0x7004);
	put16(&fixture, 0x10e, 0x7008);
	put16(&fixture, 0x110, 0x7010);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x10a);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x10c);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x110 && cpu.r[0] == 4);
	karashi_sh2_step(&cpu);
	assert(cpu.r[0] == 20);
}

static void execute_at_reset(struct karashi_sh2 *cpu, struct fixture *fixture, uint16_t opcode) {
	cpu->pc = 0x100;
	cpu->fault = KARASHI_SH2_FAULT_NONE;
	put16(fixture, 0x100, opcode);
	assert(karashi_sh2_step(cpu) != 0);
}

static void test_carry_overflow_and_long_multiply(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0xffffffff;
	cpu.r[2] = 0;
	cpu.sr = 1;
	execute_at_reset(&cpu, &fixture, 0x321e);
	assert(cpu.r[2] == 0 && (cpu.sr & 1));
	cpu.r[1] = 1;
	cpu.r[2] = 0x7fffffff;
	cpu.sr = 0;
	execute_at_reset(&cpu, &fixture, 0x321f);
	assert(cpu.r[2] == 0x80000000 && (cpu.sr & 1));
	cpu.r[1] = 1;
	cpu.r[2] = 0;
	cpu.sr = 1;
	execute_at_reset(&cpu, &fixture, 0x321a);
	assert(cpu.r[2] == 0xfffffffe && (cpu.sr & 1));
	cpu.r[1] = 1;
	cpu.r[2] = 0x80000000;
	cpu.sr = 0;
	execute_at_reset(&cpu, &fixture, 0x321b);
	assert(cpu.r[2] == 0x7fffffff && (cpu.sr & 1));
	cpu.r[1] = 2;
	cpu.r[2] = 0xffffffff;
	execute_at_reset(&cpu, &fixture, 0x3215);
	assert(cpu.mach == 1 && cpu.macl == 0xfffffffe);
	cpu.r[1] = 3;
	cpu.r[2] = 0xfffffffe;
	execute_at_reset(&cpu, &fixture, 0x321d);
	assert(cpu.mach == 0xffffffff && cpu.macl == 0xfffffffa);
}

static void test_shifts_rotates_and_control_registers(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0x80000001;
	execute_at_reset(&cpu, &fixture, 0x4100);
	assert(cpu.r[1] == 2 && (cpu.sr & 1));
	cpu.r[1] = 0x80000001;
	execute_at_reset(&cpu, &fixture, 0x4121);
	assert(cpu.r[1] == 0xc0000000 && (cpu.sr & 1));
	cpu.r[1] = 0x80000001;
	execute_at_reset(&cpu, &fixture, 0x4104);
	assert(cpu.r[1] == 3 && (cpu.sr & 1));
	cpu.r[1] = 0x80000000;
	cpu.sr = 0;
	execute_at_reset(&cpu, &fixture, 0x4124);
	assert(cpu.r[1] == 0 && (cpu.sr & 1));
	cpu.r[1] = 0x12345678;
	execute_at_reset(&cpu, &fixture, 0x4118);
	assert(cpu.r[1] == 0x34567800);
	execute_at_reset(&cpu, &fixture, 0x4119);
	assert(cpu.r[1] == 0x00345678);
	cpu.r[1] = 0x12345678;
	execute_at_reset(&cpu, &fixture, 0x4128);
	assert(cpu.r[1] == 0x56780000);
	execute_at_reset(&cpu, &fixture, 0x4129);
	assert(cpu.r[1] == 0x00005678);
	cpu.r[1] = 0x345;
	execute_at_reset(&cpu, &fixture, 0x411e);
	assert(cpu.gbr == 0x345);
	cpu.vbr = 0x87654321;
	execute_at_reset(&cpu, &fixture, 0x0122);
	assert(cpu.r[1] == 0x87654321);
	cpu.pr = 0x10203040;
	cpu.r[1] = 0x384;
	execute_at_reset(&cpu, &fixture, 0x4122);
	assert(cpu.r[1] == 0x380);
	assert(fixture.memory[0x380] == 0x10 && fixture.memory[0x383] == 0x40);
	cpu.pr = 0;
	execute_at_reset(&cpu, &fixture, 0x4126);
	assert(cpu.pr == 0x10203040 && cpu.r[1] == 0x384);
}

static void test_indexed_and_gbr_memory(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[0] = 4;
	cpu.r[1] = 0x300;
	cpu.r[2] = 0x89abcdef;
	execute_at_reset(&cpu, &fixture, 0x0126);
	assert(fixture.memory[0x304] == 0x89 && fixture.memory[0x307] == 0xef);
	cpu.r[3] = 0;
	execute_at_reset(&cpu, &fixture, 0x031e);
	assert(cpu.r[3] == 0x89abcdef);
	cpu.gbr = 0x300;
	cpu.r[0] = 0x000080ff;
	execute_at_reset(&cpu, &fixture, 0xc102);
	assert(fixture.memory[0x304] == 0x80 && fixture.memory[0x305] == 0xff);
	cpu.r[0] = 0;
	execute_at_reset(&cpu, &fixture, 0xc502);
	assert(cpu.r[0] == 0xffff80ff);
	cpu.r[0] = 7;
	fixture.memory[0x307] = 0xf0;
	execute_at_reset(&cpu, &fixture, 0xcd0f);
	assert(fixture.memory[0x307] == 0);
	execute_at_reset(&cpu, &fixture, 0xcf81);
	assert(fixture.memory[0x307] == 0x81);
}

static void test_register_relative_branches(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 4;
	put16(&fixture, 0x100, 0x0123);
	put16(&fixture, 0x102, 0x7001);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x102);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x108 && cpu.r[0] == 1);
	cpu.pc = 0x110;
	cpu.r[1] = 4;
	put16(&fixture, 0x110, 0x0103);
	put16(&fixture, 0x112, 0x7002);
	karashi_sh2_step(&cpu);
	assert(cpu.pr == 0x114 && cpu.pc == 0x112);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x118 && cpu.r[0] == 3);
}

static void test_division_step(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0xfffffffd;
	cpu.r[2] = 5;
	execute_at_reset(&cpu, &fixture, 0x2217);
	assert((cpu.sr & 0x301) == 0x201);
	execute_at_reset(&cpu, &fixture, 0x0019);
	cpu.r[1] = 3;
	cpu.r[2] = 5;
	execute_at_reset(&cpu, &fixture, 0x3214);
	assert(cpu.r[2] == 7 && (cpu.sr & 0x301) == 1);
	cpu.r[1] = 3;
	cpu.r[2] = 5;
	cpu.sr = 0x100;
	execute_at_reset(&cpu, &fixture, 0x3214);
	assert(cpu.r[2] == 13 && (cpu.sr & 0x301) == 1);
	cpu.r[2] = 5;
	cpu.sr = 0x200;
	execute_at_reset(&cpu, &fixture, 0x3214);
	assert(cpu.r[2] == 13 && (cpu.sr & 0x301) == 0x301);
	cpu.r[2] = 5;
	cpu.sr = 0x300;
	execute_at_reset(&cpu, &fixture, 0x3214);
	assert(cpu.r[2] == 7 && (cpu.sr & 0x301) == 0x301);
}

static void test_trap_return_and_illegal_exceptions(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.vbr = 0x200;
	put32(&fixture, 0x280, 0x400);
	put16(&fixture, 0x100, 0xc320);
	put16(&fixture, 0x400, 0x002b);
	put16(&fixture, 0x402, 0x0009);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x400 && cpu.r[15] == 0x7f8);
	assert(get32(&fixture, 0x7f8) == 0x102 && get32(&fixture, 0x7fc) == 0xf0);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x402 && cpu.r[15] == 0x800 && cpu.sr == 0xf0);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x102);

	initialise(&cpu, &fixture);
	cpu.vbr = 0x200;
	put32(&fixture, 0x210, 0x500);
	put16(&fixture, 0x100, 0xffff);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x500 && cpu.fault == KARASHI_SH2_FAULT_NONE);
	assert(get32(&fixture, 0x7f8) == 0x100);

	initialise(&cpu, &fixture);
	cpu.vbr = 0x200;
	put32(&fixture, 0x218, 0x600);
	put16(&fixture, 0x100, 0xa001);
	put16(&fixture, 0x102, 0xffff);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x600 && cpu.fault == KARASHI_SH2_FAULT_NONE);
	assert(get32(&fixture, 0x7f8) == 0x106);
}

static void test_multiply_accumulate(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0x300;
	cpu.r[2] = 0x308;
	put32(&fixture, 0x300, 0xfffffffe);
	put32(&fixture, 0x308, 3);
	cpu.macl = 5;
	execute_at_reset(&cpu, &fixture, 0x021f);
	assert(cpu.mach == 0xffffffff && cpu.macl == 0xffffffff);
	assert(cpu.r[1] == 0x304 && cpu.r[2] == 0x30c);
	cpu.r[1] = 0x300;
	put32(&fixture, 0x300, 2);
	put32(&fixture, 0x304, 3);
	cpu.mach = cpu.macl = 0;
	execute_at_reset(&cpu, &fixture, 0x011f);
	assert(cpu.mach == 0 && cpu.macl == 6 && cpu.r[1] == 0x308);
	cpu.r[1] = 0x300;
	cpu.r[2] = 0x308;
	put32(&fixture, 0x300, 1);
	put32(&fixture, 0x308, 2);
	cpu.sr = 2;
	cpu.mach = 0x00007fff;
	cpu.macl = 0xfffffffe;
	execute_at_reset(&cpu, &fixture, 0x021f);
	assert(cpu.mach == 0x00007fff && cpu.macl == 0xffffffff);

	cpu.r[1] = 0x300;
	cpu.r[2] = 0x304;
	put16(&fixture, 0x300, 0xfffe);
	put16(&fixture, 0x304, 3);
	cpu.sr = 0;
	cpu.mach = 0;
	cpu.macl = 5;
	execute_at_reset(&cpu, &fixture, 0x421f);
	assert(cpu.mach == 0xffffffff && cpu.macl == 0xffffffff);
	assert(cpu.r[1] == 0x302 && cpu.r[2] == 0x306);
	cpu.r[1] = 0x300;
	cpu.r[2] = 0x304;
	put16(&fixture, 0x300, 1);
	put16(&fixture, 0x304, 1);
	cpu.sr = 2;
	cpu.mach = 0;
	cpu.macl = 0x7fffffff;
	execute_at_reset(&cpu, &fixture, 0x421f);
	assert(cpu.mach == 1 && cpu.macl == 0x7fffffff);
	put16(&fixture, 0x300, 0xffff);
	cpu.r[1] = 0x300;
	cpu.r[2] = 0x304;
	cpu.mach = 0;
	cpu.macl = 0x80000000;
	execute_at_reset(&cpu, &fixture, 0x421f);
	assert(cpu.mach == 1 && cpu.macl == 0x80000000);
}

static void test_state(void) {
	struct karashi_sh2 cpu;
	struct karashi_sh2 restored;
	struct fixture fixture;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	static uint8_t state[8192];
	size_t size;

	initialise(&cpu, &fixture);
	cpu.r[3] = 0x12345678;
	cpu.pr = 0x87654321;
	cpu.cycles = 99;
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_sh2_save(&cpu, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	initialise(&restored, &fixture);
	assert(karashi_state_reader_begin(&reader, state, size));
	assert(karashi_sh2_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.r[3] == 0x12345678 && restored.pr == 0x87654321 && restored.cycles == 99);
}

static void test_displacement_moves(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[0] = 0x1234abcd;
	cpu.r[3] = 0x300;
	put16(&fixture, 0x100, 0x8035);
	put16(&fixture, 0x102, 0x8132);
	put16(&fixture, 0x104, 0x8435);
	put16(&fixture, 0x106, 0x8532);
	karashi_sh2_step(&cpu);
	assert(fixture.memory[0x305] == 0xcd);
	karashi_sh2_step(&cpu);
	assert(fixture.memory[0x304] == 0xab && fixture.memory[0x305] == 0xcd);
	cpu.r[0] = 0;
	karashi_sh2_step(&cpu);
	assert(cpu.r[0] == 0xffffffcd);
	karashi_sh2_step(&cpu);
	assert(cpu.r[0] == 0xffffabcd);
}

static void test_interrupts_and_address_errors(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	put32(&fixture, 68 * 4, 0x400);
	put32(&fixture, 9 * 4, 0x500);
	put16(&fixture, 0x100, 0x0009);
	put16(&fixture, 0x102, 0xa001);
	put16(&fixture, 0x104, 0x0009);
	put16(&fixture, 0x108, 0x001b);
	karashi_sh2_set_irl(&cpu, 8);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x102);
	cpu.sr = 0x70;
	cpu.interrupt_check = true;
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x400 && (cpu.sr & 0xf0) == 0x80 && cpu.r[15] == 0x7f8);
	assert(get32(&fixture, 0x7f8) == 0x102 && get32(&fixture, 0x7fc) == 0x70);
	cpu.pc = 0x102;
	cpu.sr = 0x70;
	cpu.interrupt_check = true;
	karashi_sh2_set_irl(&cpu, 0);
	karashi_sh2_step(&cpu);
	karashi_sh2_set_irl(&cpu, 8);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x108 && (cpu.sr & 0xf0) == 0x70);
	karashi_sh2_set_irl(&cpu, 0);
	cpu.pc = 0x108;
	karashi_sh2_step(&cpu);
	assert(cpu.stopped);
	karashi_sh2_step(&cpu);
	assert(cpu.stopped && cpu.pc == 0x10a);
	karashi_sh2_set_irl(&cpu, 14);
	karashi_sh2_step(&cpu);
	assert(!cpu.stopped && cpu.pc == 0 && (cpu.sr & 0xf0) == 0xe0);
	karashi_sh2_set_irl(&cpu, 0);
	cpu.pc = 0x200;
	cpu.r[1] = 0x301;
	put16(&fixture, 0x200, 0x6211);
	karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x500 && cpu.fault == KARASHI_SH2_FAULT_NONE);
}

static void test_division_unit(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;

	initialise(&cpu, &fixture);
	cpu.r[1] = 0xffffff00;
	cpu.r[2] = 7;
	cpu.r[3] = (uint32_t) -100;
	cpu.r[4] = 0x100;
	cpu.r[5] = 0;
	put16(&fixture, 0x100, 0x2122);
	put16(&fixture, 0x102, 0x1131);
	put16(&fixture, 0x104, 0x5011);
	put16(&fixture, 0x106, 0x5614);
	put16(&fixture, 0x108, 0x2122);
	put16(&fixture, 0x10a, 0x1131);
	put16(&fixture, 0x10c, 0x5011);
	put16(&fixture, 0x10e, 0x0009);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.r[0] == (uint32_t) -14 && cpu.r[6] == (uint32_t) -2);
	cpu.r[2] = 0;
	cpu.r[3] = 5;
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	karashi_sh2_step(&cpu);
	assert(cpu.r[0] == 0x7fffffff);
	assert(cpu.divide_control & 1);
	cpu.divisor = 3;
	cpu.dividend_high = 1;
	cpu.dividend_low = 0;
	put32(&fixture, 0x120, 0);
	cpu.r[1] = 0xffffff14;
	cpu.r[2] = 0;
	cpu.pc = 0x120;
	put16(&fixture, 0x120, 0x2122);
	karashi_sh2_step(&cpu);
	assert(cpu.dividend_low == 0x55555555 && cpu.dividend_high == 1);
}

static void test_dma_and_timers(void) {
	struct karashi_sh2 cpu;
	struct fixture fixture;
	unsigned index;

	initialise(&cpu, &fixture);
	for (index = 0; index < 8; index++) fixture.memory[0x600 + index] = (uint8_t) (index + 1);
	cpu.dma[0].source = 0x600;
	cpu.dma[0].destination = 0x700;
	cpu.dma[0].count = 4;
	cpu.dma_operation = 1;
	cpu.r[1] = 0xffffff8c;
	cpu.r[2] = 0x5405;
	put16(&fixture, 0x100, 0x2122);
	karashi_sh2_step(&cpu);
	assert(!(cpu.dma[0].control & 2));
	assert(karashi_sh2_dreq(&cpu, 0));
	assert(fixture.memory[0x700] == 1 && fixture.memory[0x701] == 2);
	assert(karashi_sh2_dreq(&cpu, 0) && karashi_sh2_dreq(&cpu, 0) && karashi_sh2_dreq(&cpu, 0));
	assert(fixture.memory[0x707] == 8 && (cpu.dma[0].control & 2));
	assert(!karashi_sh2_dreq(&cpu, 0));
	cpu.dma[1].source = 0x600;
	cpu.dma[1].destination = 0x780;
	cpu.dma[1].count = 2;
	cpu.r[1] = 0xffffff9c;
	cpu.r[2] = 0x5a05;
	cpu.pc = 0x100;
	karashi_sh2_step(&cpu);
	assert(cpu.dma[1].control & 2);
	assert(fixture.memory[0x780] == 1 && fixture.memory[0x787] == 8);

	initialise(&cpu, &fixture);
	put32(&fixture, 0x50 * 4, 0x400);
	cpu.priority_b = 0x0500;
	cpu.vector_c = 0x0050;
	cpu.frt_enable = 0x09;
	cpu.frt_compare[0] = 2;
	cpu.frt_status = 1;
	cpu.timer_limit = 1;
	cpu.sr = 0;
	cpu.interrupt_check = true;
	for (index = 0; index < 8; index++) put16(&fixture, 0x100 + index * 2, 0x0009);
	for (index = 0; index < 16 && cpu.pc < 0x400; index++) karashi_sh2_step(&cpu);
	assert(cpu.pc == 0x400 && (cpu.sr & 0xf0) == 0x50 && cpu.frt_counter < 2);
}

int main(void) {
	test_displacement_moves();
	test_interrupts_and_address_errors();
	test_division_unit();
	test_dma_and_timers();
	test_reset_arithmetic_and_compare();
	test_memory_and_sign_extension();
	test_boot_window_and_cache_alias();
	test_delayed_branch_and_subroutine();
	test_string_multiply_and_tas();
	test_conditional_branches();
	test_carry_overflow_and_long_multiply();
	test_shifts_rotates_and_control_registers();
	test_indexed_and_gbr_memory();
	test_register_relative_branches();
	test_division_step();
	test_trap_return_and_illegal_exceptions();
	test_multiply_accumulate();
	test_state();
	puts("SH-2 integer, division, MAC, memory, control, exceptions, interrupts, DIVU, DMAC, FRT, delayed branches and state: pass");
	return 0;
}
