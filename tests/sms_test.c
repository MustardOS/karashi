#include "sms.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define ROM_SIZE (8 * 0x4000)

static uint8_t rom[ROM_SIZE];
static struct karashi_sms machine;
static struct karashi_sms restored;
static uint8_t state[128 * 1024];

static uint8_t peek(uint16_t address) {
	return machine.cpu.bus.read(&machine, address);
}

static void poke(uint16_t address, uint8_t value) {
	machine.cpu.bus.write(&machine, address, value);
}

static uint8_t input(uint8_t port) {
	return machine.cpu.bus.io_read(&machine, port);
}

static void output(uint8_t port, uint8_t value) {
	machine.cpu.bus.io_write(&machine, port, value);
}

static void emit(size_t *position, uint8_t value) {
	rom[(*position)++] = value;
}

static void emit_register(size_t *position, uint8_t index, uint8_t value) {
	emit(position, 0x3e);
	emit(position, value);
	emit(position, 0xd3);
	emit(position, 0xbf);
	emit(position, 0x3e);
	emit(position, (uint8_t) (0x80 | index));
	emit(position, 0xd3);
	emit(position, 0xbf);
}

static void build_rom(void) {
	static const uint8_t interrupt[] = {0xf5, 0xe5, 0xdb, 0xbf, 0x21, 0x00, 0xc0, 0x34, 0xe1, 0xf1, 0xfb, 0xed, 0x4d};
	static const uint8_t nmi[] = {0xe5, 0x21, 0x01, 0xc0, 0x34, 0xe1, 0xed, 0x45};
	size_t position = 0x100;
	unsigned page;

	memset(rom, 0, sizeof(rom));
	for (page = 0; page < 8; page++) rom[page * 0x4000 + 0x3fff] = (uint8_t) page;
	rom[0] = 0xc3;
	rom[1] = 0x00;
	rom[2] = 0x01;
	memcpy(rom + 0x38, interrupt, sizeof(interrupt));
	memcpy(rom + 0x66, nmi, sizeof(nmi));
	emit(&position, 0xf3);
	emit(&position, 0xed);
	emit(&position, 0x56);
	emit(&position, 0x31);
	emit(&position, 0xf0);
	emit(&position, 0xdf);
	emit_register(&position, 0, 0x14);
	emit_register(&position, 1, 0x60);
	emit_register(&position, 10, 95);
	emit(&position, 0x3e);
	emit(&position, 0x9f);
	emit(&position, 0xd3);
	emit(&position, 0x7f);
	emit(&position, 0xfb);
	emit(&position, 0x76);
	emit(&position, 0x18);
	emit(&position, 0xfd);
}

static void test_memory(void) {
	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	assert(machine.mapper == KARASHI_SMS_MAPPER_SEGA);
	assert(peek(0x3fff) == 0);
	assert(peek(0x7fff) == 1);
	assert(peek(0xbfff) == 2);
	poke(0xffff, 5);
	assert(peek(0xbfff) == 5);
	assert(peek(0xdfff) == 5);
	poke(0xfffe, 7);
	assert(peek(0x7fff) == 7);
	poke(0xfffd, 3);
	assert(peek(0x3fff) == 3);
	assert(peek(0x0000) == 0xc3);
	poke(0xffff, 9);
	assert(peek(0xbfff) == 1);

	poke(0xc123, 0x5a);
	assert(peek(0xe123) == 0x5a);
	poke(0x8000, 0x77);
	assert(peek(0x8000) != 0x77);
	poke(0xfffc, 0x08);
	poke(0x8000, 0x77);
	assert(peek(0x8000) == 0x77);
	poke(0xfffc, 0x0c);
	assert(peek(0x8000) == 0x00);
	poke(0x8000, 0x33);
	poke(0xfffc, 0x08);
	assert(peek(0x8000) == 0x77);
	assert(machine.cart_ram[0x4000] == 0x33);
	poke(0xfffc, 0x00);
	assert(peek(0xbfff) == 1);
}

