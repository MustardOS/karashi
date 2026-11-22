#include "m68k.h"

#include <string.h>

enum {
	FLAG_C = 0x0001,
	FLAG_V = 0x0002,
	FLAG_Z = 0x0004,
	FLAG_N = 0x0008,
	FLAG_X = 0x0010,
	SR_SUPERVISOR = 0x2000,
	SR_TRACE = 0x8000,
	SR_IMPLEMENTED = 0xa71f,
	ADDRESS_MASK = 0x00ffffff
};

enum {
	VALID_ALL = 0xfff,
	VALID_DATA = 0xffd,
	VALID_DATA_ALTERABLE = 0x1fd,
	VALID_MEMORY_ALTERABLE = 0x1fc,
	VALID_ALTERABLE = 0x1ff,
	VALID_CONTROL = 0x7e4,
	VALID_MOVEM_STORE = 0x1f4,
	VALID_MOVEM_LOAD = 0x7ec
};

enum ea_kind {
	EA_DATA,
	EA_ADDRESS,
	EA_MEMORY,
	EA_IMMEDIATE
};

struct effective_address {
	enum ea_kind kind;
	unsigned reg;
	unsigned index;
	uint32_t address;
	uint32_t read_pc;
	uint32_t write_pc;
};

static const uint8_t ea_time_short[12] = {0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4};
static const uint8_t ea_time_long[12] = {0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8};

static void address_fault(struct karashi_m68k *cpu, uint32_t address, bool write, bool instruction) {
	if (cpu->address_error) return;
	cpu->address_error = true;
	cpu->error_address = address;
	cpu->error_pc = write ? cpu->pc + 2 : cpu->instruction_pc + 2;
	cpu->error_status = cpu->sr;
	cpu->error_opcode = cpu->opcode;
	cpu->error_write = write;
	cpu->error_instruction = instruction;
}

static uint8_t read8(struct karashi_m68k *cpu, uint32_t address) {
	const uint8_t *page;

	if (cpu->address_error) return 0;
	address &= ADDRESS_MASK;
	page = cpu->bus.read_map[address >> 16];
	if (page) return page[address & 0xffff];
	cpu->bus_events++;
	return cpu->bus.read ? cpu->bus.read(cpu->bus.context, address) : 0xff;
}

static void write8(struct karashi_m68k *cpu, uint32_t address, uint8_t value) {
	uint8_t *page;

	if (cpu->address_error) return;
	address &= ADDRESS_MASK;
	page = cpu->bus.write_map[address >> 16];
	cpu->bus_events++;
	if (page) page[address & 0xffff] = value;
	else if (cpu->bus.write) cpu->bus.write(cpu->bus.context, address, value);
}

static uint16_t read16_access(struct karashi_m68k *cpu, uint32_t address, bool instruction) {
	const uint8_t *page;
	uint32_t logical = address;

	if (cpu->address_error) return 0;
	address &= ADDRESS_MASK;
	if (logical & 1) {
		address_fault(cpu, logical, false, instruction);
		return 0;
	}
	page = cpu->bus.read_map[address >> 16];
	if (page) return (uint16_t) ((uint16_t) page[address & 0xffff] << 8 | page[(address & 0xffff) + 1]);
	cpu->bus_events++;
	if (cpu->bus.read16) return cpu->bus.read16(cpu->bus.context, address);
	return (uint16_t) ((uint16_t) read8(cpu, address) << 8 | read8(cpu, address + 1));
}

static uint16_t read16(struct karashi_m68k *cpu, uint32_t address) {
	return read16_access(cpu, address, false);
}

static uint16_t read16_instruction_aligned(struct karashi_m68k *cpu, uint32_t address) {
	const uint8_t *page = cpu->bus.read_map[address >> 16];

	if (page) return (uint16_t) ((uint16_t) page[address & 0xffff] << 8 | page[(address & 0xffff) + 1]);
	cpu->bus_events++;
	if (cpu->bus.read16) return cpu->bus.read16(cpu->bus.context, address);
	return (uint16_t) ((uint16_t) read8(cpu, address) << 8 | read8(cpu, address + 1));
}

static uint32_t read32(struct karashi_m68k *cpu, uint32_t address) {
	uint32_t high = read16(cpu, address);
	return high << 16 | read16(cpu, address + 2);
}

static void write16(struct karashi_m68k *cpu, uint32_t address, uint16_t value) {
	uint32_t logical = address;

	address &= ADDRESS_MASK;
	if (logical & 1) {
		address_fault(cpu, logical, true, false);
		return;
	}
	if (cpu->address_error) return;
	cpu->bus_events++;
	if (cpu->bus.write_map[address >> 16]) {
		uint8_t *page = cpu->bus.write_map[address >> 16];
		page[address & 0xffff] = (uint8_t) (value >> 8);
		page[(address & 0xffff) + 1] = (uint8_t) value;
		return;
	}
	if (cpu->bus.write16) {
		cpu->bus.write16(cpu->bus.context, address, value);
		return;
	}
	write8(cpu, address, (uint8_t) (value >> 8));
	write8(cpu, address + 1, (uint8_t) value);
}

static void write32(struct karashi_m68k *cpu, uint32_t address, uint32_t value) {
	write16(cpu, address, (uint16_t) (value >> 16));
	write16(cpu, address + 2, (uint16_t) value);
}

static void fill_prefetch(struct karashi_m68k *cpu) {
	if (cpu->fault != KARASHI_M68K_FAULT_NONE) return;
	if (cpu->prefetch_count && cpu->prefetch_address != cpu->pc) cpu->prefetch_count = 0;
	if (!cpu->prefetch_count) cpu->prefetch_address = cpu->pc;
	while (cpu->prefetch_count < 2 && !cpu->address_error) {
		uint32_t address = (cpu->prefetch_address + cpu->prefetch_count * 2) & ADDRESS_MASK;
		cpu->prefetch[cpu->prefetch_count++] = read16_access(cpu, address, true);
	}
}

static uint16_t fetch16(struct karashi_m68k *cpu) {
	uint32_t address;
	uint16_t value;

	if (cpu->prefetch_count < 2 || cpu->prefetch_address != cpu->pc) fill_prefetch(cpu);
	if (cpu->address_error) return 0;
	value = cpu->prefetch[0];
	cpu->prefetch[0] = cpu->prefetch[1];
	cpu->prefetch_count--;
	cpu->prefetch_address = (cpu->prefetch_address + 2) & ADDRESS_MASK;
	cpu->pc = (cpu->pc + 2) & ADDRESS_MASK;
	address = (cpu->prefetch_address + cpu->prefetch_count * 2) & ADDRESS_MASK;
	cpu->prefetch[cpu->prefetch_count++] = read16_instruction_aligned(cpu, address);
	return value;
}

static uint32_t fetch32(struct karashi_m68k *cpu) {
	uint32_t high = fetch16(cpu);
	return high << 16 | fetch16(cpu);
}

static uint32_t size_mask(unsigned size) {
	if (size == 1) return 0xff;
	if (size == 2) return 0xffff;
	return UINT32_MAX;
}

static uint32_t size_sign(unsigned size) {
	if (size == 1) return 0x80;
	if (size == 2) return 0x8000;
	return 0x80000000;
}

static uint32_t read_sized(struct karashi_m68k *cpu, uint32_t address, unsigned size) {
	if (size == 1) return read8(cpu, address);
	if (size == 2) return read16(cpu, address);
	return read32(cpu, address);
}

static void write_sized(struct karashi_m68k *cpu, uint32_t address, unsigned size, uint32_t value) {
	if (size == 1) write8(cpu, address, (uint8_t) value);
	else if (size == 2) write16(cpu, address, (uint16_t) value);
	else write32(cpu, address, value);
}

static uint32_t increment_for(unsigned reg, unsigned size) {
	return size == 1 && reg == 7 ? 2 : size;
}

static uint32_t index_value(const struct karashi_m68k *cpu, uint16_t extension) {
	unsigned reg = (extension >> 12) & 7;
	uint32_t value = extension & 0x8000 ? cpu->a[reg] : cpu->d[reg];
	return extension & 0x0800 ? value : (uint32_t) (int32_t) (int16_t) value;
}

static unsigned ea_index(unsigned mode, unsigned reg) {
	return mode < 7 ? mode : 7 + reg;
}

static bool ea_valid(unsigned mode, unsigned reg, unsigned valid) {
	unsigned index = ea_index(mode, reg);
	return index < 12 && (valid >> index & 1);
}

