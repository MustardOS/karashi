#ifndef KARASHI_SH2_H
#define KARASHI_SH2_H

#include "state.h"

#include <stdbool.h>
#include <stdint.h>

#define KARASHI_SH2_CACHE_RAM 4096

enum karashi_sh2_fault {
	KARASHI_SH2_FAULT_NONE = 0,
	KARASHI_SH2_FAULT_ADDRESS,
	KARASHI_SH2_FAULT_ILLEGAL
};

typedef uint8_t (*karashi_sh2_read8_t)(void *context, uint32_t address);
typedef void (*karashi_sh2_write8_t)(void *context, uint32_t address, uint8_t value);
typedef uint16_t (*karashi_sh2_read16_t)(void *context, uint32_t address);
typedef void (*karashi_sh2_write16_t)(void *context, uint32_t address, uint16_t value);
typedef uint32_t (*karashi_sh2_read32_t)(void *context, uint32_t address);
typedef void (*karashi_sh2_write32_t)(void *context, uint32_t address, uint32_t value);
typedef void (*karashi_sh2_serial_t)(void *context, uint8_t value);

struct karashi_sh2_bus {
	void *context;
	karashi_sh2_read8_t read;
	karashi_sh2_write8_t write;
	karashi_sh2_read16_t read16;
	karashi_sh2_write16_t write16;
	karashi_sh2_read32_t read32;
	karashi_sh2_write32_t write32;
	karashi_sh2_serial_t serial;
	const uint8_t *boot;
	uint32_t boot_mask;
	uint8_t *direct[32];
	uint32_t direct_mask[32];
	bool direct_write[32];
};

struct karashi_sh2_dmac {
	uint32_t source;
	uint32_t destination;
	uint32_t count;
	uint32_t control;
	uint32_t vector;
	uint8_t request;
};

struct karashi_sh2 {
	uint32_t r[16];
	uint32_t pc;
	uint32_t pr;
	uint32_t gbr;
	uint32_t vbr;
	uint32_t mach;
	uint32_t macl;
	uint32_t sr;
	uint32_t delay_target;
	uint64_t cycles;
	uint16_t opcode;
	enum karashi_sh2_fault fault;
	bool delay_pending;
	bool stopped;
	bool address_error;
	bool idle;
	bool interrupt_check;
	uint32_t timer_debt;
	uint32_t timer_limit;
	bool polling;
	uint64_t stores;
	uint64_t budget;
	uint32_t poll_pc;
	uint32_t poll_sr;
	uint32_t poll_regs[16];
	uint64_t poll_stores;
	uint32_t delay_loop_pc;
	uint8_t delay_loop_reg;
	uint8_t delay_loop_cost;
	uint8_t irl;
	uint32_t divisor;
	uint32_t dividend_high;
	uint32_t dividend_low;
	uint32_t divide_vector;
	uint16_t divide_control;
	struct karashi_sh2_dmac dma[2];
	uint16_t dma_operation;
	uint16_t frt_counter;
	uint16_t frt_compare[2];
	uint8_t frt_enable;
	uint8_t frt_status;
	uint8_t frt_control;
	uint8_t frt_output;
	uint8_t frt_temp;
	uint32_t frt_clock;
	uint8_t wdt_control;
	uint8_t wdt_counter;
	uint8_t wdt_reset;
	uint32_t wdt_clock;
	uint16_t interrupt_control;
	uint16_t priority_a;
	uint16_t priority_b;
	uint16_t vector_a;
	uint16_t vector_b;
	uint16_t vector_c;
	uint16_t vector_d;
	uint16_t vector_wdt;
	uint8_t cache_control;
	uint8_t serial[6];
	uint8_t serial_shift;
	bool serial_busy;
	uint32_t serial_countdown;
	uint16_t bus_control[8];
	uint8_t cache_ram[KARASHI_SH2_CACHE_RAM];
	struct karashi_sh2_bus bus;
};

void karashi_sh2_init(struct karashi_sh2 *cpu, const struct karashi_sh2_bus *bus);
void karashi_sh2_reset(struct karashi_sh2 *cpu);
unsigned karashi_sh2_step(struct karashi_sh2 *cpu);
void karashi_sh2_run(struct karashi_sh2 *cpu, uint64_t target);
void karashi_sh2_set_irl(struct karashi_sh2 *cpu, unsigned level);
void karashi_sh2_skip_idle(struct karashi_sh2 *cpu, uint64_t target);
void karashi_sh2_wait(struct karashi_sh2 *cpu, uint64_t target);
bool karashi_sh2_interrupt_ready(const struct karashi_sh2 *cpu);
void karashi_sh2_forget_loops(struct karashi_sh2 *cpu);
bool karashi_sh2_dreq(struct karashi_sh2 *cpu, unsigned channel);
void karashi_sh2_serial_receive(struct karashi_sh2 *cpu, uint8_t value);
bool karashi_sh2_save(const struct karashi_sh2 *cpu, struct karashi_state_writer *writer);
bool karashi_sh2_load(struct karashi_sh2 *cpu, struct karashi_state_reader *reader);

#endif