static void test_codemasters(void) {
	rom[0x7fe6] = 0x34;
	rom[0x7fe7] = 0x12;
	rom[0x7fe8] = 0xcc;
	rom[0x7fe9] = 0xed;
	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	assert(machine.mapper == KARASHI_SMS_MAPPER_CODEMASTERS);
	assert(peek(0xbfff) == 0);
	poke(0x8000, 6);
	assert(peek(0xbfff) == 6);
	poke(0x0000, 4);
	assert(peek(0x3fff) == 4);
	memset(rom + 0x7fe6, 0, 4);
}

static void test_ports(void) {
	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	assert(input(0xdc) == 0xff);
	assert(input(0xdd) == 0xff);
	karashi_sms_set_controller(&machine, 0, KARASHI_SMS_PAD_UP | KARASHI_SMS_PAD_TWO);
	karashi_sms_set_controller(&machine, 1, KARASHI_SMS_PAD_DOWN | KARASHI_SMS_PAD_ONE);
	assert(input(0xdc) == 0x5e);
	assert(input(0xc0) == 0x5e);
	assert(input(0xdd) == 0xfb);

	output(0x3f, 0xf5);
	assert((input(0xdd) & 0xc0) == 0xc0);
	output(0x3f, 0x55);
	assert((input(0xdd) & 0xc0) == 0x00);
	machine.japanese = true;
	assert((input(0xdd) & 0xc0) == 0xc0);

	output(0xbf, 0x34);
	output(0xbf, 0x52);
	output(0xbe, 0x99);
	assert(machine.vdp.tms.vram[0x1234] == 0x99);
	output(0x7f, 0x90);
	assert(machine.psg.volume[0] == 0);
}

static void test_game_gear(void) {
	karashi_sms_init(&machine, rom, sizeof(rom), true, false, false);
	assert(input(0x00) == 0xc0);
	karashi_sms_set_pause(&machine, true);
	assert(input(0x00) == 0x40);
	karashi_sms_run_frame(&machine);
	assert(peek(0xc001) == 0);
	output(0x06, 0x1e);
	assert(machine.psg.stereo == 0x1e);
	output(0xbf, 0x00);
	output(0xbf, 0xc0);
	output(0xbe, 0x0f);
	output(0xbe, 0x0f);
	assert(machine.vdp.cram[0] == 0x0f0f);
}

static void test_interrupts_and_pause(void) {
	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	karashi_sms_run_frame(&machine);
	assert(peek(0xc000) == 1);
	karashi_sms_run_frame(&machine);
	assert(peek(0xc000) == 4);
	karashi_sms_run_frame(&machine);
	assert(peek(0xc000) == 7);
	assert(machine.cpu.halted);
	assert(machine.audio_frames >= 800 && machine.audio_frames <= 802);

	assert(peek(0xc001) == 0);
	karashi_sms_set_pause(&machine, true);
	karashi_sms_run_frame(&machine);
	karashi_sms_run_frame(&machine);
	assert(peek(0xc001) == 1);
	karashi_sms_set_pause(&machine, false);
	karashi_sms_run_frame(&machine);
	karashi_sms_set_pause(&machine, true);
	karashi_sms_run_frame(&machine);
	assert(peek(0xc001) == 2);
}