static uint32_t memory_address(struct karashi_m68k *cpu, unsigned mode, unsigned reg, unsigned size) {
	uint32_t base;
	uint16_t extension;

	switch (ea_index(mode, reg)) {
		case 2: return cpu->a[reg];
		case 3:
			base = cpu->a[reg];
			cpu->a[reg] += increment_for(reg, size);
			return base;
		case 4:
			cpu->a[reg] -= increment_for(reg, size);
			return cpu->a[reg];
		case 5: return cpu->a[reg] + (uint32_t) (int32_t) (int16_t) fetch16(cpu);
		case 6:
			extension = fetch16(cpu);
			return cpu->a[reg] + index_value(cpu, extension) + (uint32_t) (int32_t) (int8_t) extension;
		case 7: return (uint32_t) (int32_t) (int16_t) fetch16(cpu);
		case 8: return fetch32(cpu);
		case 9:
			base = cpu->pc;
			return base + (uint32_t) (int32_t) (int16_t) fetch16(cpu);
		default:
			base = cpu->pc;
			extension = fetch16(cpu);
			return base + index_value(cpu, extension) + (uint32_t) (int32_t) (int8_t) extension;
	}
}

static bool resolve_ea(
	struct karashi_m68k *cpu, unsigned mode, unsigned reg, unsigned size, unsigned valid,
	struct effective_address *ea
) {
	unsigned index = ea_index(mode, reg);
	uint32_t stage_pc = cpu->pc;

	if (index > 11 || !(valid >> index & 1)) return false;
	cpu->extra += size == 4 ? ea_time_long[index] : ea_time_short[index];
	ea->reg = reg;
	ea->index = index;
	ea->address = 0;
	ea->read_pc = stage_pc;
	ea->write_pc = stage_pc + 2;
	if (index == 0) ea->kind = EA_DATA;
	else if (index == 1) ea->kind = EA_ADDRESS;
	else if (index == 11) {
		ea->kind = EA_IMMEDIATE;
		ea->address = size == 4 ? fetch32(cpu) : fetch16(cpu) & size_mask(size);
	} else {
		ea->kind = EA_MEMORY;
		ea->address = memory_address(cpu, mode, reg, size);
		if (index == 4 && size == 2) ea->read_pc = stage_pc + 2;
		else if (index == 7 || index == 8) ea->read_pc = cpu->pc;
	}
	return true;
}

static uint32_t ea_read(struct karashi_m68k *cpu, const struct effective_address *ea, unsigned size) {
	switch (ea->kind) {
		case EA_DATA: return cpu->d[ea->reg] & size_mask(size);
		case EA_ADDRESS: return cpu->a[ea->reg] & size_mask(size);
		case EA_MEMORY: {
			uint32_t value = read_sized(cpu, ea->address, size);
			if (cpu->address_error) {
				cpu->error_pc = ea->read_pc;
				if (ea->index == 3 && size == 4) cpu->a[ea->reg] -= 4;
				if (ea->index == 9 || ea->index == 10) cpu->error_instruction = true;
			}
			return value;
		}
		default: return ea->address;
	}
}

static void ea_write(
	struct karashi_m68k *cpu, const struct effective_address *ea, unsigned size, uint32_t value
) {
	uint32_t mask = size_mask(size);

	if (ea->kind == EA_DATA) cpu->d[ea->reg] = (cpu->d[ea->reg] & ~mask) | (value & mask);
	else if (ea->kind == EA_ADDRESS) cpu->a[ea->reg] = size == 2 ? (uint32_t) (int32_t) (int16_t) value : value;
	else if (ea->kind == EA_MEMORY) {
		if (ea->index == 4 && size == 4) {
			write16(cpu, ea->address + 2, (uint16_t) value);
			if (!cpu->address_error) write16(cpu, ea->address, (uint16_t) (value >> 16));
		} else {
			write_sized(cpu, ea->address, size, value);
		}
		if (cpu->address_error) {
			cpu->error_pc = ea->write_pc;
			if (ea->index == 3) cpu->a[ea->reg] -= increment_for(ea->reg, size);
			else if (ea->index == 4 && size == 4) cpu->a[ea->reg] += 4;
			if (ea->index == 4 && size == 2 && cpu->prefetch_count) cpu->error_opcode = cpu->prefetch[0];
		}
	}
}

static void set_logic_flags(struct karashi_m68k *cpu, uint32_t value, unsigned size) {
	uint32_t result = value & size_mask(size);

	cpu->sr &= (uint16_t) ~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (!result) cpu->sr |= FLAG_Z;
	if (result & size_sign(size)) cpu->sr |= FLAG_N;
}

static uint32_t add_value(
	struct karashi_m68k *cpu, uint32_t destination, uint32_t source, unsigned extend, unsigned size,
	bool sticky_zero
) {
	uint32_t mask = size_mask(size);
	uint32_t sign = size_sign(size);
	uint64_t wide = (uint64_t) (destination & mask) + (source & mask) + extend;
	uint32_t result = (uint32_t) wide & mask;
	uint16_t flags = 0;

	if (result & sign) flags |= FLAG_N;
	if (sticky_zero ? !result && (cpu->sr & FLAG_Z) : !result) flags |= FLAG_Z;
	if (~(destination ^ source) & (destination ^ result) & sign) flags |= FLAG_V;
	if (wide > mask) flags |= FLAG_X | FLAG_C;
	cpu->sr = (uint16_t) ((cpu->sr & ~0x1f) | flags);
	return result;
}

static uint32_t subtract_value(
	struct karashi_m68k *cpu, uint32_t destination, uint32_t source, unsigned extend, unsigned size,
	bool sticky_zero, bool set_extend
) {
	uint32_t mask = size_mask(size);
	uint32_t sign = size_sign(size);
	uint32_t lhs = destination & mask;
	uint32_t rhs = source & mask;
	uint32_t result = (lhs - rhs - extend) & mask;
	uint16_t flags = set_extend ? 0 : cpu->sr & FLAG_X;

	if (result & sign) flags |= FLAG_N;
	if (sticky_zero ? !result && (cpu->sr & FLAG_Z) : !result) flags |= FLAG_Z;
	if ((lhs ^ rhs) & (lhs ^ result) & sign) flags |= FLAG_V;
	if ((uint64_t) rhs + extend > lhs) flags |= set_extend ? FLAG_X | FLAG_C : FLAG_C;
	cpu->sr = (uint16_t) ((cpu->sr & ~0x1f) | flags);
	return result;
}

static uint8_t bcd_add(struct karashi_m68k *cpu, unsigned destination, unsigned source) {
	unsigned extend = cpu->sr & FLAG_X ? 1 : 0;
	unsigned binary = (destination & 0xff) + (source & 0xff) + extend;
	unsigned result = (destination & 15) + (source & 15) + extend;
	bool carry;

	if (result > 9) result += 6;
	result += (destination & 0xf0) + (source & 0xf0);
	carry = result > 0x9f;
	if (carry) result -= 0xa0;
	cpu->sr &= (uint16_t) ~(FLAG_X | FLAG_C | FLAG_N | FLAG_V);
	if (carry) cpu->sr |= FLAG_X | FLAG_C;
	if (~binary & result & 0x80) cpu->sr |= FLAG_V;
	if (result & 0xff) cpu->sr &= (uint16_t) ~FLAG_Z;
	if (result & 0x80) cpu->sr |= FLAG_N;
	return (uint8_t) result;
}

static uint8_t bcd_subtract(struct karashi_m68k *cpu, unsigned destination, unsigned source) {
	int extend = cpu->sr & FLAG_X ? 1 : 0;
	int binary = (int) (destination & 0xff) - (int) (source & 0xff) - extend;
	int result = (int) (destination & 15) - (int) (source & 15) - extend;
	bool borrow;

	result = result < 0 ? binary - 6 : binary;
	borrow = result < 0;
	if (binary < 0) result -= 0x60;
	cpu->sr &= (uint16_t) ~(FLAG_X | FLAG_C | FLAG_N | FLAG_V);
	if (borrow) cpu->sr |= FLAG_X | FLAG_C;
	if (binary & ~result & 0x80) cpu->sr |= FLAG_V;
	if (result & 0xff) cpu->sr &= (uint16_t) ~FLAG_Z;
	if (result & 0x80) cpu->sr |= FLAG_N;
	return (uint8_t) result;
}

static void push32(struct karashi_m68k *cpu, uint32_t value) {
	cpu->a[7] -= 4;
	write32(cpu, cpu->a[7], value);
}

static void push16(struct karashi_m68k *cpu, uint16_t value) {
	cpu->a[7] -= 2;
	write16(cpu, cpu->a[7], value);
}

static uint32_t pop32(struct karashi_m68k *cpu) {
	uint32_t value = read32(cpu, cpu->a[7]);
	cpu->a[7] += 4;
	return value;
}

static uint16_t pop16(struct karashi_m68k *cpu) {
	uint16_t value = read16(cpu, cpu->a[7]);
	cpu->a[7] += 2;
	return value;
}

static void set_status(struct karashi_m68k *cpu, uint16_t value) {
	bool old_supervisor = (cpu->sr & SR_SUPERVISOR) != 0;
	bool new_supervisor = (value & SR_SUPERVISOR) != 0;

	if (old_supervisor && !new_supervisor) {
		cpu->ssp = cpu->a[7];
		cpu->a[7] = cpu->usp;
	} else if (!old_supervisor && new_supervisor) {
		cpu->usp = cpu->a[7];
		cpu->a[7] = cpu->ssp;
	}
	cpu->sr = value & SR_IMPLEMENTED;
}

