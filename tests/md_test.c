#include "md.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *rom, size_t *position, uint16_t value) {
	rom[(*position)++] = (uint8_t) (value >> 8);
	rom[(*position)++] = (uint8_t) value;
}

static void put32(uint8_t *rom, size_t *position, uint32_t value) {
	put16(rom, position, (uint16_t) (value >> 16));
	put16(rom, position, (uint16_t) value);
}

static void move_word_immediate_absolute(uint8_t *rom, size_t *position, uint16_t value, uint32_t address) {
	put16(rom, position, 0x33fc);
	put16(rom, position, value);
	put32(rom, position, address);
}

static void move_byte_immediate_absolute(uint8_t *rom, size_t *position, uint8_t value, uint32_t address) {
	put16(rom, position, 0x13fc);
	put16(rom, position, value);
	put32(rom, position, address);
}

static void move_byte_absolute_absolute(uint8_t *rom, size_t *position, uint32_t source, uint32_t destination) {
	put16(rom, position, 0x13f9);
	put32(rom, position, source);
	put32(rom, position, destination);
}

static void test_ports(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x1000];
	static const uint8_t sram_header[12] = {
		'R', 'A', 0xe0, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0xff, 0xff
	};
	size_t position = 0x200;
	unsigned index;

	put32(rom, &(size_t){0}, 0x00ffff00);
	put32(rom, &(size_t){4}, 0x00000200);
	memcpy(rom + 0x1b0, sram_header, sizeof(sram_header));
	memcpy(rom + 0x1f0, "E", 1);
	move_byte_immediate_absolute(rom, &position, 0x40, 0x00a10009);
	move_byte_immediate_absolute(rom, &position, 0x40, 0x00a10003);
	move_byte_absolute_absolute(rom, &position, 0x00a10003, 0x00ff0010);
	move_byte_immediate_absolute(rom, &position, 0x00, 0x00a10003);
	move_byte_absolute_absolute(rom, &position, 0x00a10003, 0x00ff0011);
	for (index = 0; index < 2; index++) {
		move_byte_immediate_absolute(rom, &position, 0x40, 0x00a10003);
		move_byte_immediate_absolute(rom, &position, 0x00, 0x00a10003);
	}
	move_byte_absolute_absolute(rom, &position, 0x00a10003, 0x00ff0012);
	move_byte_immediate_absolute(rom, &position, 0x40, 0x00a10003);
	move_byte_absolute_absolute(rom, &position, 0x00a10003, 0x00ff0013);
	move_byte_immediate_absolute(rom, &position, 0x00, 0x00a10003);
	move_byte_absolute_absolute(rom, &position, 0x00a10003, 0x00ff0014);
	move_byte_immediate_absolute(rom, &position, 0x01, 0x00a11200);
	move_byte_immediate_absolute(rom, &position, 0x01, 0x00a11100);
	move_byte_absolute_absolute(rom, &position, 0x00a11100, 0x00ff0015);
	move_byte_immediate_absolute(rom, &position, 0x5a, 0x00a00123);
	move_byte_absolute_absolute(rom, &position, 0x00a02123, 0x00ff0016);
	move_byte_immediate_absolute(rom, &position, 0xa7, 0x00200001);
	move_byte_absolute_absolute(rom, &position, 0x00200001, 0x00ff0017);
	move_byte_absolute_absolute(rom, &position, 0x00a10001, 0x00ff0018);
	move_byte_immediate_absolute(rom, &position, 0x26, 0x00a04000);
	move_byte_immediate_absolute(rom, &position, 0xff, 0x00a04001);
	move_byte_immediate_absolute(rom, &position, 0x27, 0x00a04000);
	move_byte_immediate_absolute(rom, &position, 0x0a, 0x00a04001);
	put16(rom, &position, 0x4e72);
	put16(rom, &position, 0x2700);

	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.pal && machine.vdp.lines == 313);
	karashi_md_set_pad(&machine, 0, KARASHI_MD_PAD_SIX);
	karashi_md_set_controller(
		&machine, 0, KARASHI_MD_PAD_UP | KARASHI_MD_PAD_B | KARASHI_MD_PAD_A | KARASHI_MD_PAD_X | KARASHI_MD_PAD_MODE
	);
	karashi_md_run_frame(&machine);
	assert(machine.cpu.stopped);
	assert(machine.work_ram[0x10] == 0x6e);
	assert(machine.work_ram[0x11] == 0x22);
	assert(machine.work_ram[0x12] == 0x20);
	assert(machine.work_ram[0x13] == 0x63);
	assert(machine.work_ram[0x14] == 0x2f);
	assert(machine.work_ram[0x15] == 0x00);
	assert(machine.work_ram[0x16] == 0x5a && machine.z80_ram[0x123] == 0x5a);
	assert(machine.work_ram[0x17] == 0xa7 && machine.sram[1] == 0xa7);
	assert(machine.work_ram[0x18] == 0xe0);
	machine.z80_bank = 0;
	assert(machine.z80.bus.read(machine.z80.bus.context, 0x8200) == rom[0x200]);
	machine.work_ram[0x1234] = 0x5a;
	machine.z80_bank = 0x1fe;
	assert(machine.z80.bus.read(machine.z80.bus.context, 0x9234) == 0x5a);
	machine.z80.bus.write(machine.z80.bus.context, 0x9235, 0xa5);
	assert(machine.work_ram[0x1235] == 0xa5);
	{
		uint8_t old_volume = machine.psg.volume[0];
		uint8_t new_volume = (uint8_t) ((old_volume + 1) & 15);
		machine.z80.bus.write(machine.z80.bus.context, 0x7f10, (uint8_t) (0x90 | new_volume));
		assert(machine.psg.volume[0] == old_volume);
		machine.z80.bus.write(machine.z80.bus.context, 0x7f11, (uint8_t) (0x90 | new_volume));
		assert(machine.psg.volume[0] == new_volume);
	}
	assert(machine.audio_frames > 955 && machine.audio_frames < 975);
	assert(machine.fm_frames > 1050 && machine.fm_frames < 1075);
	assert(karashi_ym2612_read(&machine.ym) & 2);
	machine.cpu.cycles += 12000;
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a10003, 0x40);
	assert(machine.pad_phase[0] == 2);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x00a10003) == 0x6e);
	karashi_md_set_pad(&machine, 1, KARASHI_MD_PAD_THREE);
	karashi_md_set_controller(
		&machine, 1, KARASHI_MD_PAD_UP | KARASHI_MD_PAD_B | KARASHI_MD_PAD_A | KARASHI_MD_PAD_START
	);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a1000b, 0x40);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a10005, 0x40);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x00a10005) == 0x6e);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a10005, 0x00);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x00a10005) == 0x02);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a1000b, 0x00);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x00a10005) == 0x02);
	machine.cpu.cycles += 171;
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x00a10005) == 0x02);
	machine.cpu.cycles++;
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x00a10005) == 0x6e);
}