static void test_pal(void) {
	uint64_t start;

	karashi_sms_init(&machine, rom, sizeof(rom), false, false, true);
	start = machine.cpu.cycles;
	karashi_sms_run_frame(&machine);
	assert(machine.cpu.cycles - start >= KARASHI_SEGA8_PAL_FRAME_CYCLES);
	assert(machine.cpu.cycles - start < KARASHI_SEGA8_PAL_FRAME_CYCLES + 32);
	assert(machine.audio_frames >= 965 && machine.audio_frames <= 967);
	assert(karashi_sms_frame_rate(&machine) > 49.69 && karashi_sms_frame_rate(&machine) < 49.71);
	assert(karashi_sms_height(&machine) == 192);
	machine.ram[0] = 0x5a;
	start = machine.cpu.cycles;
	karashi_sms_set_region(&machine, false, false);
	assert(!machine.pal && !machine.vdp.pal && !machine.japanese && !machine.fm_present);
	assert(machine.psg.clock_rate == KARASHI_SEGA8_CPU_CLOCK);
	assert(machine.cpu.cycles == start && machine.ram[0] == 0x5a);
	assert(karashi_sms_frame_rate(&machine) > 59.92 && karashi_sms_frame_rate(&machine) < 59.93);
	karashi_sms_set_region(&machine, true, false);
	assert(machine.japanese && machine.fm_present && machine.fm_clock == machine.cpu.cycles);
	karashi_sms_init(&machine, rom, sizeof(rom), true, false, true);
	assert(!machine.pal);
	karashi_sms_set_region(&machine, false, true);
	assert(!machine.pal && !machine.vdp.pal);
}

static void test_state(void) {
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;
	unsigned frame;

	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	for (frame = 0; frame < 3; frame++) karashi_sms_run_frame(&machine);
	poke(0xfffc, 0x08);
	poke(0x9000, 0x42);
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 2, 1));
	assert(karashi_sms_save(&machine, &writer));
	assert(karashi_state_writer_finish(&writer, &size));

	restored = machine;
	for (frame = 0; frame < 5; frame++) karashi_sms_run_frame(&machine);
	restored.ram[0] = 0xee;
	restored.cart_ram[0x1000] = 0;
	assert(karashi_state_reader_begin(&reader, state, size));
	assert(karashi_sms_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.cart_ram[0x1000] == 0x42);
	restored.cpu.bus.context = &restored;
	for (frame = 0; frame < 5; frame++) karashi_sms_run_frame(&restored);
	assert(restored.cpu.cycles == machine.cpu.cycles);
	assert(restored.cpu.pc == machine.cpu.pc);
	assert(memcmp(restored.ram, machine.ram, sizeof(machine.ram)) == 0);
	assert(memcmp(restored.video, machine.video, sizeof(machine.video)) == 0);
	assert(memcmp(restored.audio, machine.audio, sizeof(machine.audio)) == 0);

	state[size - 1] ^= 0x01;
	assert(!karashi_state_reader_begin(&reader, state, size));
}

static void test_phaser(void) {
	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	karashi_sms_set_device(&machine, 0, KARASHI_SMS_DEVICE_PHASER);
	karashi_sms_set_phaser(&machine, 0, 100, 50, true);
	assert((input(0xdc) & 0x10) == 0);
	machine.video[50 * KARASHI_SMSVDP_WIDTH + 100] = 0xffff;
	machine.line = 50;
	machine.line_start = machine.cpu.cycles - 40;
	assert(input(0xdd) & 0x40);
	machine.line_start = machine.cpu.cycles - 100;
	assert((input(0xdd) & 0x40) == 0);
	assert(input(0x7f) == (100 + 44) / 2);
	machine.line = 120;
	assert(input(0xdd) & 0x40);
	karashi_sms_set_phaser(&machine, 0, -1, -1, false);
	machine.line = 50;
	assert(input(0xdd) & 0x40);
	assert(input(0xdc) & 0x10);
}

static void test_paddle(void) {
	karashi_sms_init(&machine, rom, sizeof(rom), false, true, false);
	karashi_sms_set_device(&machine, 0, KARASHI_SMS_DEVICE_PADDLE);
	karashi_sms_set_paddle(&machine, 0, 0xa5, true);
	machine.cpu.cycles = 0;
	assert((input(0xdc) & 0x3f) == 0x05);
	machine.cpu.cycles = 224;
	assert((input(0xdc) & 0x3f) == 0x2a);
	karashi_sms_set_paddle(&machine, 0, 0xa5, false);
	assert((input(0xdc) & 0x3f) == 0x3a);

	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	karashi_sms_set_device(&machine, 0, KARASHI_SMS_DEVICE_PADDLE);
	karashi_sms_set_paddle(&machine, 0, 0xa5, false);
	output(0x3f, 0xdd);
	assert((input(0xdc) & 0x2f) == 0x05);
	output(0x3f, 0xfd);
	assert((input(0xdc) & 0x2f) == 0x2a);
}