static unsigned take_exception(struct karashi_m68k *cpu, unsigned vector, unsigned cycles) {
	uint16_t old_status = cpu->sr;

	set_status(cpu, (uint16_t) ((old_status | SR_SUPERVISOR) & ~SR_TRACE));
	push32(cpu, cpu->pc);
	push16(cpu, old_status);
	cpu->pc = read32(cpu, vector * 4) & ADDRESS_MASK;
	cpu->stopped = false;
	return cycles;
}

static unsigned take_interrupt(struct karashi_m68k *cpu, unsigned level) {
	uint16_t old_status = cpu->sr;

	set_status(cpu, (uint16_t) ((old_status | SR_SUPERVISOR) & ~SR_TRACE));
	cpu->sr = (uint16_t) ((cpu->sr & ~0x0700) | (level << 8));
	push32(cpu, cpu->pc);
	push16(cpu, old_status);
	if (cpu->bus.acknowledge) cpu->bus.acknowledge(cpu->bus.context, level);
	else cpu->irq_level = 0;
	cpu->pc = read32(cpu, (24 + level) * 4) & ADDRESS_MASK;
	cpu->stopped = false;
	return 44;
}

static unsigned take_address_error(struct karashi_m68k *cpu) {
	uint16_t old_status = cpu->error_status;
	uint32_t address = cpu->error_address;
	uint16_t function = (uint16_t) ((old_status & SR_SUPERVISOR ? 4 : 0) | (cpu->error_instruction ? 2 : 1));
	uint16_t access = (uint16_t) ((cpu->error_opcode & 0xffe0) | (cpu->error_write ? 0 : 0x10) | function);

	cpu->address_error = false;
	cpu->address_error_count++;
	set_status(cpu, (uint16_t) ((old_status | SR_SUPERVISOR) & ~SR_TRACE));
	push32(cpu, cpu->error_pc);
	push16(cpu, old_status);
	push16(cpu, cpu->error_opcode);
	push32(cpu, address);
	push16(cpu, access);
	cpu->pc = read32(cpu, 3 * 4) & ADDRESS_MASK;
	if (cpu->address_error || (cpu->pc & 1)) cpu->fault = KARASHI_M68K_FAULT_ADDRESS;
	cpu->address_error = false;
	cpu->prefetch_count = 0;
	cpu->stopped = false;
	return 50;
}

static unsigned illegal(struct karashi_m68k *cpu) {
	unsigned top = cpu->opcode >> 12;

	if (!cpu->illegal_count) {
		cpu->first_illegal_pc = cpu->instruction_pc;
		cpu->first_illegal_opcode = cpu->opcode;
	}
	cpu->illegal_count++;
	cpu->pc = cpu->instruction_pc;
	return take_exception(cpu, top == 10 ? 10 : top == 15 ? 11 : 4, 34);
}

static unsigned privilege(struct karashi_m68k *cpu) {
	cpu->pc = cpu->instruction_pc;
	return take_exception(cpu, 8, 34);
}

static bool condition(const struct karashi_m68k *cpu, unsigned code) {
	bool c = (cpu->sr & FLAG_C) != 0;
	bool v = (cpu->sr & FLAG_V) != 0;
	bool z = (cpu->sr & FLAG_Z) != 0;
	bool n = (cpu->sr & FLAG_N) != 0;

	switch (code & 15) {
		case 0: return true;
		case 1: return false;
		case 2: return !c && !z;
		case 3: return c || z;
		case 4: return !c;
		case 5: return c;
		case 6: return !z;
		case 7: return z;
		case 8: return !v;
		case 9: return v;
		case 10: return !n;
		case 11: return n;
		case 12: return n == v;
		case 13: return n != v;
		case 14: return !z && n == v;
		default: return z || n != v;
	}
}

static unsigned execute_bit(struct karashi_m68k *cpu, uint16_t opcode, uint32_t bit, bool immediate) {
	unsigned operation = (opcode >> 6) & 3;
	unsigned mode = (opcode >> 3) & 7;
	unsigned size = mode == 0 ? 4 : 1;
	unsigned valid = operation ? VALID_DATA_ALTERABLE : immediate ? 0x7fd : VALID_DATA;
	struct effective_address ea;
	uint32_t value;
	uint32_t mask;
	unsigned cycles;

	cpu->extra = 0;
	if (!resolve_ea(cpu, mode, opcode & 7, 1, valid, &ea)) return illegal(cpu);
	bit &= size == 4 ? 31 : 7;
	mask = UINT32_C(1) << bit;
	value = ea_read(cpu, &ea, size);
	if (cpu->address_error) return 0;
	if (value & mask) cpu->sr &= (uint16_t) ~FLAG_Z;
	else cpu->sr |= FLAG_Z;
	if (operation == 0) cycles = mode == 0 || ea.kind == EA_IMMEDIATE ? 6 + cpu->extra : 4 + cpu->extra;
	else {
		if (operation == 1) value ^= mask;
		else if (operation == 2) value &= ~mask;
		else value |= mask;
		ea_write(cpu, &ea, size, value);
		if (mode) cycles = 8 + cpu->extra;
		else cycles = (operation == 2 ? 8 : 6) + (bit > 15 ? 2 : 0);
	}
	return cycles + (immediate ? 4 : 0);
}

static unsigned execute_movep(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned data = (opcode >> 9) & 7;
	unsigned size = opcode & 0x0040 ? 4 : 2;
	uint32_t address = cpu->a[opcode & 7] + (uint32_t) (int32_t) (int16_t) fetch16(cpu);
	uint32_t value = 0;
	unsigned index;

	if (opcode & 0x0080) {
		for (index = 0; index < size; index++) {
			write8(cpu, address + index * 2, (uint8_t) (cpu->d[data] >> ((size - 1 - index) * 8)));
		}
	} else {
		for (index = 0; index < size; index++) value = value << 8 | read8(cpu, address + index * 2);
		if (size == 2) cpu->d[data] = (cpu->d[data] & 0xffff0000) | value;
		else cpu->d[data] = value;
	}
	return size == 2 ? 16 : 24;
}

static unsigned execute_group0(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned mode = (opcode >> 3) & 7;
	unsigned reg = opcode & 7;
	unsigned operation = (opcode >> 9) & 7;
	unsigned size_code = (opcode >> 6) & 3;
	unsigned size;
	struct effective_address ea;
	uint32_t source;
	uint32_t value;
	uint32_t result;

	if (opcode & 0x0100) {
		if (mode == 1) return execute_movep(cpu, opcode);
		return execute_bit(cpu, opcode, cpu->d[operation], false);
	}
	if (operation == 4) return execute_bit(cpu, opcode, fetch16(cpu), true);
	if (operation == 7) return illegal(cpu);
	if ((opcode & 0x3f) == 0x3c && (operation == 0 || operation == 1 || operation == 5)) {
		uint16_t mask;
		if (size_code > 1) return illegal(cpu);
		if (size_code == 1 && !(cpu->sr & SR_SUPERVISOR)) return privilege(cpu);
		mask = size_code ? 0xffff : 0x001f;
		value = fetch16(cpu) & mask;
		if (operation == 0) result = cpu->sr | value;
		else if (operation == 1) result = cpu->sr & (value | (uint16_t) ~mask);
		else result = cpu->sr ^ value;
		set_status(cpu, (uint16_t) result);
		return 20;
	}
	if (size_code == 3) return illegal(cpu);
	size = 1u << size_code;
	if (!ea_valid(mode, reg, VALID_DATA_ALTERABLE)) return illegal(cpu);
	source = size == 4 ? fetch32(cpu) : fetch16(cpu) & size_mask(size);
	cpu->extra = 0;
	resolve_ea(cpu, mode, reg, size, VALID_DATA_ALTERABLE, &ea);
	value = ea_read(cpu, &ea, size);
	if (cpu->address_error) return 0;
	switch (operation) {
		case 0: result = value | source; set_logic_flags(cpu, result, size); break;
		case 1: result = value & source; set_logic_flags(cpu, result, size); break;
		case 2: result = subtract_value(cpu, value, source, 0, size, false, true); break;
		case 3: result = add_value(cpu, value, source, 0, size, false); break;
		case 5: result = value ^ source; set_logic_flags(cpu, result, size); break;
		default:
			(void) subtract_value(cpu, value, source, 0, size, false, false);
			if (mode == 0) return size == 4 ? 14 : 8;
			return (size == 4 ? 12 : 8) + cpu->extra;
	}
	ea_write(cpu, &ea, size, result);
	if (mode == 0) return size == 4 ? 16 : 8;
	return (size == 4 ? 20 : 12) + cpu->extra;
}

