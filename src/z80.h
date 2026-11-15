#ifndef KARASHI_Z80_H
#define KARASHI_Z80_H

#include "state.h"

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t (*karashi_z80_read_t)(void *context, uint16_t address);
typedef void (*karashi_z80_write_t)(void *context, uint16_t address, uint8_t value);
typedef uint8_t (*karashi_z80_io_read_t)(void *context, uint16_t port);
typedef void (*karashi_z80_io_write_t)(void *context, uint16_t port, uint8_t value);

struct karashi_z80_bus {
	void *context;
	karashi_z80_read_t read;
	karashi_z80_write_t write;
	karashi_z80_io_read_t io_read;
	karashi_z80_io_write_t io_write;
};

struct karashi_z80 {
	uint16_t af;
	uint16_t bc;
	uint16_t de;
	uint16_t hl;
	uint16_t af_alt;
	uint16_t bc_alt;
	uint16_t de_alt;
	uint16_t hl_alt;
	uint16_t ix;
	uint16_t iy;
	uint16_t sp;
	uint16_t pc;
	uint64_t cycles;
	uint8_t i;
	uint8_t r;
	uint8_t interrupt_mode;
	uint8_t ei_delay;
	bool iff1;
	bool iff2;
	bool halted;
	bool irq_pending;
	bool nmi_pending;
	bool prefetched_valid;
	bool prefix_active;
	uint8_t prefetched_opcode;
	struct karashi_z80_bus bus;
};

void karashi_z80_init(struct karashi_z80 *cpu, const struct karashi_z80_bus *bus);
void karashi_z80_reset(struct karashi_z80 *cpu);
unsigned karashi_z80_step(struct karashi_z80 *cpu);
void karashi_z80_irq(struct karashi_z80 *cpu, bool asserted);
void karashi_z80_nmi(struct karashi_z80 *cpu);
bool karashi_z80_save(const struct karashi_z80 *cpu, struct karashi_state_writer *writer);
bool karashi_z80_load(struct karashi_z80 *cpu, struct karashi_state_reader *reader);

#endif