static void test_sports_pad(void) {
	uint8_t nibbles[4];
	unsigned index;

	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	karashi_sms_set_device(&machine, 0, KARASHI_SMS_DEVICE_SPORTS_PAD);
	karashi_sms_set_sports_pad(&machine, 0, 0x12, -0x34, 1);
	machine.cpu.cycles = 10000;
	for (index = 0; index < 4; index++) {
		output(0x3f, index & 1 ? 0x2d : 0x0d);
		nibbles[index] = input(0xdc);
		machine.cpu.cycles += 300;
	}
	assert((uint8_t) -((nibbles[0] & 0x0f) << 4 | (nibbles[1] & 0x0f)) == 0x12);
	assert((uint8_t) -((nibbles[2] & 0x0f) << 4 | (nibbles[3] & 0x0f)) == (uint8_t) -0x34);
	assert((nibbles[0] & 0x30) == 0x20);
	machine.cpu.cycles += 5000;
	output(0x3f, 0x2d);
	output(0x3f, 0x0d);
	assert((input(0xdc) & 0x0f) == 0);

	karashi_sms_init(&machine, rom, sizeof(rom), false, true, false);
	karashi_sms_set_device(&machine, 0, KARASHI_SMS_DEVICE_SPORTS_PAD);
	karashi_sms_set_sports_pad(&machine, 0, 0x5a, 0x3c, 2);
	machine.cpu.cycles = 224;
	assert((input(0xdc) & 0x3f) == 0x3d);
	machine.cpu.cycles = 448;
	assert((input(0xdc) & 0x3f) == 0x05);
	machine.cpu.cycles = 672;
	assert((input(0xdc) & 0x3f) == 0x1a);
	machine.cpu.cycles = 896;
	assert((input(0xdc) & 0x3f) == 0x03);
	machine.cpu.cycles = 1120;
	assert((input(0xdc) & 0x3f) == 0x1c);
	machine.cpu.cycles = 0;
	assert((input(0xdc) & 0x30) == 0x10);
}

static void test_fm_unit(void) {
	unsigned before;
	unsigned frame;
	bool audible = false;

	karashi_sms_init(&machine, rom, sizeof(rom), false, true, false);
	assert(machine.fm_present);
	karashi_sn76489_write(&machine.psg, 0x80);
	karashi_sn76489_write(&machine.psg, 0x02);
	karashi_sn76489_write(&machine.psg, 0x90);
	machine.cpu.cycles = 1000;
	output(0xf2, 0x01);
	assert((input(0xf2) & 7) == 1);
	assert(machine.audio_frames > 0);
	for (frame = 0; frame < machine.audio_frames; frame++) {
		if (machine.audio[frame * 2] || machine.audio[frame * 2 + 1]) audible = true;
	}
	assert(audible);
	before = machine.audio_frames;
	machine.cpu.cycles = 2000;
	output(0xf2, 0x03);
	assert((input(0xf2) & 7) == 3 && machine.audio_frames > before);
	for (frame = before; frame < machine.audio_frames; frame++) {
		assert(machine.audio[frame * 2] == 0 && machine.audio[frame * 2 + 1] == 0);
	}
	before = machine.fm_frames;
	machine.cpu.cycles = 2200;
	output(0xf0, 0x10);
	assert(machine.fm_frames == before);
	output(0xf1, 0x55);
	assert(machine.fm.registers[0x10] == 0x55 && machine.fm_frames > before);

	karashi_sms_init(&machine, rom, sizeof(rom), false, false, false);
	assert(!machine.fm_present);
	output(0xf2, 0x01);
	assert(machine.fm_control == 0);
}

