#include "sg1000.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void emit(uint8_t *rom, size_t *position, uint8_t value) {
	rom[(*position)++] = value;
}

static void output(uint8_t *rom, size_t *position, uint8_t port, uint8_t value) {
	emit(rom, position, 0x3e);
	emit(rom, position, value);
	emit(rom, position, 0xd3);
	emit(rom, position, port);
}

static void register_write(uint8_t *rom, size_t *position, uint8_t index, uint8_t value) {
	output(rom, position, 0xbf, value);
	output(rom, position, 0xbf, (uint8_t) (0x80 | index));
}

static void build_diagnostic(uint8_t *rom, size_t size) {
	size_t position = 0x40;

	memset(rom, 0, size);
	rom[0] = 0xc3;
	rom[1] = 0x40;
	rom[2] = 0x00;
	rom[0x38] = 0xdb;
	rom[0x39] = 0xbf;
	rom[0x3a] = 0xfb;
	rom[0x3b] = 0xed;
	rom[0x3c] = 0x4d;
	emit(rom, &position, 0xf3);
	emit(rom, &position, 0x31);
	emit(rom, &position, 0x00);
	emit(rom, &position, 0xd0);
	register_write(rom, &position, 1, 0x60);
	register_write(rom, &position, 2, 0x06);
	register_write(rom, &position, 3, 0x80);
	register_write(rom, &position, 4, 0x00);
	register_write(rom, &position, 5, 0x36);
	register_write(rom, &position, 6, 0x00);
	register_write(rom, &position, 7, 0x04);
	output(rom, &position, 0xbf, 0x08);
	output(rom, &position, 0xbf, 0x40);
	output(rom, &position, 0xbe, 0x80);
	output(rom, &position, 0xbf, 0x00);
	output(rom, &position, 0xbf, 0x60);
	output(rom, &position, 0xbe, 0xf4);
	output(rom, &position, 0xbf, 0x00);
	output(rom, &position, 0xbf, 0x58);
	output(rom, &position, 0xbe, 0x01);
	output(rom, &position, 0x7f, 0x80);
	output(rom, &position, 0x7f, 0x20);
	output(rom, &position, 0x7f, 0x90);
	emit(rom, &position, 0xfb);
	emit(rom, &position, 0x76);
	emit(rom, &position, 0x18);
	emit(rom, &position, 0xfd);
}

static void test_state(struct karashi_sg1000 *machine) {
	static struct karashi_sg1000 restored;
	static uint8_t state[65536];
	struct karashi_state_writer writer;
	struct karashi_state_reader reader;
	size_t size;
	unsigned frame;

	assert(karashi_state_writer_begin(&writer, state, sizeof(state), 1, 1));
	assert(karashi_sg1000_save(machine, &writer));
	assert(karashi_state_writer_finish(&writer, &size));
	restored = *machine;
	for (frame = 0; frame < 4; frame++) karashi_sg1000_run_frame(machine);
	restored.ram[5] = 0xee;
	assert(karashi_state_reader_begin(&reader, state, size));
	assert(karashi_sg1000_load(&restored, &reader));
	assert(karashi_state_reader_finished(&reader));
	restored.cpu.bus.context = &restored;
	for (frame = 0; frame < 4; frame++) karashi_sg1000_run_frame(&restored);
	assert(restored.cpu.cycles == machine->cpu.cycles);
	assert(memcmp(restored.ram, machine->ram, sizeof(machine->ram)) == 0);
	assert(memcmp(restored.video, machine->video, sizeof(machine->video)) == 0);
	assert(memcmp(restored.audio, machine->audio, sizeof(machine->audio)) == 0);
}

static void test_msx_layout(void) {
	static struct karashi_sg1000 machine;
	static uint8_t rom[0x8000];

	memset(rom, 0x00, sizeof(rom));
	memset(rom + 0x2000, 0xff, 0x2000);
	karashi_sg1000_init(&machine, rom, sizeof(rom), false);
	assert(machine.msx_layout);
	machine.cpu.bus.write(&machine, 0x2345, 0x5a);
	assert(machine.cpu.bus.read(&machine, 0x2345) == 0x5a);
	machine.cpu.bus.write(&machine, 0xc000, 0x11);
	machine.cpu.bus.write(&machine, 0xc400, 0x22);
	assert(machine.cpu.bus.read(&machine, 0xc000) == 0x11 && machine.cpu.bus.read(&machine, 0xe400) == 0x22);
	rom[0x2100] = 0x12;
	karashi_sg1000_init(&machine, rom, sizeof(rom), false);
	assert(!machine.msx_layout);
	machine.cpu.bus.write(&machine, 0x2345, 0x5a);
	assert(machine.cpu.bus.read(&machine, 0x2345) == 0xff);
	machine.cpu.bus.write(&machine, 0xc000, 0x11);
	machine.cpu.bus.write(&machine, 0xc400, 0x22);
	assert(machine.cpu.bus.read(&machine, 0xc000) == 0x22);
}