static void test_eeprom_cartridge_detection(void) {
	static struct karashi_md machine;
	static struct karashi_md restored;
	static uint8_t rom[0x300000];
	static uint8_t state[512 * 1024];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t state_size;

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "T-81476", 7);
	rom[0x200000] = 0x5a;
	rom[0x200001] = 0xa5;
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_present);
	assert(!machine.sram_present);
	assert(machine.eeprom.scl && machine.eeprom.sda);
	assert(machine.eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM);
	assert(machine.eeprom_size == 8192 && machine.eeprom_address_bytes == 2);
	assert(!machine.eeprom_read_enabled);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 0x5aa5);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x200000, 0x0000);
	assert(machine.eeprom_read_enabled);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 1);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x200000, 0x0001);
	assert(!machine.eeprom_read_enabled);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 0x5aa5);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x200000, 0x0000);
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 3, 1));
	machine.z80_bank = 0x1fe;
	machine.z80_wait_quarters = 3;
	assert(karashi_md_save(&machine, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	karashi_md_init(&restored, rom, sizeof(rom));
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_md_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.z80_bank == 0x1fe && restored.z80_wait_quarters == 3);
	assert(restored.eeprom_read_enabled);
	assert(restored.cpu.bus.read16(restored.cpu.bus.context, 0x200000) == 1);
}

static void test_header_eeprom(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x200000];
	static const uint8_t header[12] = {'R', 'A', 0xe8, 0x40, 0x00, 0x20, 0x00, 0x01, 0x00, 0x20, 0x00, 0x01};

	memcpy(rom + 0x1b0, header, sizeof(header));
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_present && machine.eeprom_mapping == KARASHI_MD_EEPROM_SEGA);
	assert(machine.eeprom_size == 128);
	assert(!machine.sram_present);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200001, 0x03);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200001, 0x02);
	assert(machine.eeprom.scl && !machine.eeprom.sda && machine.eeprom.state != 0);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200000, 0x00);
	assert(machine.eeprom.scl);
	assert(!(machine.cpu.bus.read(machine.cpu.bus.context, 0x200001) & 1));
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200001, 0x03);
	assert(machine.eeprom.state == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x200001) & 1);
}