static unsigned execute_move(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned top = opcode >> 12;
	unsigned size = top == 1 ? 1 : top == 2 ? 4 : 2;
	unsigned destination_mode = (opcode >> 6) & 7;
	unsigned destination_reg = (opcode >> 9) & 7;
	struct effective_address source;
	struct effective_address destination;
	uint32_t value;

	cpu->extra = 0;
	if (!ea_valid(destination_mode, destination_reg, size == 1 ? VALID_DATA_ALTERABLE : VALID_ALTERABLE)
	    || !resolve_ea(cpu, (opcode >> 3) & 7, opcode & 7, size, size == 1 ? VALID_DATA : VALID_ALL, &source)) {
		return illegal(cpu);
	}
	value = ea_read(cpu, &source, size);
	if (cpu->address_error) return 4 + cpu->extra;
	resolve_ea(cpu, destination_mode, destination_reg, size, VALID_ALTERABLE, &destination);
	if (cpu->address_error) return 4 + cpu->extra;
	if (destination_mode != 1
	    && (size != 4 || destination_mode == 0 || destination_mode == 4
	        || (destination_mode == 7 && !(source.kind == EA_MEMORY && source.index == 3 && destination.index == 8))
	        || (source.kind == EA_MEMORY && destination_mode != 2 && destination_mode != 3
	            && !(source.index == 3 && destination.index == 8)))) {
		set_logic_flags(cpu, value, size);
	} else if (size == 4 && source.kind == EA_MEMORY
	           && (destination_mode == 2 || destination_mode == 3 || (source.index == 3 && destination.index == 8))) {
		uint16_t low = (uint16_t) value;
		cpu->sr &= (uint16_t) ~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
		if (!low) cpu->sr |= FLAG_Z;
		if (low & 0x8000) cpu->sr |= FLAG_N;
	} else if (size == 4 && source.kind != EA_MEMORY && (destination_mode == 5 || destination_mode == 6)) {
		cpu->sr &= (uint16_t) ~(FLAG_N | FLAG_Z);
		if (!value) cpu->sr |= FLAG_Z;
		if (value & 0x80000000) cpu->sr |= FLAG_N;
	}
	ea_write(cpu, &destination, size, value);
	if (cpu->address_error && source.kind < EA_MEMORY && destination.index == 8) {
		cpu->error_pc = cpu->pc;
	}
	if (!cpu->address_error && destination_mode != 1 && size == 4
	    && destination_mode != 0 && destination_mode != 4 && destination_mode != 7) {
		set_logic_flags(cpu, value, size);
	}
	return 4 + cpu->extra - (destination_mode == 4 ? 2 : 0);
}

static uint32_t control_address(struct karashi_m68k *cpu, unsigned mode, unsigned reg) {
	if (mode == 3 || mode == 4) return cpu->a[reg];
	return memory_address(cpu, mode, reg, 4);
}

static unsigned execute_movem(struct karashi_m68k *cpu, uint16_t opcode) {
	static const uint8_t load_time[11] = {0, 0, 12, 12, 0, 16, 18, 16, 20, 16, 18};
	static const uint8_t store_time[9] = {0, 0, 8, 0, 8, 12, 14, 12, 16};
	bool load = (opcode & 0x0400) != 0;
	unsigned size = opcode & 0x0040 ? 4 : 2;
	unsigned mode = (opcode >> 3) & 7;
	unsigned reg = opcode & 7;
	unsigned index = ea_index(mode, reg);
	uint16_t mask;
	uint32_t address;
	unsigned bit;
	unsigned count = 0;

	if (!ea_valid(mode, reg, load ? VALID_MOVEM_LOAD : VALID_MOVEM_STORE)) return illegal(cpu);
	mask = fetch16(cpu);
	address = control_address(cpu, mode, reg);
	if (!load && mode == 4) {
		for (bit = 0; bit < 16; bit++) {
			unsigned source = 15 - bit;
			if (!(mask >> bit & 1)) continue;
			address -= size;
			if (size == 4) {
				uint32_t value = source < 8 ? cpu->d[source] : cpu->a[source - 8];
				write16(cpu, address + 2, (uint16_t) value);
				if (!cpu->address_error) write16(cpu, address, (uint16_t) (value >> 16));
			} else {
				write16(cpu, address, (uint16_t) (source < 8 ? cpu->d[source] : cpu->a[source - 8]));
			}
			if (cpu->address_error) {
				cpu->error_pc = cpu->pc + 2;
				return 0;
			}
			count++;
		}
		cpu->a[reg] = address;
		return store_time[index] + count * size * 2;
	}
	for (bit = 0; bit < 16; bit++) {
		if (!(mask >> bit & 1)) continue;
		if (load) {
			uint32_t value = size == 2 ? (uint32_t) (int32_t) (int16_t) read16(cpu, address) : read32(cpu, address);
			if (cpu->address_error) {
				if (mode == 7 && (reg == 2 || reg == 3)) cpu->error_instruction = true;
				cpu->error_pc = cpu->pc + 2;
				return 0;
			}
			if (bit < 8) cpu->d[bit] = value;
			else cpu->a[bit - 8] = value;
		} else {
			write_sized(cpu, address, size, bit < 8 ? cpu->d[bit] : cpu->a[bit - 8]);
			if (cpu->address_error) {
				cpu->error_pc = cpu->pc + 2;
				return 0;
			}
		}
		address += size;
		count++;
	}
	if (load && mode == 3) cpu->a[reg] = address;
	return (load ? load_time[index] : store_time[index]) + count * size * 2;
}

static unsigned execute_system(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned reg = opcode & 7;
	uint16_t status;
	uint32_t value;

	if ((opcode & 0xfff0) == 0x4e40) return take_exception(cpu, 32 + (opcode & 15), 34);
	if ((opcode & 0xfff8) == 0x4e50) {
		int16_t displacement = (int16_t) fetch16(cpu);
		value = cpu->a[reg];
		cpu->a[7] -= 4;
		write32(cpu, cpu->a[7], value);
		cpu->a[reg] = cpu->a[7];
		cpu->a[7] += (uint32_t) (int32_t) displacement;
		return 16;
	}
	if ((opcode & 0xfff8) == 0x4e58) {
		uint32_t address = cpu->a[reg];
		value = read32(cpu, address);
		if (cpu->address_error) {
			cpu->error_pc = cpu->instruction_pc + 4;
			return 12;
		}
		cpu->a[7] = address + 4;
		cpu->a[reg] = value;
		return 12;
	}
	if ((opcode & 0xfff0) == 0x4e60) {
		if (!(cpu->sr & SR_SUPERVISOR)) return privilege(cpu);
		if (opcode & 8) cpu->a[reg] = cpu->usp;
		else cpu->usp = cpu->a[reg];
		return 4;
	}
	switch (opcode) {
		case 0x4e70:
			if (!(cpu->sr & SR_SUPERVISOR)) return privilege(cpu);
			return 132;
		case 0x4e71: return 4;
		case 0x4e72:
			if (!(cpu->sr & SR_SUPERVISOR)) return privilege(cpu);
			status = fetch16(cpu);
			set_status(cpu, status);
			cpu->prefetch[0] = opcode;
			cpu->prefetch[1] = status;
			cpu->prefetch_address = cpu->instruction_pc;
			cpu->prefetch_count = 2;
			cpu->stopped = true;
			return 4;
		case 0x4e73:
			if (!(cpu->sr & SR_SUPERVISOR)) return privilege(cpu);
			status = pop16(cpu);
			value = pop32(cpu);
			set_status(cpu, status);
			if (!cpu->address_error && (value & 1)) address_fault(cpu, value, false, true);
			cpu->pc = value & ADDRESS_MASK;
			return 20;
		case 0x4e75:
			value = pop32(cpu);
			if (!cpu->address_error && (value & 1)) address_fault(cpu, value, false, true);
			cpu->pc = value & ADDRESS_MASK;
			return 16;
		case 0x4e76:
			if (!(cpu->sr & FLAG_V)) return 4;
			return take_exception(cpu, 7, 34);
		case 0x4e77:
			status = pop16(cpu);
			cpu->sr = (uint16_t) ((cpu->sr & 0xff00) | (status & 0x1f));
			value = pop32(cpu);
			if (!cpu->address_error && (value & 1)) address_fault(cpu, value, false, true);
			cpu->pc = value & ADDRESS_MASK;
			return 20;
		default: return illegal(cpu);
	}
}