static void test_sc3000(void) {
	static struct karashi_sg1000 machine;
	static uint8_t rom[0x4000];

	memset(rom, 0, sizeof(rom));
	karashi_sg1000_init(&machine, rom, sizeof(rom), true);
	assert(machine.sc3000);
	machine.cpu.bus.write(&machine, 0xc123, 0x5a);
	assert(machine.cpu.bus.read(&machine, 0xc923) == 0x5a);
	machine.cpu.bus.io_write(&machine, 0xdf, 0x92);
	karashi_sg1000_set_key(&machine, 0, 0, true);
	machine.cpu.bus.io_write(&machine, 0xde, 0);
	assert((machine.cpu.bus.io_read(&machine, 0xdc) & 1) == 0);
	machine.cpu.bus.io_write(&machine, 0xde, 1);
	assert((machine.cpu.bus.io_read(&machine, 0xdc) & 1) != 0);
	karashi_sg1000_set_key(&machine, 6, 11, true);
	machine.cpu.bus.io_write(&machine, 0xde, 6);
	assert((machine.cpu.bus.io_read(&machine, 0xdd) & 8) == 0);
	karashi_sg1000_clear_keys(&machine);
	assert((machine.cpu.bus.io_read(&machine, 0xdd) & 8) != 0);
	karashi_sg1000_set_controller(&machine, 0, 0x3f);
	karashi_sg1000_set_controller(&machine, 1, 0x3f);
	machine.cpu.bus.io_write(&machine, 0xde, 7);
	assert(machine.cpu.bus.io_read(&machine, 0xdc) == 0);
	assert((machine.cpu.bus.io_read(&machine, 0xdd) & 0x0f) == 0);
	machine.cpu.bus.io_write(&machine, 0xdf, 0x0c);
	assert((machine.ppi_port_c & 0x40) == 0);
	machine.cpu.bus.io_write(&machine, 0xdf, 0x0d);
	assert((machine.ppi_port_c & 0x40) != 0);
}

int main(int argc, char **argv) {
	test_msx_layout();
	test_sc3000();
	static struct karashi_sg1000 machine;
	uint8_t rom[512];
	bool audio_changed = false;
	unsigned frame;

	build_diagnostic(rom, sizeof(rom));
	if (argc == 3 && strcmp(argv[1], "--write-rom") == 0) {
		FILE *output_file = fopen(argv[2], "wb");
		if (!output_file) return 1;
		if (fwrite(rom, 1, sizeof(rom), output_file) != sizeof(rom)) {
			fclose(output_file);
			return 1;
		}
		return fclose(output_file) == 0 ? 0 : 1;
	}
	karashi_sg1000_init(&machine, rom, sizeof(rom), false);
	karashi_sg1000_run_frame(&machine);
	karashi_sg1000_run_frame(&machine);
	assert(machine.video[0] == KARASHI_PIXEL(0xff, 0xff, 0xff));
	assert(machine.video[1] == KARASHI_PIXEL(0x54, 0x55, 0xed));
	assert(machine.audio_frames >= 800 && machine.audio_frames <= 802);
	for (frame = 1; frame < machine.audio_frames; frame++) {
		if (machine.audio[frame * 2] != machine.audio[(frame - 1) * 2]) audio_changed = true;
	}
	assert(audio_changed);
	assert(machine.cpu.cycles >= 59736);
	assert(machine.cpu.halted);
	assert(!karashi_tms9918_interrupt(&machine.vdp));
	karashi_sg1000_set_controller(&machine, 0, 0x11);
	assert(machine.controller[0] == 0x11);
	machine.cpu.bus.write(&machine, 0x8123, 0x5a);
	assert(machine.cpu.bus.read(&machine, 0xa123) == 0x5a);
	machine.cpu.bus.write(&machine, 0x0100, 0x77);
	assert(machine.cpu.bus.read(&machine, 0x0100) == rom[0x0100]);
	test_state(&machine);
	puts("SG-1000 diagnostic ROM video, audio, VBlank interrupt and state: pass");
	return 0;
}