static void test_product_eeprom_families(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x200000];
	static const uint8_t sram_header[12] = {'R', 'A', 0xf8, 0x20, 0x00, 0x20, 0x00, 0x01, 0x00, 0x20, 0x3f, 0xff};
	static const struct {
		const char *product;
		uint8_t mapping;
		uint32_t size;
	} products[] = {
		{"T-081326", KARASHI_MD_EEPROM_ACCLAIM_WORD, 256},
		{"T-81033", KARASHI_MD_EEPROM_ACCLAIM_WORD, 256},
		{"T-081276", KARASHI_MD_EEPROM_ACCLAIM, 256},
		{"T-81406", KARASHI_MD_EEPROM_ACCLAIM, 512},
		{"T-081586", KARASHI_MD_EEPROM_ACCLAIM, 2048},
		{"T-81476", KARASHI_MD_EEPROM_ACCLAIM, 8192},
		{"T-81576", KARASHI_MD_EEPROM_ACCLAIM, 8192},
		{"T-120106", KARASHI_MD_EEPROM_J_CART, 1024},
		{"T-120096", KARASHI_MD_EEPROM_J_CART, 2048},
		{"T-120146", KARASHI_MD_EEPROM_J_CART, 8192},
		{"T-12046", KARASHI_MD_EEPROM_SEGA, 128},
		{"T-12053", KARASHI_MD_EEPROM_SEGA, 128},
		{"MK-1215", KARASHI_MD_EEPROM_SEGA, 128},
		{"MK-1228", KARASHI_MD_EEPROM_SEGA, 128},
		{"G-5538", KARASHI_MD_EEPROM_SEGA, 128},
		{"PR-1993", KARASHI_MD_EEPROM_SEGA, 128},
		{"G-4060", KARASHI_MD_EEPROM_SEGA, 128},
		{"00001211", KARASHI_MD_EEPROM_SEGA, 128},
		{"00004076", KARASHI_MD_EEPROM_SEGA, 128},
		{"G-4524", KARASHI_MD_EEPROM_SEGA, 128},
		{"00054503", KARASHI_MD_EEPROM_SEGA, 128},
		{"T-50176", KARASHI_MD_EEPROM_EA, 128},
		{"T-50396", KARASHI_MD_EEPROM_EA, 128},
		{"T-50446", KARASHI_MD_EEPROM_EA, 128},
		{"T-50516", KARASHI_MD_EEPROM_EA, 128},
		{"T-50606", KARASHI_MD_EEPROM_EA, 128}
	};
	size_t index;

	for (index = 0; index < sizeof(products) / sizeof(products[0]); index++) {
		memset(rom, 0, sizeof(rom));
		memcpy(rom + 0x183, products[index].product, strlen(products[index].product));
		karashi_md_init(&machine, rom, sizeof(rom));
		assert(machine.eeprom_present && !machine.sram_present);
		assert(machine.eeprom_mapping == products[index].mapping);
		assert(machine.eeprom_size == products[index].size);
	}

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "T-50176", 7);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_present && machine.eeprom_mapping == KARASHI_MD_EEPROM_EA);
	assert(!machine.sram_present);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200001, 0xc0);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200001, 0x40);
	assert(machine.eeprom.scl && !machine.eeprom.sda && machine.eeprom.state != 0);
	assert(!(machine.cpu.bus.read(machine.cpu.bus.context, 0x200001) & 0x80));
	machine.cpu.bus.write(machine.cpu.bus.context, 0x300001, 0xc0);
	assert(machine.eeprom.state == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x300001) & 0x80);

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "MK-1215", 7);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_present && machine.eeprom_mapping == KARASHI_MD_EEPROM_SEGA);
	assert(!machine.sram_present);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x280001, 0x03);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x280001, 0x02);
	assert(machine.eeprom.scl && !machine.eeprom.sda && machine.eeprom.state != 0);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x380001, 0x03);
	assert(machine.eeprom.state == 0);

	memcpy(rom + 0x1b0, sram_header, sizeof(sram_header));
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(!machine.eeprom_present && machine.eeprom_mapping == KARASHI_MD_EEPROM_NONE);
	assert(machine.sram_present && machine.sram_size == 0x2000);

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "T-081326", 8);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_mapping == KARASHI_MD_EEPROM_ACCLAIM_WORD && machine.eeprom_size == 256);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200000, 0x03);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200000, 0x02);
	assert(machine.eeprom.scl && !machine.eeprom.sda && machine.eeprom.state != 0);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x300000, 0x03);
	assert(machine.eeprom.state == 0);

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "T-120146", 8);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_mapping == KARASHI_MD_EEPROM_J_CART && machine.eeprom_size == 8192);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x300000, 0x03);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x300000, 0x02);
	assert(machine.eeprom.scl && !machine.eeprom.sda && machine.eeprom.state != 0);
	assert(!(machine.cpu.bus.read(machine.cpu.bus.context, 0x380001) & 0x80));
	machine.cpu.bus.write(machine.cpu.bus.context, 0x300000, 0x03);
	assert(machine.eeprom.state == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x380001) & 0x80);

	memset(rom, 0, sizeof(rom));
	rom[0] = 0x44;
	rom[1] = 0x4e;
	rom[2] = 0x4c;
	rom[3] = 0x44;
	memcpy(rom + 0x183, "00000000", 8);
	rom[0x18e] = 0x16;
	rom[0x18f] = 0x8b;
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.eeprom_mapping == KARASHI_MD_EEPROM_J_CART && machine.eeprom_size == 1024);
}

static void test_jcart_controllers(void) {
	static struct karashi_md machine;
	static struct karashi_md restored;
	static uint8_t rom[0x400000];
	static uint8_t state[512 * 1024];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t state_size;

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "T-120096", 8);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.jcart_present);
	assert(machine.eeprom_present && machine.eeprom_mapping == KARASHI_MD_EEPROM_J_CART);
	assert(machine.eeprom_size == 2048);
	karashi_md_set_controller(&machine, 2, KARASHI_MD_PAD_UP | KARASHI_MD_PAD_B);
	karashi_md_set_controller(&machine, 3, KARASHI_MD_PAD_DOWN | KARASHI_MD_PAD_C);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x380000) == 0x1dee);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x380000) == 0x1d);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x380001) == 0xee);
	machine.cpu.cycles = 1234;
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x380000, 0x0000);
	assert(!machine.jcart_th);
	assert(machine.pad_time[2] == 8638 && machine.pad_time[3] == 8638);
	machine.pad_latency[0] = 12345;
	karashi_md_set_controller(&machine, 2, KARASHI_MD_PAD_A | KARASHI_MD_PAD_START);
	karashi_md_set_controller(&machine, 3, KARASHI_MD_PAD_UP);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x380000) == 0x3283);

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_md_save(&machine, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	karashi_md_init(&restored, rom, sizeof(rom));
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_md_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.jcart_present && !restored.jcart_th);
	assert(restored.controller[2] == (KARASHI_MD_PAD_A | KARASHI_MD_PAD_START));
	assert(restored.controller[3] == KARASHI_MD_PAD_UP);
	assert(restored.pad_time[2] == 8638 && restored.pad_time[3] == 8638);
	assert(restored.pad_latency[0] == 12345);
	assert(restored.cpu.bus.read16(restored.cpu.bus.context, 0x380000) == 0x3283);

	memset(rom, 0, sizeof(rom));
	memcpy(rom + 0x183, "T-120146", 8);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(!machine.jcart_present && machine.eeprom_present);

	memset(rom, 0, 0x200000);
	memcpy(rom + 0x183, "T-120066", 8);
	karashi_md_init(&machine, rom, 0x200000);
	assert(machine.jcart_present && !machine.eeprom_present && !machine.sram_present);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x380000) == 0x3f7f);

	memset(rom, 0, sizeof(rom));
	rom[0] = rom[1] = rom[2] = 0xff;
	rom[3] = 0xfc;
	memcpy(rom + 0x183, "00000000", 8);
	rom[0x18e] = 0x16;
	rom[0x18f] = 0x8b;
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.jcart_present && !machine.eeprom_present);
	assert(machine.cpu.bus.read_map[0x38] == NULL);
}