static unsigned execute_group4(struct karashi_m68k *cpu, uint16_t opcode) {
	static const uint8_t lea_time[11] = {0, 0, 4, 0, 0, 8, 12, 8, 12, 8, 12};
	static const uint8_t pea_time[11] = {0, 0, 12, 0, 0, 16, 20, 16, 20, 16, 20};
	static const uint8_t jmp_time[11] = {0, 0, 8, 0, 0, 10, 14, 10, 12, 10, 14};
	static const uint8_t jsr_time[11] = {0, 0, 16, 0, 0, 18, 22, 18, 20, 18, 22};
	unsigned mode = (opcode >> 3) & 7;
	unsigned reg = opcode & 7;
	unsigned index = ea_index(mode, reg);
	unsigned size_code = (opcode >> 6) & 3;
	unsigned size = 1u << size_code;
	struct effective_address ea = {0};
	uint32_t address;
	uint32_t value;

	cpu->extra = 0;
	if (opcode & 0x0100) {
		if ((opcode & 0x01c0) == 0x01c0) {
			if (!ea_valid(mode, reg, VALID_CONTROL)) return illegal(cpu);
			cpu->a[(opcode >> 9) & 7] = control_address(cpu, mode, reg);
			return lea_time[index];
		}
		if ((opcode & 0x01c0) == 0x0180) {
			int16_t bound;
			int16_t data;
			if (!resolve_ea(cpu, mode, reg, 2, VALID_DATA, &ea)) return illegal(cpu);
			bound = (int16_t) ea_read(cpu, &ea, 2);
			if (cpu->address_error) return 0;
			data = (int16_t) cpu->d[(opcode >> 9) & 7];
			cpu->sr &= (uint16_t) ~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
			if (data < 0) {
				int16_t difference = (int16_t) ((uint16_t) data - (uint16_t) bound);

				cpu->sr |= FLAG_N;
				return take_exception(cpu, 6, (difference > 0 ? 38 : 40) + cpu->extra);
			}
			if (data > bound) return take_exception(cpu, 6, 38 + cpu->extra);
			return 10 + cpu->extra;
		}
		return illegal(cpu);
	}
	switch ((opcode >> 9) & 7) {
		case 0:
			if (size_code == 3) {
				if (!resolve_ea(cpu, mode, reg, 2, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
				if (ea.kind == EA_MEMORY) (void) ea_read(cpu, &ea, 2);
				if (cpu->address_error) return 0;
				ea_write(cpu, &ea, 2, cpu->sr);
				return mode == 0 ? 6 : 8 + cpu->extra;
			}
			if (!resolve_ea(cpu, mode, reg, size, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
			value = ea_read(cpu, &ea, size);
			if (cpu->address_error) return 0;
			value = subtract_value(cpu, 0, value, cpu->sr & FLAG_X ? 1 : 0, size, true, true);
			ea_write(cpu, &ea, size, value);
			break;
		case 1:
			if (size_code == 3 || !resolve_ea(cpu, mode, reg, size, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
			if (ea.kind == EA_MEMORY) (void) ea_read(cpu, &ea, size);
			if (cpu->address_error) return 0;
			set_logic_flags(cpu, 0, size);
			ea_write(cpu, &ea, size, 0);
			break;
		case 2:
			if (size_code == 3) {
				if (!resolve_ea(cpu, mode, reg, 2, VALID_DATA, &ea)) return illegal(cpu);
				value = ea_read(cpu, &ea, 2);
				if (cpu->address_error) return 0;
				cpu->sr = (uint16_t) ((cpu->sr & 0xff00) | (value & 0x1f));
				return 12 + cpu->extra;
			}
			if (!resolve_ea(cpu, mode, reg, size, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
			value = ea_read(cpu, &ea, size);
			if (cpu->address_error) return 0;
			value = subtract_value(cpu, 0, value, 0, size, false, true);
			ea_write(cpu, &ea, size, value);
			break;
		case 3:
			if (size_code == 3) {
				if (!ea_valid(mode, reg, VALID_DATA)) return illegal(cpu);
				if (!(cpu->sr & SR_SUPERVISOR)) return privilege(cpu);
				resolve_ea(cpu, mode, reg, 2, VALID_DATA, &ea);
				value = ea_read(cpu, &ea, 2);
				if (cpu->address_error) return 0;
				set_status(cpu, (uint16_t) value);
				return 12 + cpu->extra;
			}
			if (!resolve_ea(cpu, mode, reg, size, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
			value = ea_read(cpu, &ea, size);
			if (cpu->address_error) return 0;
			value = ~value & size_mask(size);
			set_logic_flags(cpu, value, size);
			ea_write(cpu, &ea, size, value);
			break;
		case 4:
			if (size_code == 0) {
				if (!resolve_ea(cpu, mode, reg, 1, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
				value = ea_read(cpu, &ea, 1);
				if (cpu->address_error) return 0;
				ea_write(cpu, &ea, 1, bcd_subtract(cpu, 0, value));
				return mode == 0 ? 6 : 8 + cpu->extra;
			}
			if (mode == 0) {
				if (size_code == 1) {
					cpu->d[reg] = cpu->d[reg] << 16 | cpu->d[reg] >> 16;
					set_logic_flags(cpu, cpu->d[reg], 4);
				} else if (size_code == 2) {
					cpu->d[reg] = (cpu->d[reg] & 0xffff0000) | (uint16_t) (int16_t) (int8_t) cpu->d[reg];
					set_logic_flags(cpu, cpu->d[reg], 2);
				} else {
					cpu->d[reg] = (uint32_t) (int32_t) (int16_t) cpu->d[reg];
					set_logic_flags(cpu, cpu->d[reg], 4);
				}
				return 4;
			}
			if (size_code == 1) {
				if (!ea_valid(mode, reg, VALID_CONTROL)) return illegal(cpu);
				address = control_address(cpu, mode, reg);
				push32(cpu, address);
				return pea_time[index];
			}
			return execute_movem(cpu, opcode);
		case 5:
			if (size_code == 3) {
				if (!resolve_ea(cpu, mode, reg, 1, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
				value = ea_read(cpu, &ea, 1);
				if (cpu->address_error) return 0;
				set_logic_flags(cpu, value, 1);
				if (ea.kind == EA_DATA || !cpu->bus.tas_read_only) ea_write(cpu, &ea, 1, value | 0x80);
				return mode == 0 ? 4 : 10 + cpu->extra;
			}
			if (!resolve_ea(cpu, mode, reg, size, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
			value = ea_read(cpu, &ea, size);
			if (cpu->address_error) return 0;
			set_logic_flags(cpu, value, size);
			return 4 + cpu->extra;
		case 6:
			if (size_code < 2) return illegal(cpu);
			return execute_movem(cpu, opcode);
		default:
			if (size_code == 0) return illegal(cpu);
			if (size_code == 1) return execute_system(cpu, opcode);
			if (!ea_valid(mode, reg, VALID_CONTROL)) return illegal(cpu);
			address = control_address(cpu, mode, reg);
			if (size_code == 2) {
				if (address & 1) {
					address_fault(cpu, address, false, true);
					cpu->error_pc = cpu->pc;
					return jsr_time[index];
				}
				push32(cpu, cpu->pc);
				cpu->pc = address & ADDRESS_MASK;
				return jsr_time[index];
			}
			if (address & 1) address_fault(cpu, address, false, true);
			cpu->pc = address & ADDRESS_MASK;
			return jmp_time[index];
	}
	if (mode == 0) return size == 4 ? 6 : 4;
	return (size == 4 ? 12 : 8) + cpu->extra;
}

static unsigned execute_group5(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned mode = (opcode >> 3) & 7;
	unsigned reg = opcode & 7;
	unsigned size_code = (opcode >> 6) & 3;
	unsigned size = 1u << size_code;
	unsigned quick = (opcode >> 9) & 7;
	struct effective_address ea;
	uint32_t value;

	cpu->extra = 0;
	if (size_code == 3) {
		bool met = condition(cpu, (opcode >> 8) & 15);
		if (mode == 1) {
			uint32_t base = cpu->pc;
			int16_t displacement = (int16_t) fetch16(cpu);
			uint16_t counter;
			if (met) return 12;
			counter = (uint16_t) (cpu->d[reg] - 1);
			if (counter == 0xffff) {
				cpu->d[reg] = (cpu->d[reg] & 0xffff0000) | counter;
				return 14;
			}
			cpu->pc = (base + (uint32_t) (int32_t) displacement) & ADDRESS_MASK;
			if (cpu->pc & 1) {
				address_fault(cpu, cpu->pc, false, true);
				cpu->error_pc = cpu->instruction_pc + 4;
				return 10;
			}
			cpu->d[reg] = (cpu->d[reg] & 0xffff0000) | counter;
			return 10;
		}
		if (!resolve_ea(cpu, mode, reg, 1, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
		if (ea.kind == EA_MEMORY) (void) read8(cpu, ea.address);
		ea_write(cpu, &ea, 1, met ? 0xff : 0);
		return mode == 0 ? (met ? 6 : 4) : 8 + cpu->extra;
	}
	if (!quick) quick = 8;
	if ((mode == 1 && size == 1) || !resolve_ea(cpu, mode, reg, size, VALID_ALTERABLE, &ea)) return illegal(cpu);
	if (mode == 1) {
		cpu->a[reg] = opcode & 0x0100 ? cpu->a[reg] - quick : cpu->a[reg] + quick;
		return 8;
	}
	value = ea_read(cpu, &ea, size);
	if (cpu->address_error) return 0;
	value = opcode & 0x0100 ? subtract_value(cpu, value, quick, 0, size, false, true)
	                        : add_value(cpu, value, quick, 0, size, false);
	ea_write(cpu, &ea, size, value);
	if (mode == 0) return size == 4 ? 8 : 4;
	return (size == 4 ? 12 : 8) + cpu->extra;
}

static unsigned execute_branch(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned code = (opcode >> 8) & 15;
	uint32_t base = cpu->pc;
	bool word = (opcode & 0xff) == 0;
	int32_t displacement = word ? (int16_t) fetch16(cpu) : (int8_t) opcode;

	if (code == 1) {
		push32(cpu, cpu->pc);
		cpu->pc = (base + (uint32_t) displacement) & ADDRESS_MASK;
		if (!cpu->address_error && (cpu->pc & 1)) {
			address_fault(cpu, cpu->pc, false, true);
			cpu->error_pc = cpu->pc;
		}
		return 18;
	}
	if (!condition(cpu, code)) return word ? 12 : 8;
	cpu->pc = (base + (uint32_t) displacement) & ADDRESS_MASK;
	return 10;
}

static unsigned source_time(unsigned mode, unsigned reg, unsigned size, unsigned extra) {
	if (size != 4) return 4 + extra;
	return (mode < 2 || (mode == 7 && reg == 4) ? 8 : 6) + extra;
}

static unsigned execute_arithmetic(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned top = opcode >> 12;
	unsigned reg = (opcode >> 9) & 7;
	unsigned opmode = (opcode >> 6) & 7;
	unsigned mode = (opcode >> 3) & 7;
	unsigned source_reg = opcode & 7;
	unsigned size = 1u << (opmode & 3);
	bool subtract = top == 9 || top == 11;
	struct effective_address ea;
	uint32_t source;
	uint32_t value;

	cpu->extra = 0;
	if ((opmode & 3) == 3) {
		size = opmode == 3 ? 2 : 4;
		if (!resolve_ea(cpu, mode, source_reg, size, VALID_ALL, &ea)) return illegal(cpu);
		source = ea_read(cpu, &ea, size);
		if (cpu->address_error) return 0;
		if (size == 2) source = (uint32_t) (int32_t) (int16_t) source;
		if (top == 11) {
			(void) subtract_value(cpu, cpu->a[reg], source, 0, 4, false, false);
			return 6 + cpu->extra;
		}
		cpu->a[reg] = subtract ? cpu->a[reg] - source : cpu->a[reg] + source;
		if (size == 2) return 8 + cpu->extra;
		return source_time(mode, source_reg, 4, cpu->extra);
	}
	if (opmode < 3) {
		if (!resolve_ea(cpu, mode, source_reg, size, size == 1 ? VALID_DATA : VALID_ALL, &ea)) return illegal(cpu);
		source = ea_read(cpu, &ea, size);
		if (cpu->address_error) return 0;
		if (top == 11) {
			(void) subtract_value(cpu, cpu->d[reg], source, 0, size, false, false);
			return (size == 4 ? 6 : 4) + cpu->extra;
		}
		value = subtract ? subtract_value(cpu, cpu->d[reg], source, 0, size, false, true)
		                 : add_value(cpu, cpu->d[reg], source, 0, size, false);
		cpu->d[reg] = (cpu->d[reg] & ~size_mask(size)) | value;
		return source_time(mode, source_reg, size, cpu->extra);
	}
	if (top == 11) {
		if (mode == 1) {
			if (size == 4) {
				source = (uint32_t) read16(cpu, cpu->a[source_reg]) << 16;
				cpu->a[source_reg] += 2;
				if (!cpu->address_error) {
					source |= read16(cpu, cpu->a[source_reg]);
					cpu->a[source_reg] += 2;
				}
			} else {
				source = read_sized(cpu, cpu->a[source_reg], size);
				cpu->a[source_reg] += increment_for(source_reg, size);
			}
			if (cpu->address_error) {
				cpu->error_pc = cpu->instruction_pc + 4;
				return 0;
			}
			if (size == 4) {
				value = (uint32_t) read16(cpu, cpu->a[reg]) << 16;
				if (!cpu->address_error) {
					cpu->a[reg] += 2;
					value |= read16(cpu, cpu->a[reg]);
					if (!cpu->address_error) cpu->a[reg] += 2;
				}
			} else {
				value = read_sized(cpu, cpu->a[reg], size);
				if (!cpu->address_error) cpu->a[reg] += increment_for(reg, size);
			}
			if (cpu->address_error) {
				cpu->error_pc = cpu->instruction_pc + 4;
				return 0;
			}
			(void) subtract_value(cpu, value, source, 0, size, false, false);
			return size == 4 ? 20 : 12;
		}
		if (!resolve_ea(cpu, mode, source_reg, size, VALID_DATA_ALTERABLE, &ea)) return illegal(cpu);
		value = ea_read(cpu, &ea, size);
		if (cpu->address_error) return 0;
		value ^= cpu->d[reg] & size_mask(size);
		set_logic_flags(cpu, value, size);
		ea_write(cpu, &ea, size, value);
		if (mode == 0) return size == 4 ? 8 : 4;
		return (size == 4 ? 12 : 8) + cpu->extra;
	}
	if (mode < 2) {
		unsigned extend = cpu->sr & FLAG_X ? 1 : 0;
		if (mode == 0) {
			value = subtract ? subtract_value(cpu, cpu->d[reg], cpu->d[source_reg], extend, size, true, true)
			                 : add_value(cpu, cpu->d[reg], cpu->d[source_reg], extend, size, true);
			cpu->d[reg] = (cpu->d[reg] & ~size_mask(size)) | value;
			return size == 4 ? 8 : 4;
		}
		if (size == 4) {
			uint32_t address = cpu->a[source_reg];
			uint32_t low = read16(cpu, address - 2);
			if (cpu->address_error) {
				cpu->error_pc = cpu->instruction_pc + 4;
				return 0;
			}
			source = (uint32_t) read16(cpu, address - 4) << 16 | low;
			cpu->a[source_reg] = address - 4;
		} else {
			cpu->a[source_reg] -= increment_for(source_reg, size);
			source = read_sized(cpu, cpu->a[source_reg], size);
		}
		if (cpu->address_error) {
			cpu->error_pc = cpu->instruction_pc + 4;
			return 0;
		}
		if (size == 4) {
			uint32_t address = cpu->a[reg];
			uint32_t low = read16(cpu, address - 2);
			if (cpu->address_error) {
				cpu->error_pc = cpu->instruction_pc + 4;
				return 0;
			}
			value = (uint32_t) read16(cpu, address - 4) << 16 | low;
			cpu->a[reg] = address - 4;
		} else {
			cpu->a[reg] -= increment_for(reg, size);
			value = read_sized(cpu, cpu->a[reg], size);
		}
		if (cpu->address_error) {
			cpu->error_pc = cpu->instruction_pc + 4;
			return 0;
		}
		value = subtract ? subtract_value(cpu, value, source, extend, size, true, true)
		                 : add_value(cpu, value, source, extend, size, true);
		write_sized(cpu, cpu->a[reg], size, value);
		return size == 4 ? 30 : 18;
	}
	if (!resolve_ea(cpu, mode, source_reg, size, VALID_MEMORY_ALTERABLE, &ea)) return illegal(cpu);
	value = ea_read(cpu, &ea, size);
	if (cpu->address_error) return 0;
	value = subtract ? subtract_value(cpu, value, cpu->d[reg], 0, size, false, true)
	                 : add_value(cpu, value, cpu->d[reg], 0, size, false);
	ea_write(cpu, &ea, size, value);
	return (size == 4 ? 12 : 8) + cpu->extra;
}

static unsigned divide_unsigned_time(uint32_t dividend, uint32_t divisor) {
	uint32_t aligned = divisor << 16;
	unsigned cycles = 76;
	unsigned index;

	for (index = 0; index < 15; index++) {
		bool carry = (dividend & 0x80000000) != 0;
		dividend <<= 1;
		if (carry) dividend -= aligned;
		else if (dividend >= aligned) {
			dividend -= aligned;
			cycles += 2;
		} else cycles += 4;
	}
	return cycles;
}

static unsigned divide_signed_time(int32_t dividend, int16_t divisor) {
	uint32_t magnitude = dividend < 0 ? 0u - (uint32_t) dividend : (uint32_t) dividend;
	uint32_t divisor_magnitude = divisor < 0 ? (uint32_t) -(int32_t) divisor : (uint32_t) divisor;
	unsigned cycles = dividend < 0 ? 14 : 12;
	uint32_t quotient;
	unsigned bit;

	if ((magnitude >> 16) >= divisor_magnitude) return cycles + 4;
	cycles += 110;
	if (divisor >= 0) cycles += dividend >= 0 ? -2 : 2;
	quotient = magnitude / divisor_magnitude;
	for (bit = 1; bit < 16; bit++) {
		if (!(quotient >> bit & 1)) cycles += 2;
	}
	return cycles;
}

static unsigned execute_divide(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned reg = (opcode >> 9) & 7;
	bool is_signed = (opcode & 0x0100) != 0;
	struct effective_address ea;
	uint32_t divisor;
	unsigned cycles;

	cpu->extra = 0;
	if (!resolve_ea(cpu, (opcode >> 3) & 7, opcode & 7, 2, VALID_DATA, &ea)) return illegal(cpu);
	divisor = ea_read(cpu, &ea, 2);
	if (cpu->address_error) return 0;
	if (!divisor) {
		cpu->sr &= (uint16_t) ~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
		return take_exception(cpu, 5, 38 + cpu->extra);
	}
	cpu->sr &= (uint16_t) ~(FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (is_signed) {
		int32_t dividend = (int32_t) cpu->d[reg];
		int32_t signed_divisor = (int16_t) divisor;
		int32_t quotient;
		int32_t remainder;
		cycles = divide_signed_time(dividend, (int16_t) divisor);
		if (dividend == INT32_MIN && signed_divisor == -1) {
			cpu->sr |= FLAG_V | FLAG_N;
			return cycles + cpu->extra;
		}
		quotient = dividend / signed_divisor;
		remainder = dividend % signed_divisor;
		if (quotient < INT16_MIN || quotient > INT16_MAX) {
			cpu->sr |= FLAG_V | FLAG_N;
			return cycles + cpu->extra;
		}
		cpu->d[reg] = (uint32_t) ((uint16_t) remainder) << 16 | (uint16_t) quotient;
		if (!quotient) cpu->sr |= FLAG_Z;
		if (quotient < 0) cpu->sr |= FLAG_N;
	} else {
		uint32_t quotient = cpu->d[reg] / divisor;
		uint32_t remainder = cpu->d[reg] % divisor;
		if (quotient > UINT16_MAX) {
			cpu->sr |= FLAG_V | FLAG_N;
			return 10 + cpu->extra;
		}
		cycles = divide_unsigned_time(cpu->d[reg], divisor);
		cpu->d[reg] = remainder << 16 | quotient;
		if (!quotient) cpu->sr |= FLAG_Z;
		if (quotient & 0x8000) cpu->sr |= FLAG_N;
	}
	return cycles + cpu->extra;
}

static unsigned execute_multiply(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned reg = (opcode >> 9) & 7;
	struct effective_address ea;
	uint32_t source;
	uint32_t pattern;
	unsigned cycles = 38;

	cpu->extra = 0;
	if (!resolve_ea(cpu, (opcode >> 3) & 7, opcode & 7, 2, VALID_DATA, &ea)) return illegal(cpu);
	source = ea_read(cpu, &ea, 2);
	if (cpu->address_error) return 0;
	if (opcode & 0x0100) {
		cpu->d[reg] = (uint32_t) ((int32_t) (int16_t) cpu->d[reg] * (int32_t) (int16_t) source);
		pattern = (source ^ (source << 1)) & 0xffff;
	} else {
		cpu->d[reg] = (cpu->d[reg] & 0xffff) * source;
		pattern = source;
	}
	for (; pattern; pattern &= pattern - 1) cycles += 2;
	set_logic_flags(cpu, cpu->d[reg], 4);
	return cycles + cpu->extra;
}

static unsigned execute_logic(struct karashi_m68k *cpu, uint16_t opcode) {
	bool and_operation = (opcode >> 12) == 12;
	unsigned reg = (opcode >> 9) & 7;
	unsigned opmode = (opcode >> 6) & 7;
	unsigned mode = (opcode >> 3) & 7;
	unsigned other = opcode & 7;
	unsigned size = 1u << (opmode & 3);
	struct effective_address ea;
	uint32_t value;

	if ((opmode & 3) == 3) return and_operation ? execute_multiply(cpu, opcode) : execute_divide(cpu, opcode);
	cpu->extra = 0;
	if (opmode == 4 && mode < 2) {
		if (mode == 0) {
			value = and_operation ? bcd_add(cpu, cpu->d[reg], cpu->d[other])
			                      : bcd_subtract(cpu, cpu->d[reg], cpu->d[other]);
			cpu->d[reg] = (cpu->d[reg] & 0xffffff00) | value;
			return 6;
		}
		cpu->a[other] -= increment_for(other, 1);
		value = read8(cpu, cpu->a[other]);
		cpu->a[reg] -= increment_for(reg, 1);
		value = and_operation ? bcd_add(cpu, read8(cpu, cpu->a[reg]), value)
		                      : bcd_subtract(cpu, read8(cpu, cpu->a[reg]), value);
		write8(cpu, cpu->a[reg], (uint8_t) value);
		return 18;
	}
	if (and_operation && (opmode == 5 || opmode == 6) && mode < 2) {
		if (opmode == 5 && mode == 0) {
			value = cpu->d[reg];
			cpu->d[reg] = cpu->d[other];
			cpu->d[other] = value;
		} else if (opmode == 5) {
			value = cpu->a[reg];
			cpu->a[reg] = cpu->a[other];
			cpu->a[other] = value;
		} else if (mode == 1) {
			value = cpu->d[reg];
			cpu->d[reg] = cpu->a[other];
			cpu->a[other] = value;
		} else return illegal(cpu);
		return 6;
	}
	if (opmode < 3) {
		if (!resolve_ea(cpu, mode, other, size, VALID_DATA, &ea)) return illegal(cpu);
		value = ea_read(cpu, &ea, size);
		if (cpu->address_error) return 0;
		value = and_operation ? cpu->d[reg] & (value | ~size_mask(size)) : cpu->d[reg] | value;
		cpu->d[reg] = value;
		set_logic_flags(cpu, value, size);
		return source_time(mode, other, size, cpu->extra);
	}
	if (!resolve_ea(cpu, mode, other, size, VALID_MEMORY_ALTERABLE, &ea)) return illegal(cpu);
	value = ea_read(cpu, &ea, size);
	if (cpu->address_error) return 0;
	value = and_operation ? value & cpu->d[reg] : value | cpu->d[reg];
	set_logic_flags(cpu, value, size);
	ea_write(cpu, &ea, size, value);
	return (size == 4 ? 12 : 8) + cpu->extra;
}

static uint32_t shift_value(
	struct karashi_m68k *cpu, unsigned type, bool left, unsigned size, uint32_t value, unsigned count
) {
	uint32_t mask = size_mask(size);
	uint32_t sign = size_sign(size);
	bool extend = (cpu->sr & FLAG_X) != 0;
	bool carry = false;
	bool overflow = false;
	unsigned index;

	value &= mask;
	for (index = 0; index < count; index++) {
		uint32_t top = value & sign;
		bool out = left ? top != 0 : (value & 1) != 0;
		if (left) {
			value = value << 1 & mask;
			if (type == 2) value |= extend ? 1 : 0;
			else if (type == 3) value |= out ? 1 : 0;
			if (type == 0 && (value & sign) != top) overflow = true;
		} else {
			value >>= 1;
			if (type == 0) value |= top;
			else if (type == 2) value |= extend ? sign : 0;
			else if (type == 3) value |= out ? sign : 0;
		}
		carry = out;
		if (type != 3) extend = out;
	}
	if (!count && type == 2) carry = extend;
	cpu->sr &= (uint16_t) ~(FLAG_X | FLAG_N | FLAG_Z | FLAG_V | FLAG_C);
	if (extend) cpu->sr |= FLAG_X;
	if (carry) cpu->sr |= FLAG_C;
	if (overflow) cpu->sr |= FLAG_V;
	if (!value) cpu->sr |= FLAG_Z;
	if (value & sign) cpu->sr |= FLAG_N;
	return value;
}

static unsigned execute_shift(struct karashi_m68k *cpu, uint16_t opcode) {
	unsigned size_code = (opcode >> 6) & 3;
	unsigned size = 1u << size_code;
	bool left = (opcode & 0x0100) != 0;
	unsigned reg = opcode & 7;
	struct effective_address ea;
	unsigned count;
	uint32_t value;

	if (size_code == 3) {
		cpu->extra = 0;
		if ((opcode & 0x0800) || !resolve_ea(cpu, (opcode >> 3) & 7, reg, 2, VALID_MEMORY_ALTERABLE, &ea)) {
			return illegal(cpu);
		}
		value = ea_read(cpu, &ea, 2);
		if (cpu->address_error) return 0;
		value = shift_value(cpu, (opcode >> 9) & 3, left, 2, value, 1);
		ea_write(cpu, &ea, 2, value);
		return 8 + cpu->extra;
	}
	count = (opcode >> 9) & 7;
	if (opcode & 0x0020) count = cpu->d[count] & 63;
	else if (!count) count = 8;
	value = shift_value(cpu, (opcode >> 3) & 3, left, size, cpu->d[reg], count);
	cpu->d[reg] = (cpu->d[reg] & ~size_mask(size)) | value;
	return (size == 4 ? 8 : 6) + count * 2;
}

void karashi_m68k_init(struct karashi_m68k *cpu, const struct karashi_m68k_bus *bus) {
	memset(cpu, 0, sizeof(*cpu));
	if (bus) cpu->bus = *bus;
	karashi_m68k_reset(cpu);
}

void karashi_m68k_reset(struct karashi_m68k *cpu) {
	struct karashi_m68k_bus bus = cpu->bus;

	memset(cpu, 0, sizeof(*cpu));
	cpu->bus = bus;
	cpu->sr = SR_SUPERVISOR | 0x0700;
	cpu->a[7] = read32(cpu, 0);
	cpu->ssp = cpu->a[7];
	cpu->pc = read32(cpu, 4) & ADDRESS_MASK;
	if (cpu->address_error || (cpu->pc & 1)) cpu->fault = KARASHI_M68K_FAULT_ADDRESS;
	cpu->address_error = false;
	cpu->prefetch_count = 0;
}

static bool interrupt_pending(const struct karashi_m68k *cpu) {
	return cpu->irq_level && (cpu->irq_level > ((cpu->sr >> 8) & 7) || cpu->irq_level == 7);
}

unsigned karashi_m68k_step(struct karashi_m68k *cpu) {
	uint16_t opcode;
	unsigned cycles;
	bool trace;

	if (cpu->fault != KARASHI_M68K_FAULT_NONE) return 0;
	if (interrupt_pending(cpu)) {
		cycles = take_interrupt(cpu, cpu->irq_level);
		if (cpu->address_error) cycles += take_address_error(cpu);
		fill_prefetch(cpu);
		cpu->cycles += cycles;
		return cycles;
	}
	if (cpu->trace_pending) {
		cpu->trace_pending = false;
		cycles = take_exception(cpu, 9, 34);
		fill_prefetch(cpu);
		cpu->cycles += cycles;
		return cycles;
	}
	if (cpu->stopped) {
		cpu->cycles += 4;
		return 4;
	}
	trace = (cpu->sr & SR_TRACE) != 0;
	cpu->instruction_pc = cpu->pc;
	opcode = fetch16(cpu);
	cpu->opcode = opcode;
	switch (opcode >> 12) {
		case 0: cycles = execute_group0(cpu, opcode); break;
		case 1:
		case 2:
		case 3: cycles = execute_move(cpu, opcode); break;
		case 4: cycles = execute_group4(cpu, opcode); break;
		case 5: cycles = execute_group5(cpu, opcode); break;
		case 6: cycles = execute_branch(cpu, opcode); break;
		case 7:
			if (opcode & 0x0100) cycles = illegal(cpu);
			else {
				unsigned reg = (opcode >> 9) & 7;
				cpu->d[reg] = (uint32_t) (int32_t) (int8_t) opcode;
				set_logic_flags(cpu, cpu->d[reg], 4);
				cycles = 4;
			}
			break;
		case 8:
		case 12: cycles = execute_logic(cpu, opcode); break;
		case 9:
		case 11:
		case 13: cycles = execute_arithmetic(cpu, opcode); break;
		case 14: cycles = execute_shift(cpu, opcode); break;
		default: cycles = illegal(cpu); break;
	}
	if (cpu->pc & 1) address_fault(cpu, cpu->pc, false, true);
	if (cpu->address_error) cycles = take_address_error(cpu);
	else cpu->trace_pending = trace;
	if (!cpu->stopped && (cpu->prefetch_count < 2 || cpu->prefetch_address != cpu->pc)) fill_prefetch(cpu);
	cpu->cycles += cycles;
	return cycles;
}

static void remember_loop(struct karashi_m68k *cpu, bool self) {
	cpu->loop_valid = true;
	cpu->loop_self = self;
	cpu->loop_pc = cpu->pc;
	cpu->loop_cycles = cpu->cycles;
	cpu->loop_events = cpu->bus_events;
	cpu->loop_sr = cpu->sr;
	memcpy(cpu->loop_regs, cpu->d, sizeof(cpu->d));
	memcpy(cpu->loop_regs + 8, cpu->a, sizeof(cpu->a));
}

static bool loop_repeats(const struct karashi_m68k *cpu) {
	return cpu->loop_valid && !cpu->loop_self && cpu->loop_pc == cpu->pc && cpu->loop_events == cpu->bus_events && cpu->loop_sr == cpu->sr
	       && !(cpu->sr & SR_TRACE) && !memcmp(cpu->loop_regs, cpu->d, sizeof(cpu->d))
	       && !memcmp(cpu->loop_regs + 8, cpu->a, sizeof(cpu->a));
}

static void skip_loop(struct karashi_m68k *cpu, uint32_t start, uint64_t limit) {
	uint16_t opcode = cpu->opcode;
	bool branch = (opcode & 0xf000) == 0x6000 || (opcode & 0xf0f8) == 0x50c8;
	uint64_t cost;
	uint64_t count;

	if (!branch || cpu->instruction_pc != start || cpu->pc >= start || start - cpu->pc > 256) {
		if ((opcode & 0xfff8) == 0x51c8 && cpu->pc == start && cpu->instruction_pc == start && !(cpu->sr & SR_TRACE)) {
			unsigned reg = opcode & 7;
			if (cpu->loop_valid && cpu->loop_self && cpu->loop_pc == start && cpu->loop_events == cpu->bus_events && cpu->cycles < limit) {
				cost = cpu->cycles - cpu->loop_cycles;
				count = cost ? (limit - 1 - cpu->cycles) / cost : 0;
				if (count > (cpu->d[reg] & 0xffff)) count = cpu->d[reg] & 0xffff;
				cpu->d[reg] = (cpu->d[reg] & 0xffff0000u) | ((cpu->d[reg] - (uint32_t) count) & 0xffff);
				cpu->cycles += count * cost;
			}
			remember_loop(cpu, true);
		}
		return;
	}
	if (loop_repeats(cpu) && cpu->cycles < limit) {
		cost = cpu->cycles - cpu->loop_cycles;
		count = cost ? (limit - 1 - cpu->cycles) / cost : 0;
		cpu->cycles += count * cost;
	}
	remember_loop(cpu, false);
}

void karashi_m68k_run(struct karashi_m68k *cpu, uint64_t limit) {
	cpu->loop_valid = false;
	if (cpu->stopped && !cpu->trace_pending && cpu->fault == KARASHI_M68K_FAULT_NONE
	    && !interrupt_pending(cpu) && cpu->cycles < limit) {
		/* STOP idles in four-cycle steps until the scheduler can change the IRQ level. */
		uint64_t remaining = limit - cpu->cycles;
		cpu->cycles += remaining + ((-remaining) & 3);
		return;
	}
	while (cpu->cycles < limit) {
		uint32_t start = cpu->pc;
		if (!karashi_m68k_step(cpu)) {
			cpu->cycles = limit;
			return;
		}
		skip_loop(cpu, start, limit);
	}
}

void karashi_m68k_irq(struct karashi_m68k *cpu, unsigned level) {
	if (level <= 7 && level > cpu->irq_level) cpu->irq_level = (uint8_t) level;
}

void karashi_m68k_set_irq(struct karashi_m68k *cpu, unsigned level) {
	cpu->irq_level = (uint8_t) (level & 7);
}

bool karashi_m68k_save(const struct karashi_m68k *cpu, struct karashi_state_writer *writer) {
	unsigned index;

	for (index = 0; index < 8; index++) karashi_state_write_u32(writer, cpu->d[index]);
	for (index = 0; index < 8; index++) karashi_state_write_u32(writer, cpu->a[index]);
	karashi_state_write_u32(writer, cpu->pc);
	karashi_state_write_u32(writer, cpu->instruction_pc);
	karashi_state_write_u32(writer, cpu->prefetch_address);
	karashi_state_write_u16(writer, cpu->prefetch[0]);
	karashi_state_write_u16(writer, cpu->prefetch[1]);
	karashi_state_write_u8(writer, cpu->prefetch_count);
	karashi_state_write_u32(writer, cpu->usp);
	karashi_state_write_u32(writer, cpu->ssp);
	karashi_state_write_u64(writer, cpu->cycles);
	karashi_state_write_u16(writer, cpu->sr);
	karashi_state_write_u8(writer, cpu->irq_level);
	karashi_state_write_u8(writer, (uint8_t) cpu->fault);
	return karashi_state_write_bool(writer, cpu->stopped);
}

bool karashi_m68k_load(struct karashi_m68k *cpu, struct karashi_state_reader *reader) {
	unsigned index;
	uint8_t fault;
	bool loaded = true;

	cpu->loop_valid = false;
	for (index = 0; index < 8; index++) loaded = karashi_state_read_u32(reader, &cpu->d[index]) && loaded;
	for (index = 0; index < 8; index++) loaded = karashi_state_read_u32(reader, &cpu->a[index]) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->pc) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->instruction_pc) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->prefetch_address) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->prefetch[0]) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->prefetch[1]) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->prefetch_count) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->usp) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->ssp) && loaded;
	loaded = karashi_state_read_u64(reader, &cpu->cycles) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->sr) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->irq_level) && loaded;
	loaded = karashi_state_read_u8(reader, &fault) && loaded;
	loaded = karashi_state_read_bool(reader, &cpu->stopped) && loaded;
	if (!loaded || fault > KARASHI_M68K_FAULT_ILLEGAL || cpu->irq_level > 7 || cpu->prefetch_count > 2) return false;
	cpu->pc &= ADDRESS_MASK;
	cpu->prefetch_address &= ADDRESS_MASK;
	cpu->sr &= SR_IMPLEMENTED;
	cpu->fault = (enum karashi_m68k_fault) fault;
	cpu->address_error = false;
	return true;
}