static void test_korean_mappers(void) {
	static uint8_t korean[0x20000];
	unsigned bank;

	for (bank = 0; bank < 16; bank++) memset(korean + bank * 0x2000, (int) bank, 0x2000);
	karashi_sms_init(&machine, korean, sizeof(korean), false, true, false);
	poke(0x0002, 5);
	assert(machine.mapper == KARASHI_SMS_MAPPER_MSX);
	assert(peek(0x4000) == 5 && peek(0x0000) == 0 && peek(0x2000) == 1);
	poke(0x0000, 7);
	poke(0x0001, 9);
	poke(0x0003, 11);
	assert(peek(0x8000) == 7 && peek(0xa000) == 9 && peek(0x6000) == 11);

	karashi_sms_init(&machine, korean, sizeof(korean), false, true, false);
	poke(0xa000, 3);
	assert(machine.mapper == KARASHI_SMS_MAPPER_KOREAN);
	assert(peek(0x8000) == 6 && peek(0xa000) == 7 && peek(0x4000) == 2);

	memcpy(korean + 0x7ff0, "TMR SEGA", 8);
	karashi_sms_init(&machine, korean, sizeof(korean), false, true, false);
	poke(0x0002, 5);
	poke(0xa000, 3);
	assert(machine.mapper == KARASHI_SMS_MAPPER_SEGA);
	karashi_sms_reset(&machine);
	assert(machine.mapper == KARASHI_SMS_MAPPER_SEGA);
}

static void test_bios(void) {
	static uint8_t bios[0x2000];
	static uint8_t gg_bios[0x400];
	static const uint8_t handoff[] = {
		0x21, 0x00, 0xc0, 0x36, 0xd3, 0x23, 0x36, 0x3e, 0x23, 0x36, 0xc3, 0x23, 0x36, 0x00, 0x23, 0x36, 0x00,
		0x3e, 0xab, 0xc3, 0x00, 0xc0
	};
	unsigned frame;

	memset(bios, 0x76, sizeof(bios));
	memcpy(bios, handoff, sizeof(handoff));
	bios[0x1234] = 0x5a;
	karashi_sms_init(&machine, rom, ROM_SIZE, false, false, false);
	karashi_sms_set_bios(&machine, bios, sizeof(bios));
	assert(peek(0x0000) == 0x21 && peek(0x1234) == 0x5a && peek(0x2000 + 0x1234) == 0x5a);
	assert(machine.cpu.sp != 0xdff0);
	for (frame = 0; frame < 2; frame++) karashi_sms_run_frame(&machine);
	assert(machine.memory_control == 0xab);
	assert(peek(0x0000) == rom[0] && peek(0x7fff) == rom[0x7fff]);
	output(0x3e, 0xe3);
	assert(peek(0x0000) == 0x21);
	output(0x3e, 0xab);

	memset(gg_bios, 0, sizeof(gg_bios));
	memcpy(gg_bios, handoff, sizeof(handoff));
	karashi_sms_init(&machine, rom, ROM_SIZE, true, false, false);
	karashi_sms_set_bios(&machine, gg_bios, sizeof(gg_bios));
	assert(peek(0x0000) == 0x21 && peek(0x0400) == rom[0x0400] && peek(0x7ff0) == rom[0x7ff0]);
	for (frame = 0; frame < 2; frame++) karashi_sms_run_frame(&machine);
	assert(machine.memory_control & 0x08);
	assert(peek(0x0000) == rom[0]);
	karashi_sms_set_bios(&machine, NULL, 0);
}

int main(void) {
	build_rom();
	test_bios();
	test_korean_mappers();
	test_fm_unit();
	test_phaser();
	test_paddle();
	test_sports_pad();
	test_memory();
	test_codemasters();
	test_ports();
	test_game_gear();
	test_interrupts_and_pause();
	test_pal();
	test_state();
	puts("SMS and Game Gear BIOS, Sega, Codemasters, Korean and MSX mappers, Light Phaser, paddle, Sports Pad, FM unit, ports, interrupts, pause and state: pass");
	return 0;
}