static void test_product_sram_windows(void) {
	static struct karashi_md machine;
	static uint8_t barkley[0x200000];
	static uint8_t triple_play[0x400000];
	static uint8_t missing_header[0x100000];
	static const uint8_t barkley_header[12] = {
		'R', 'A', 0xf8, 0x40, 0x00, 0x20, 0x00, 0x01, 0x00, 0x20, 0x0f, 0xff
	};
	static const uint8_t triple_play_header[12] = {
		'R', 'A', 0xf8, 0x20, 0x00, 0x20, 0x00, 0x01, 0x00, 0x20, 0xff, 0xff
	};
	static const uint8_t psy_o_blade_header[12] = {
		'R', 'A', 0xf8, 0x20, 0x20, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x20
	};
	static const uint8_t volatile_header[12] = {
		'R', 'A', 0x20, 0x20, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff
	};
	static const uint8_t invalid_header[12] = {
		'R', 'A', 0xf8, 0x20, 0x00, 0x90, 0x00, 0x01, 0x00, 0x90, 0xff, 0xff
	};
	static const uint8_t even_header[12] = {
		'R', 'A', 0xf0, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x0e
	};
	static const uint8_t both_header[12] = {
		'R', 'A', 0xe0, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x0f
	};
	static const uint8_t volatile_both_header[12] = {
		'R', 'A', 0xa0, 0x20, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x0f
	};

	memset(barkley, 0, sizeof(barkley));
	memcpy(barkley + 0x183, "T-119186", 8);
	memcpy(barkley + 0x1b0, barkley_header, sizeof(barkley_header));
	karashi_md_init(&machine, barkley, sizeof(barkley));
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x3fffff);
	assert(machine.sram_size == 0x800 && machine.sram_mode == KARASHI_MD_SRAM_ODD);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200123, 0x5a);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x3ff123) == 0x5a);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x3fe457, 0xa5);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x200457) == 0xa5);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200456, 0x31);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200456) == 0xa5a5);

	memset(triple_play, 0, sizeof(triple_play));
	memcpy(triple_play + 0x183, "T-172026", 8);
	memcpy(triple_play + 0x1b0, triple_play_header, sizeof(triple_play_header));
	triple_play[0x200123] = 0x11;
	triple_play[0x300123] = 0x22;
	karashi_md_init(&machine, triple_play, sizeof(triple_play));
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x2fffff);
	assert(machine.sram_size == 0x8000 && machine.sram_mode == KARASHI_MD_SRAM_ODD);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x200123) == 0xff);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200123, 0x6b);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x2f0123) == 0x6b);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x300123) == 0x22);
	assert(machine.cpu.bus.read_map[0x20] == NULL && machine.cpu.bus.read_map[0x2f] == NULL);
	assert(machine.cpu.bus.read_map[0x30] == triple_play + 0x300000);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x183, "T-50086", 7);
	karashi_md_init(&machine, missing_header, 0x80000);
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x203fff);
	assert(machine.sram_size == 0x2000);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200101, 0x48);
	machine.cpu.bus.write(machine.cpu.bus.context, 0xa130f1, 0x02);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200101, 0x84);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x200101) == 0x48);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x183, "ACLD007", 7);
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x200fff);
	assert(machine.sram_size == 0x800);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200321, 0x7e);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x200321) == 0x7e);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x183, "T-50286", 7);
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x203fff);
	assert(machine.sram_size == 0x2000);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x183, "T-26013", 7);
	memcpy(missing_header + 0x1b0, psy_o_blade_header, sizeof(psy_o_blade_header));
	karashi_md_init(&machine, missing_header, 0xa0000);
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x203fff);
	assert(machine.sram_size == 0x2000);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x1b0, volatile_header, sizeof(volatile_header));
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(!machine.sram_present && !machine.sram_enabled);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x183, "00001051", 8);
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(!machine.sram_present && !machine.sram_enabled);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x183, "T-113016", 8);
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(!machine.sram_present && !machine.sram_enabled);

	memset(triple_play, 0, sizeof(triple_play));
	memset(triple_play + 0x150, ' ', 48);
	memcpy(triple_play + 0x150, "SONIC & KNUCKLES", 16);
	karashi_md_init(&machine, triple_play, sizeof(triple_play));
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x203fff);
	assert(machine.sram_size == 0x2000);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200123, 0x4d);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x200123) == 0x4d);

	memset(missing_header, 0, sizeof(missing_header));
	missing_header[0x18e] = 0x81;
	missing_header[0x18f] = 0x04;
	missing_header[0x200] = 0xae;
	missing_header[0x201] = 0xaa;
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(machine.sram_present && machine.sram_enabled);
	assert(machine.sram_start == 0x400001 && machine.sram_end == 0x40ffff);
	assert(machine.sram_size == 0x8000 && machine.sram_mode == KARASHI_MD_SRAM_ODD);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x400123, 0x72);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x400123) == 0x72);

	memset(triple_play, 0, sizeof(triple_play));
	memcpy(triple_play + 0x1b0, invalid_header, sizeof(invalid_header));
	karashi_md_init(&machine, triple_play, 0x300000);
	assert(machine.sram_present);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x20ffff);
	assert(machine.sram_size == 0x8000 && machine.sram_mode == KARASHI_MD_SRAM_ODD);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x1b0, even_header, sizeof(even_header));
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(machine.sram_size == 8 && machine.sram_mode == KARASHI_MD_SRAM_EVEN);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x200000, 0x5a91);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 0x5a5a);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x1b0, both_header, sizeof(both_header));
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(machine.sram_size == 16 && machine.sram_mode == KARASHI_MD_SRAM_BOTH);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x200000, 0x5a91);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 0x5a91);

	memset(missing_header, 0, sizeof(missing_header));
	memcpy(missing_header + 0x1b0, volatile_both_header, sizeof(volatile_both_header));
	karashi_md_init(&machine, missing_header, sizeof(missing_header));
	assert(machine.sram_present && !machine.sram_persistent && machine.sram_enabled);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x200000, 0x19a4);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 0x19a4);
}

