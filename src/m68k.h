#ifndef KARASHI_M68K_H
#define KARASHI_M68K_H

#include "state.h"

#include <stdbool.h>
#include <stdint.h>

enum karashi_m68k_fault {
	KARASHI_M68K_FAULT_NONE = 0,
	KARASHI_M68K_FAULT_ADDRESS,
	KARASHI_M68K_FAULT_ILLEGAL
};

typedef uint8_t (*karashi_m68k_read_t)(void *context, uint32_t address);
typedef void (*karashi_m68k_write_t)(void *context, uint32_t address, uint8_t value);
typedef uint16_t (*karashi_m68k_read16_t)(void *context, uint32_t address);
typedef void (*karashi_m68k_write16_t)(void *context, uint32_t address, uint16_t value);
typedef void (*karashi_m68k_ack_t)(void *context, unsigned level);

struct karashi_m68k_bus {
	void *context;
	karashi_m68k_read_t read;
	karashi_m68k_write_t write;
	karashi_m68k_read16_t read16;
	karashi_m68k_write16_t write16;
	karashi_m68k_ack_t acknowledge;
	bool tas_read_only;
	const uint8_t *read_map[256];
	uint8_t *write_map[256];
};

struct karashi_m68k {
	uint32_t d[8];
	uint32_t a[8];
	uint32_t pc;
	uint32_t instruction_pc;
	uint32_t usp;
	uint32_t ssp;
	uint64_t cycles;
	uint16_t sr;
	uint8_t irq_level;
	enum karashi_m68k_fault fault;
	bool stopped;
	uint32_t illegal_count;
	uint32_t address_error_count;
	uint32_t first_illegal_pc;
	uint16_t first_illegal_opcode;
	uint32_t error_address;
	uint32_t error_pc;
	uint16_t error_status;
	uint16_t error_opcode;
	uint16_t opcode;
	uint16_t prefetch[2];
	uint32_t prefetch_address;
	unsigned extra;
	uint8_t prefetch_count;
	bool address_error;
	bool error_write;
	bool error_instruction;
	bool trace_pending;
	uint64_t bus_events;
	uint64_t loop_cycles;
	uint64_t loop_events;
	uint32_t loop_pc;
	uint32_t loop_regs[16];
	uint16_t loop_sr;
	bool loop_valid;
	bool loop_self;
	struct karashi_m68k_bus bus;
};

void karashi_m68k_init(struct karashi_m68k *cpu, const struct karashi_m68k_bus *bus);
void karashi_m68k_reset(struct karashi_m68k *cpu);
unsigned karashi_m68k_step(struct karashi_m68k *cpu);
void karashi_m68k_run(struct karashi_m68k *cpu, uint64_t limit);
void karashi_m68k_irq(struct karashi_m68k *cpu, unsigned level);
void karashi_m68k_set_irq(struct karashi_m68k *cpu, unsigned level);
bool karashi_m68k_save(const struct karashi_m68k *cpu, struct karashi_state_writer *writer);
bool karashi_m68k_load(struct karashi_m68k *cpu, struct karashi_state_reader *reader);

#endif