static void test_sf004_mapper(void) {
	static struct karashi_md machine;
	static struct karashi_md restored;
	static uint8_t rom[0x200000];
	static uint8_t state[512 * 1024];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t state_size;
	unsigned bank;

	memset(rom, 0xff, sizeof(rom));
	memcpy(rom + 0x180, "SF-004", 6);
	for (bank = 0; bank < 32; bank++) rom[bank * 0x10000 + 0x1000] = (uint8_t) bank;
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.mapper == KARASHI_MD_MAPPER_SF004);
	assert(machine.sram_present && !machine.sram_enabled);
	assert(machine.sram_start == 0x200001 && machine.sram_end == 0x2fffff && machine.sram_size == 0x2000);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0xa13001) == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0xa13005) == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0xa13009) == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x001000) == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x031000) == 3);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x041000) == 0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x1f1000) == 3);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200000, 0x11);
	assert(machine.sram[0] == 0xff);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x000d01, 0x80);
	assert(machine.sram_enabled);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x200001, 0x22);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x201001, 0x33);
	assert(machine.sram[0] == 0x22 && machine.sram[0x800] == 0x33);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x204001) == 0x22);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x205001) == 0x33);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x200000) == 0x2222);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x000f01, 0x63);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x001000) == 0x18);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x031000) == 0x1b);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x041000) == 0x18);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x000e01, 0xc0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x001000) == 0x18);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x041000) == 0x1c);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x000f01, 0x10);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x041000) == 0x04);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x000e01, 0xc0);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x101000) == 0x14);
	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_md_save(&machine, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	memset(&restored, 0, sizeof(restored));
	restored.rom = rom;
	restored.rom_size = sizeof(rom);
	karashi_md_reset(&restored);
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_md_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.mapper == KARASHI_MD_MAPPER_SF004 && restored.sram_enabled && restored.sf004_linear);
	assert(restored.cpu.bus.read(restored.cpu.bus.context, 0x101000) == 0x14);
	assert(restored.cpu.bus.read(restored.cpu.bus.context, 0x2f9001) == 0x33);
	restored.cpu.bus.write(restored.cpu.bus.context, 0x000d01, 0x00);
	assert(!restored.sram_enabled);
}

static void t5740_lines(struct karashi_md *machine, uint8_t value) {
	machine->cpu.bus.write(machine->cpu.bus.context, 0x00a13009, value);
}

static void t5740_select(struct karashi_md *machine) {
	t5740_lines(machine, 0x0c);
	t5740_lines(machine, 0x04);
}

static void t5740_deselect(struct karashi_md *machine) {
	t5740_lines(machine, 0x0c);
}

static void t5740_send_byte(struct karashi_md *machine, uint8_t value) {
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		uint8_t input = (value & (0x80 >> bit)) ? 1 : 0;
		t5740_lines(machine, (uint8_t) (0x04 | input));
		t5740_lines(machine, (uint8_t) (0x06 | input));
	}
}

static uint8_t t5740_receive_byte(struct karashi_md *machine) {
	uint8_t value = 0;
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		t5740_lines(machine, 0x04);
		t5740_lines(machine, 0x06);
		value = (uint8_t) (value << 1 | (machine->cpu.bus.read(machine->cpu.bus.context, 0x00a1300b) & 1));
	}
	return value;
}

static void t5740_command(struct karashi_md *machine, uint8_t opcode) {
	t5740_select(machine);
	t5740_send_byte(machine, opcode);
	t5740_deselect(machine);
}

static void test_t5740_mapper(void) {
	static struct karashi_md machine;
	static struct karashi_md restored;
	static uint8_t rom[0x800000];
	static uint8_t state[512 * 1024];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t state_size;
	unsigned bank;

	memset(rom, 0xff, sizeof(rom));
	memcpy(rom + 0x183, "T-5740", 6);
	for (bank = 0; bank < 16; bank++) rom[bank * 0x80000 + 0x1234] = (uint8_t) bank;
	rom[0x0100] = 0x5a;
	rom[0x18100] = 0xa5;
	rom[0x18101] = 0xc3;
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.mapper == KARASHI_MD_MAPPER_T5740);
	assert(machine.eeprom_present && machine.eeprom_mapping == KARASHI_MD_EEPROM_SPI);
	assert(machine.eeprom_size == KARASHI_SPI_EEPROM_SIZE && !machine.sram_present);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x281234) == 5);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x301234) == 6);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x381234) == 7);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x018100) == 0x5a);
	assert(machine.cpu.bus.read16(machine.cpu.bus.context, 0x018100) == 0xa5c3);
	assert(machine.vdp.dma_read(machine.vdp.dma_context, 0x018100) == 0xa5c3);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a13003, 0x0f);
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00a13005, 0x0e);
	machine.cpu.bus.write16(machine.cpu.bus.context, 0x00a13006, 0x000d);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x281234) == 15);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x301234) == 14);
	assert(machine.cpu.bus.read(machine.cpu.bus.context, 0x381234) == 13);

	t5740_command(&machine, 0x06);
	t5740_select(&machine);
	t5740_send_byte(&machine, 0x02);
	t5740_send_byte(&machine, 0x12);
	t5740_send_byte(&machine, 0x34);
	t5740_send_byte(&machine, 0x6b);
	t5740_deselect(&machine);
	assert(machine.sram[0x1234] == 0x6b);
	t5740_select(&machine);
	t5740_send_byte(&machine, 0x03);
	t5740_send_byte(&machine, 0x12);
	t5740_send_byte(&machine, 0x34);
	assert(t5740_receive_byte(&machine) == 0x6b);
	t5740_deselect(&machine);

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_md_save(&machine, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	memset(&restored, 0, sizeof(restored));
	restored.rom = rom;
	restored.rom_size = sizeof(rom);
	karashi_md_reset(&restored);
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_md_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.mapper == KARASHI_MD_MAPPER_T5740);
	assert(restored.rom_bank[5] == 15 && restored.rom_bank[6] == 14 && restored.rom_bank[7] == 13);
	assert(restored.cpu.bus.read(restored.cpu.bus.context, 0x281234) == 15);
	assert(restored.sram[0x1234] == 0x6b);
}

static void test_regions(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x400];
	static const struct {
		const char *code;
		uint8_t version;
	} cases[] = {
		{"U  ", 0xa0}, {"J  ", 0x20}, {"E  ", 0xe0}, {"JUE", 0xa0}, {"JE ", 0x20}, {"K  ", 0x20},
		{"EUR", 0xe0}, {"4  ", 0xa0}, {"8  ", 0xe0}, {"1  ", 0x20}, {"F  ", 0xa0}, {"\x01E ", 0xa0}
	};
	unsigned index;

	put32(rom, &(size_t){0}, 0x00ffff00);
	put32(rom, &(size_t){4}, 0x00000200);
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		memcpy(rom + 0x1f0, cases[index].code, 3);
		karashi_md_init(&machine, rom, sizeof(rom));
		assert(machine.version == cases[index].version);
		assert(machine.pal == ((cases[index].version & 0x40) != 0));
	}
	karashi_md_set_region(&machine, KARASHI_MD_REGION_EUROPE);
	assert(machine.pal && machine.version == 0xe0);
	machine.work_ram[0] = 0x5a;
	machine.cpu.cycles = 1234;
	karashi_md_change_region(&machine, KARASHI_MD_REGION_USA);
	assert(!machine.pal && machine.version == 0xa0 && machine.vdp.lines == 262);
	assert(machine.master_clock == KARASHI_MD_NTSC_CLOCK && machine.psg.clock_rate == KARASHI_MD_NTSC_CLOCK / 15);
	assert(machine.work_ram[0] == 0x5a && machine.cpu.cycles == 1234);
	karashi_md_change_region(&machine, KARASHI_MD_REGION_JAPAN);
	assert(!machine.pal && machine.version == 0x20 && machine.work_ram[0] == 0x5a);
}

static void test_timing(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x400];
	size_t position = 0;

	memset(rom, 0, sizeof(rom));
	put32(rom, &position, 0x00ffff00);
	put32(rom, &position, 0x00000200);
	position = 0x200;
	put16(rom, &position, 0x4e72);
	put16(rom, &position, 0x2700);
	memcpy(rom + 0x1f0, "U  ", 3);
	karashi_md_init(&machine, rom, sizeof(rom));
	assert(!machine.pal && machine.vdp.lines == 262);
	assert(karashi_md_frame_rate(&machine) > 59.92 && karashi_md_frame_rate(&machine) < 59.93);
	karashi_md_run_frame(&machine);
	assert(machine.line_start == (uint64_t) KARASHI_MD_LINE_MCLK * 262);
	assert(machine.cpu.cycles * 7 >= machine.line_start && machine.cpu.cycles * 7 < machine.line_start + 28);
	assert(machine.audio_frames >= 799 && machine.audio_frames <= 801);

	karashi_md_set_region(&machine, KARASHI_MD_REGION_EUROPE);
	assert(machine.pal && machine.vdp.lines == 313);
	assert(karashi_md_frame_rate(&machine) > 49.70 && karashi_md_frame_rate(&machine) < 49.71);
	karashi_md_run_frame(&machine);
	assert(machine.line_start == (uint64_t) KARASHI_MD_LINE_MCLK * 313);
	assert(machine.cpu.cycles * 7 >= machine.line_start && machine.cpu.cycles * 7 < machine.line_start + 28);
	assert(machine.audio_frames >= 964 && machine.audio_frames <= 966);
}

static void test_main_bus_timing(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x400];
	size_t position = 0;
	uint64_t clocks;
	uint64_t available;

	put32(rom, &position, 0x00ffff00);
	put32(rom, &position, 0x200);
	position = 0x200;
	put16(rom, &position, 0x5280); /* ADDQ.L #1,D0 (8 clocks) */
	put16(rom, &position, 0x60fc); /* BRA.S back (10 clocks) */
	karashi_md_init(&machine, rom, sizeof(rom));
	karashi_md_run_frame(&machine);
	clocks = machine.cpu.cycles;
	available = clocks - (clocks / 128) * 2;
	assert(available == (uint64_t) machine.cpu.d[0] * 18 - (machine.cpu.pc == 0x202 ? 10 : 0));

	/* An ignored DD prefix must preserve the bank wait on LD A,($8200). */
	memcpy(machine.z80_ram, (const uint8_t[]){0xdd, 0x3a, 0x00, 0x82}, 4);
	machine.z80.cycles = 0;
	karashi_z80_step(&machine.z80);
	assert(machine.z80.af >> 8 == rom[0x200]);
	assert(machine.z80.cycles == 4 + 13 + 3);
	assert(machine.cpu.cycles == clocks + 8);

	machine.cpu.stopped = true;
	machine.z80_bank = 0x1fe;
	machine.z80.bus.write(machine.z80.bus.context, 0x8000, 0x5a);
	assert(machine.work_ram[0] == 0x5a && machine.z80.cycles == 23);
	machine.z80.bus.read(machine.z80.bus.context, 0x8000);
	machine.z80.bus.read(machine.z80.bus.context, 0x8000);
	assert(machine.z80.cycles == 30 && machine.z80_wait_quarters == 0);
	assert(machine.cpu.cycles == clocks + 8);
}

static void test_autovector_timing(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x400];
	size_t position = 0;
	unsigned phase;

	put32(rom, &position, 0x00ffff00);
	put32(rom, &position, 0x200);
	put32(rom, &(size_t){30 * 4}, 0x300);
	karashi_md_init(&machine, rom, sizeof(rom));
	for (phase = 0; phase < 10; phase++) {
		machine.cpu.cycles = 100 + phase;
		machine.cpu.sr = 0x2000;
		karashi_m68k_set_irq(&machine.cpu, 6);
		karashi_m68k_step(&machine.cpu);
		assert(machine.cpu.pc == 0x300);
		assert(machine.cpu.cycles == (phase ? 160u : 150u));
	}
}

static void test_psg_write_timing(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x400];
	unsigned before;
	unsigned frame;
	bool audible = false;

	memset(rom, 0, sizeof(rom));
	put32(rom, &(size_t){0}, 0x00ffff00);
	put32(rom, &(size_t){4}, 0x00000200);
	memcpy(rom + 0x1f0, "U  ", 3);
	karashi_md_init(&machine, rom, sizeof(rom));
	karashi_sn76489_write(&machine.psg, 0x80);
	karashi_sn76489_write(&machine.psg, 0x02);
	karashi_sn76489_write(&machine.psg, 0x90);
	machine.cpu.cycles = 1000;
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00c00011, 0x9f);
	assert(machine.audio_frames > 0);
	for (frame = 0; frame < machine.audio_frames; frame++) {
		if (machine.audio[frame * 2] || machine.audio[frame * 2 + 1]) audible = true;
	}
	assert(audible);
	before = machine.audio_frames;
	machine.cpu.cycles = 2000;
	machine.cpu.bus.write(machine.cpu.bus.context, 0x00c00011, 0x90);
	assert(machine.audio_frames > before);
	for (frame = before; frame < machine.audio_frames; frame++) {
		assert(machine.audio[frame * 2] == 0 && machine.audio[frame * 2 + 1] == 0);
	}
}

static void test_tmss(void) {
	static struct karashi_md machine;
	static uint8_t rom[0x1000];
	static uint8_t tmss[0x800];
	static const uint8_t handoff[] = {
		0x41, 0xf9, 0x00, 0x00, 0x02, 0x20, 0x43, 0xf9, 0x00, 0xff, 0x00, 0x00, 0x70, 0x08, 0x32, 0xd8,
		0x51, 0xc8, 0xff, 0xfc, 0x4e, 0xf9, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x13, 0xfc, 0x00, 0x01, 0x00, 0xa1, 0x41, 0x01, 0x2e, 0x78, 0x00, 0x00, 0x20, 0x78, 0x00, 0x04,
		0x4e, 0xd0
	};
	size_t position = 0x300;

	memset(rom, 0, sizeof(rom));
	memset(tmss, 0, sizeof(tmss));
	put32(rom, &(size_t){0}, 0x00fffe00);
	put32(rom, &(size_t){4}, 0x00000300);
	memcpy(rom + 0x100, "SEGA", 4);
	put16(rom, &position, 0x4e72);
	put16(rom, &position, 0x2700);
	put32(tmss, &(size_t){0}, 0x00ffff00);
	put32(tmss, &(size_t){4}, 0x00000200);
	memcpy(tmss + 0x200, handoff, sizeof(handoff));
	karashi_md_init(&machine, rom, sizeof(rom));
	assert((machine.version & 0x0f) == 0);
	karashi_md_set_tmss(&machine, tmss, sizeof(tmss));
	assert((machine.version & 0x0f) == 1 && machine.tmss_mapped);
	assert(machine.cpu.pc == 0x200 && machine.cpu.a[7] == 0x00ffff00);
	karashi_md_run_frame(&machine);
	assert(!machine.tmss_mapped && machine.cpu.stopped);
	assert(machine.cpu.pc == 0x304 && machine.cpu.a[7] == 0x00fffe00);
}

static void test_paprium(void) {
	static struct karashi_md md;
	static uint8_t rom[0x800000];
	static const uint8_t packed[] = {0x80, 2, 0xaa, 0xbb, 0x84, 2, 0};
	memcpy(rom + 0x183, "T-574120-00", 11);
	put16(rom, &(size_t){0x18e}, 0xa5fb);
	memcpy(rom + 0x700000, packed, sizeof(packed));
	karashi_md_init(&md, rom, sizeof(rom));
	assert(md.mapper == KARASHI_MD_MAPPER_PAPRIUM && md.sram_size == 4 * 0x780);
	struct karashi_m68k_bus *bus = &md.cpu.bus;
	assert(bus->read16(&md, 0x1560) == 0x4ef9 && bus->read16(&md, 0x700000) == 0x8002);
	bus->write16(&md, 0x1e10, 0x1000);
	bus->write16(&md, 0x1e12, 0x70);
	bus->write16(&md, 0x1e14, 0);
	bus->write16(&md, 0x1fea, 0xda02);
	assert(md.paprium.remaining == 6);
	assert(bus->read16(&md, 0xc000) == 0xaabb && bus->read16(&md, 0xc004) == 0xaabb);
	assert(md.paprium.remaining == 0);

	bus->write16(&md, 0x1e10, 0xbeef);
	bus->write16(&md, 0x1e12, 0xc000);
	for (unsigned bank = 1; bank <= 4; bank++) bus->write16(&md, 0x1fea, 0xe000 + bank);
	bus->write16(&md, 0x1fea, 0x8400);
	assert(md.paprium.restart_pending);
	karashi_md_reset(&md);
	assert(md.sram[0] == 0xaa && md.sram[3 * 0x780 + 1] == 0xbb);
	bus->write16(&md, 0x1fea, 0x8400);
	assert(!md.paprium.restart_pending);
}

int main(void) {
	test_tmss();
	test_regions();
	test_timing();
	test_main_bus_timing();
	test_autovector_timing();
	test_psg_write_timing();
	test_ports();
	test_paprium();
	test_eeprom_cartridge_detection();
	test_header_eeprom();
	test_product_eeprom_families();
	test_jcart_controllers();
	test_product_sram_windows();
	test_sf004_mapper();
	test_t5740_mapper();

	struct karashi_md machine;
	struct karashi_md restored;
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	static uint8_t rom[0x1000];
	static uint8_t state[512 * 1024];
	size_t position = 0x200;
	size_t state_size;
	unsigned index;

	put32(rom, &(size_t){0}, 0x00ffff00);
	put32(rom, &(size_t){4}, 0x00000200);
	move_word_immediate_absolute(rom, &position, 0x8004, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x8144, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x8230, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x8c01, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x8f02, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0xc002, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x0000, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x000e, 0x00c00000);
	move_word_immediate_absolute(rom, &position, 0x4020, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x0000, 0x00c00004);
	for (index = 0; index < 16; index++) {
		move_word_immediate_absolute(rom, &position, 0x1111, 0x00c00000);
	}
	move_word_immediate_absolute(rom, &position, 0x4000, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x0003, 0x00c00004);
	move_word_immediate_absolute(rom, &position, 0x0001, 0x00c00000);
	put16(rom, &position, 0x23fc);
	put32(rom, &position, 0x12345678);
	put32(rom, &position, 0x00ff0000);
	move_word_immediate_absolute(rom, &position, 0x0100, 0x00a11100);
	put16(rom, &position, 0x4e72);
	put16(rom, &position, 0x2700);

	karashi_md_init(&machine, rom, sizeof(rom));
	assert(machine.cpu.pc == 0x200);
	karashi_md_run_frame(&machine);
	assert(machine.cpu.stopped);
	assert(machine.work_ram[0] == 0x12 && machine.work_ram[3] == 0x78);
	assert(machine.z80_bus_requested);
	karashi_md_run_frame(&machine);
	for (index = 0; index < 8; index++) {
		assert(machine.video[KARASHI_MD_WIDTH + index] == KARASHI_PIXEL(255, 0, 0));
	}
	assert(machine.audio_frames > 790 && machine.audio_frames < 810);
	karashi_md_set_controller(&machine, 0, KARASHI_MD_PAD_A | KARASHI_MD_PAD_START);
	machine.io_data[0] = 0;
	assert((machine.controller[0] & (KARASHI_MD_PAD_A | KARASHI_MD_PAD_START)) != 0);

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 4, 1));
	assert(karashi_md_save(&machine, &writer));
	assert(karashi_state_writer_finish(&writer, &state_size));
	memset(&restored, 0, sizeof(restored));
	restored.rom = rom;
	restored.rom_size = sizeof(rom);
	karashi_md_reset(&restored);
	assert(karashi_state_reader_begin(&reader, state, state_size));
	assert(karashi_md_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	assert(restored.cpu.pc == machine.cpu.pc);
	assert(restored.work_ram[0] == 0x12);
	assert(restored.vdp.vram[32] == 0x11);
	machine = restored;
	assert(machine.cpu.bus.write_map[0xe0] != machine.work_ram);
	karashi_md_rebind(&machine);
	assert(machine.cpu.bus.context == &machine);
	assert(machine.z80.bus.context == &machine);
	assert(machine.vdp.dma_context == &machine);
	assert(machine.cpu.bus.read_map[0xe0] == machine.work_ram);
	assert(machine.cpu.bus.write_map[0xe0] == machine.work_ram);
	puts("Mega Drive reset, memory map, product SRAM, SF-004, T-5740 SPI, J-Cart, VDP, Z80 bus, pads, save RAM, FM timers, regions, TMSS and state: pass");
	return 0;
}
