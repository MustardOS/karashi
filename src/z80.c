#include "z80.h"

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

static uint8_t high(uint16_t value) {
	return (uint8_t) (value >> 8);
}

static uint8_t low(uint16_t value) {
	return (uint8_t) value;
}

static void set_high(uint16_t *value, uint8_t byte) {
	*value = (uint16_t) ((uint16_t) byte << 8 | low(*value));
}

static void set_low(uint16_t *value, uint8_t byte) {
	*value = (uint16_t) ((uint16_t) high(*value) << 8 | byte);
}

static uint8_t memory_read(struct karashi_z80 *cpu, uint16_t address) {
	return cpu->bus.read ? cpu->bus.read(cpu->bus.context, address) : 0xff;
}

static void memory_write(struct karashi_z80 *cpu, uint16_t address, uint8_t value) {
	if (cpu->bus.write) cpu->bus.write(cpu->bus.context, address, value);
}

static uint8_t port_read(struct karashi_z80 *cpu, uint16_t port) {
	return cpu->bus.io_read ? cpu->bus.io_read(cpu->bus.context, port) : 0xff;
}

static void port_write(struct karashi_z80 *cpu, uint16_t port, uint8_t value) {
	if (cpu->bus.io_write) cpu->bus.io_write(cpu->bus.context, port, value);
}

static void increment_r(struct karashi_z80 *cpu) {
	cpu->r = (uint8_t) ((cpu->r & 0x80) | ((cpu->r + 1) & 0x7f));
}

static uint8_t fetch_opcode(struct karashi_z80 *cpu) {
	if (cpu->prefetched_valid) {
		cpu->prefetched_valid = false;
		return cpu->prefetched_opcode;
	}
	uint8_t value = memory_read(cpu, cpu->pc++);
	increment_r(cpu);
	return value;
}

static uint8_t fetch8(struct karashi_z80 *cpu) {
	return memory_read(cpu, cpu->pc++);
}

static uint16_t fetch16(struct karashi_z80 *cpu) {
	uint16_t value = fetch8(cpu);
	return (uint16_t) (value | (uint16_t) fetch8(cpu) << 8);
}

static void push16(struct karashi_z80 *cpu, uint16_t value) {
	memory_write(cpu, --cpu->sp, high(value));
	memory_write(cpu, --cpu->sp, low(value));
}

static uint16_t pop16(struct karashi_z80 *cpu) {
	uint16_t value = memory_read(cpu, cpu->sp++);
	return (uint16_t) (value | (uint16_t) memory_read(cpu, cpu->sp++) << 8);
}

static bool parity(uint8_t value) {
	value ^= value >> 4;
	value &= 0x0f;
	return ((0x6996 >> value) & 1) == 0;
}

static uint8_t szxy(uint8_t value) {
	uint8_t flags = value & (FLAG_S | FLAG_Y | FLAG_X);
	if (value == 0) flags |= FLAG_Z;
	return flags;
}

static uint8_t szpxy(uint8_t value) {
	uint8_t flags = szxy(value);
	if (parity(value)) flags |= FLAG_PV;
	return flags;
}

static uint16_t *pair(struct karashi_z80 *cpu, unsigned index) {
	switch (index & 3) {
		case 0: return &cpu->bc;
		case 1: return &cpu->de;
		case 2: return &cpu->hl;
		default: return &cpu->sp;
	}
}

static uint16_t *pair2(struct karashi_z80 *cpu, unsigned index) {
	return (index & 3) == 3 ? &cpu->af : pair(cpu, index);
}

static uint8_t read_register(struct karashi_z80 *cpu, unsigned index) {
	switch (index & 7) {
		case 0: return high(cpu->bc);
		case 1: return low(cpu->bc);
		case 2: return high(cpu->de);
		case 3: return low(cpu->de);
		case 4: return high(cpu->hl);
		case 5: return low(cpu->hl);
		case 6: return memory_read(cpu, cpu->hl);
		default: return high(cpu->af);
	}
}

static void write_register(struct karashi_z80 *cpu, unsigned index, uint8_t value) {
	switch (index & 7) {
		case 0: set_high(&cpu->bc, value); break;
		case 1: set_low(&cpu->bc, value); break;
		case 2: set_high(&cpu->de, value); break;
		case 3: set_low(&cpu->de, value); break;
		case 4: set_high(&cpu->hl, value); break;
		case 5: set_low(&cpu->hl, value); break;
		case 6: memory_write(cpu, cpu->hl, value); break;
		default: set_high(&cpu->af, value); break;
	}
}

static bool condition(const struct karashi_z80 *cpu, unsigned index) {
	uint8_t flags = low(cpu->af);
	switch (index & 7) {
		case 0: return !(flags & FLAG_Z);
		case 1: return (flags & FLAG_Z) != 0;
		case 2: return !(flags & FLAG_C);
		case 3: return (flags & FLAG_C) != 0;
		case 4: return !(flags & FLAG_PV);
		case 5: return (flags & FLAG_PV) != 0;
		case 6: return !(flags & FLAG_S);
		default: return (flags & FLAG_S) != 0;
	}
}

static uint8_t increment8(struct karashi_z80 *cpu, uint8_t value) {
	uint8_t result = (uint8_t) (value + 1);
	uint8_t flags = low(cpu->af) & FLAG_C;
	flags |= szxy(result);
	if ((value & 0x0f) == 0x0f) flags |= FLAG_H;
	if (value == 0x7f) flags |= FLAG_PV;
	set_low(&cpu->af, flags);
	return result;
}

static uint8_t decrement8(struct karashi_z80 *cpu, uint8_t value) {
	uint8_t result = (uint8_t) (value - 1);
	uint8_t flags = (low(cpu->af) & FLAG_C) | FLAG_N;
	flags |= szxy(result);
	if ((value & 0x0f) == 0) flags |= FLAG_H;
	if (value == 0x80) flags |= FLAG_PV;
	set_low(&cpu->af, flags);
	return result;
}

static void add8(struct karashi_z80 *cpu, uint8_t value, bool carry) {
	uint8_t a = high(cpu->af);
	unsigned carry_in = carry && (low(cpu->af) & FLAG_C);
	unsigned wide = a + value + carry_in;
	uint8_t result = (uint8_t) wide;
	uint8_t flags = szxy(result);
	if (((a ^ value ^ result) & 0x10) != 0) flags |= FLAG_H;
	if (((~(a ^ value) & (a ^ result)) & 0x80) != 0) flags |= FLAG_PV;
	if (wide > 0xff) flags |= FLAG_C;
	cpu->af = (uint16_t) ((uint16_t) result << 8 | flags);
}

static void subtract8(struct karashi_z80 *cpu, uint8_t value, bool carry, bool compare) {
	uint8_t a = high(cpu->af);
	unsigned carry_in = carry && (low(cpu->af) & FLAG_C);
	unsigned subtrahend = value + carry_in;
	uint8_t result = (uint8_t) (a - subtrahend);
	uint8_t flags = (compare ? value : result) & (FLAG_Y | FLAG_X);
	flags |= FLAG_N;
	if (result & FLAG_S) flags |= FLAG_S;
	if (result == 0) flags |= FLAG_Z;
	if (((a ^ value ^ result) & 0x10) != 0) flags |= FLAG_H;
	if ((((a ^ value) & (a ^ result)) & 0x80) != 0) flags |= FLAG_PV;
	if (a < subtrahend) flags |= FLAG_C;
	set_low(&cpu->af, flags);
	if (!compare) set_high(&cpu->af, result);
}

static void logic8(struct karashi_z80 *cpu, uint8_t value, unsigned operation) {
	uint8_t result;
	uint8_t flags;
	uint8_t a = high(cpu->af);

	if (operation == 4) {
		result = (uint8_t) (a & value);
		flags = FLAG_H;
	} else if (operation == 5) {
		result = (uint8_t) (a ^ value);
		flags = 0;
	} else {
		result = (uint8_t) (a | value);
		flags = 0;
	}
	flags |= szpxy(result);
	cpu->af = (uint16_t) ((uint16_t) result << 8 | flags);
}

static void arithmetic(struct karashi_z80 *cpu, unsigned operation, uint8_t value) {
	switch (operation & 7) {
		case 0: add8(cpu, value, false); break;
		case 1: add8(cpu, value, true); break;
		case 2: subtract8(cpu, value, false, false); break;
		case 3: subtract8(cpu, value, true, false); break;
		case 4:
		case 5:
		case 6: logic8(cpu, value, operation); break;
		default: subtract8(cpu, value, false, true); break;
	}
}

static void add16(struct karashi_z80 *cpu, uint16_t value) {
	uint16_t left = cpu->hl;
	uint32_t wide = (uint32_t) left + value;
	uint16_t result = (uint16_t) wide;
	uint8_t flags = low(cpu->af) & (FLAG_S | FLAG_Z | FLAG_PV);
	flags |= high(result) & (FLAG_Y | FLAG_X);
	if (((left ^ value ^ result) & 0x1000) != 0) flags |= FLAG_H;
	if (wide > 0xffff) flags |= FLAG_C;
	cpu->hl = result;
	set_low(&cpu->af, flags);
}

static void add_index16(struct karashi_z80 *cpu, uint16_t *index, uint16_t value) {
	uint16_t left = *index;
	uint32_t wide = (uint32_t) left + value;
	uint16_t result = (uint16_t) wide;
	uint8_t flags = low(cpu->af) & (FLAG_S | FLAG_Z | FLAG_PV);

	flags |= high(result) & (FLAG_Y | FLAG_X);
	if (((left ^ value ^ result) & 0x1000) != 0) flags |= FLAG_H;
	if (wide > 0xffff) flags |= FLAG_C;
	*index = result;
	set_low(&cpu->af, flags);
}

static uint8_t rotate_shift(struct karashi_z80 *cpu, unsigned operation, uint8_t value) {
	uint8_t carry_in = (low(cpu->af) & FLAG_C) != 0;
	uint8_t carry_out;
	uint8_t result;

	switch (operation & 7) {
		case 0:
			carry_out = value >> 7;
			result = (uint8_t) ((value << 1) | carry_out);
			break;
		case 1:
			carry_out = value & 1;
			result = (uint8_t) ((value >> 1) | (carry_out << 7));
			break;
		case 2:
			carry_out = value >> 7;
			result = (uint8_t) ((value << 1) | carry_in);
			break;
		case 3:
			carry_out = value & 1;
			result = (uint8_t) ((value >> 1) | (carry_in << 7));
			break;
		case 4:
			carry_out = value >> 7;
			result = (uint8_t) (value << 1);
			break;
		case 5:
			carry_out = value & 1;
			result = (uint8_t) ((value >> 1) | (value & 0x80));
			break;
		case 6:
			carry_out = value >> 7;
			result = (uint8_t) ((value << 1) | 1);
			break;
		default:
			carry_out = value & 1;
			result = value >> 1;
			break;
	}
	set_low(&cpu->af, (uint8_t) (szpxy(result) | carry_out));
	return result;
}

static unsigned execute_cb(struct karashi_z80 *cpu) {
	uint8_t opcode = fetch_opcode(cpu);
	unsigned group = opcode >> 6;
	unsigned operation = (opcode >> 3) & 7;
	unsigned target = opcode & 7;
	uint8_t value = read_register(cpu, target);

	if (group == 0) {
		write_register(cpu, target, rotate_shift(cpu, operation, value));
	} else if (group == 1) {
		uint8_t mask = (uint8_t) (1u << operation);
		uint8_t flags = (low(cpu->af) & FLAG_C) | FLAG_H;
		if (!(value & mask)) flags |= FLAG_Z | FLAG_PV;
		if (operation == 7 && (value & mask)) flags |= FLAG_S;
		flags |= value & (FLAG_Y | FLAG_X);
		set_low(&cpu->af, flags);
	} else if (group == 2) {
		write_register(cpu, target, (uint8_t) (value & ~(1u << operation)));
	} else {
		write_register(cpu, target, (uint8_t) (value | (1u << operation)));
	}

	if (target != 6) return 8;
	return group == 1 ? 12 : 15;
}

static void arithmetic16(struct karashi_z80 *cpu, uint16_t value, bool subtract) {
	uint16_t left = cpu->hl;
	unsigned carry = (low(cpu->af) & FLAG_C) != 0;
	uint32_t wide;
	uint16_t result;
	uint8_t flags;

	if (subtract) wide = (uint32_t) left - value - carry;
	else wide = (uint32_t) left + value + carry;
	result = (uint16_t) wide;
	flags = high(result) & (FLAG_S | FLAG_Y | FLAG_X);
	if (result == 0) flags |= FLAG_Z;
	if ((left ^ value ^ result) & 0x1000) flags |= FLAG_H;
	if (subtract) {
		flags |= FLAG_N;
		if (((left ^ value) & (left ^ result)) & 0x8000) flags |= FLAG_PV;
		if ((uint32_t) left < (uint32_t) value + carry) flags |= FLAG_C;
	} else {
		if ((~(left ^ value) & (left ^ result)) & 0x8000) flags |= FLAG_PV;
		if (wide > 0xffff) flags |= FLAG_C;
	}
	cpu->hl = result;
	set_low(&cpu->af, flags);
}

static void block_transfer(struct karashi_z80 *cpu, bool decrement) {
	uint8_t value = memory_read(cpu, cpu->hl);
	uint8_t sum = (uint8_t) (high(cpu->af) + value);
	uint8_t flags = low(cpu->af) & (FLAG_S | FLAG_Z | FLAG_C);

	memory_write(cpu, cpu->de, value);
	cpu->hl = (uint16_t) (cpu->hl + (decrement ? -1 : 1));
	cpu->de = (uint16_t) (cpu->de + (decrement ? -1 : 1));
	cpu->bc--;
	if (cpu->bc) flags |= FLAG_PV;
	flags |= sum & FLAG_X;
	flags |= (sum & 0x02) << 4;
	set_low(&cpu->af, flags);
}

static void block_compare(struct karashi_z80 *cpu, bool decrement) {
	uint8_t value = memory_read(cpu, cpu->hl);
	uint8_t a = high(cpu->af);
	uint8_t result = (uint8_t) (a - value);
	uint8_t flags = (low(cpu->af) & FLAG_C) | FLAG_N;
	uint8_t adjusted;

	cpu->hl = (uint16_t) (cpu->hl + (decrement ? -1 : 1));
	cpu->bc--;
	if (result & FLAG_S) flags |= FLAG_S;
	if (result == 0) flags |= FLAG_Z;
	if ((a ^ value ^ result) & 0x10) flags |= FLAG_H;
	if (cpu->bc) flags |= FLAG_PV;
	adjusted = (uint8_t) (result - ((flags & FLAG_H) != 0));
	flags |= adjusted & FLAG_X;
	flags |= (adjusted & 0x02) << 4;
	set_low(&cpu->af, flags);
}

static void block_input(struct karashi_z80 *cpu, bool decrement) {
	uint8_t value = port_read(cpu, cpu->bc);
	uint8_t b = (uint8_t) (high(cpu->bc) - 1);
	uint8_t c = low(cpu->bc);
	unsigned sum = value + (uint8_t) (c + (decrement ? -1 : 1));
	uint8_t flags = szxy(b);

	memory_write(cpu, cpu->hl, value);
	cpu->hl = (uint16_t) (cpu->hl + (decrement ? -1 : 1));
	set_high(&cpu->bc, b);
	if (value & 0x80) flags |= FLAG_N;
	if (sum > 0xff) flags |= FLAG_H | FLAG_C;
	if (parity((uint8_t) ((sum & 7) ^ b))) flags |= FLAG_PV;
	set_low(&cpu->af, flags);
}

static void block_output(struct karashi_z80 *cpu, bool decrement) {
	uint8_t value = memory_read(cpu, cpu->hl);
	uint8_t b = (uint8_t) (high(cpu->bc) - 1);
	unsigned sum;
	uint8_t flags = szxy(b);

	cpu->hl = (uint16_t) (cpu->hl + (decrement ? -1 : 1));
	set_high(&cpu->bc, b);
	port_write(cpu, cpu->bc, value);
	sum = value + low(cpu->hl);
	if (value & 0x80) flags |= FLAG_N;
	if (sum > 0xff) flags |= FLAG_H | FLAG_C;
	if (parity((uint8_t) ((sum & 7) ^ b))) flags |= FLAG_PV;
	set_low(&cpu->af, flags);
}

static unsigned execute_ed(struct karashi_z80 *cpu) {
	static const uint8_t interrupt_modes[8] = {0, 0, 1, 2, 0, 0, 1, 2};
	uint8_t opcode = fetch_opcode(cpu);
	unsigned x = opcode >> 6;
	unsigned y = (opcode >> 3) & 7;
	unsigned z = opcode & 7;
	unsigned p = y >> 1;
	unsigned q = y & 1;
	uint16_t address;
	uint16_t value16;
	uint8_t value;
	uint8_t flags;
	bool decrement;
	bool repeat;

	if (x == 1) {
		switch (z) {
			case 0:
				value = port_read(cpu, cpu->bc);
				if (y != 6) write_register(cpu, y, value);
				set_low(&cpu->af, (uint8_t) ((low(cpu->af) & FLAG_C) | szpxy(value)));
				return 12;
			case 1:
				port_write(cpu, cpu->bc, y == 6 ? 0 : read_register(cpu, y));
				return 12;
			case 2:
				arithmetic16(cpu, *pair(cpu, p), !q);
				return 15;
			case 3:
				address = fetch16(cpu);
				if (!q) {
					value16 = *pair(cpu, p);
					memory_write(cpu, address, low(value16));
					memory_write(cpu, address + 1, high(value16));
				} else {
					*pair(cpu, p) = (uint16_t) (memory_read(cpu, address) |
						(uint16_t) memory_read(cpu, address + 1) << 8);
				}
				return 20;
			case 4:
				value = high(cpu->af);
				set_high(&cpu->af, 0);
				subtract8(cpu, value, false, false);
				return 8;
			case 5:
				cpu->pc = pop16(cpu);
				cpu->iff1 = cpu->iff2;
				return 14;
			case 6:
				cpu->interrupt_mode = interrupt_modes[y];
				return 8;
			default:
				if (y == 0) cpu->i = high(cpu->af);
				else if (y == 1) cpu->r = high(cpu->af);
				else if (y == 2 || y == 3) {
					value = y == 2 ? cpu->i : cpu->r;
					set_high(&cpu->af, value);
					flags = (uint8_t) ((low(cpu->af) & FLAG_C) | szxy(value));
					if (cpu->iff2) flags |= FLAG_PV;
					set_low(&cpu->af, flags);
				} else if (y == 4 || y == 5) {
					value = memory_read(cpu, cpu->hl);
					if (y == 4) {
						memory_write(cpu, cpu->hl, (uint8_t) ((high(cpu->af) << 4) | (value >> 4)));
						set_high(&cpu->af, (uint8_t) ((high(cpu->af) & 0xf0) | (value & 0x0f)));
					} else {
						memory_write(cpu, cpu->hl, (uint8_t) ((value << 4) | (high(cpu->af) & 0x0f)));
						set_high(&cpu->af, (uint8_t) ((high(cpu->af) & 0xf0) | (value >> 4)));
					}
					set_low(&cpu->af, (uint8_t) ((low(cpu->af) & FLAG_C) | szpxy(high(cpu->af))));
					return 18;
				}
				return y < 4 ? 9 : 8;
		}
	}

	if (x == 2 && y >= 4 && z <= 3) {
		decrement = (y & 1) != 0;
		repeat = y >= 6;
		if (z == 0) block_transfer(cpu, decrement);
		else if (z == 1) block_compare(cpu, decrement);
		else if (z == 2) block_input(cpu, decrement);
		else block_output(cpu, decrement);
		if (repeat && ((z == 0) ? cpu->bc :
			(z == 1) ? (cpu->bc && !(low(cpu->af) & FLAG_Z)) : high(cpu->bc))) {
			cpu->pc -= 2;
			return 21;
		}
		return 16;
	}

	return 8;
}

static uint8_t read_index_register(struct karashi_z80 *cpu, uint16_t index, unsigned target) {
	if ((target & 7) == 4) return high(index);
	if ((target & 7) == 5) return low(index);
	return read_register(cpu, target);
}

static void write_index_register(struct karashi_z80 *cpu, uint16_t *index, unsigned target, uint8_t value) {
	if ((target & 7) == 4) set_high(index, value);
	else if ((target & 7) == 5) set_low(index, value);
	else write_register(cpu, target, value);
}

static unsigned execute_index_cb(struct karashi_z80 *cpu, uint16_t address) {
	uint8_t opcode = fetch8(cpu);
	unsigned group = opcode >> 6;
	unsigned operation = (opcode >> 3) & 7;
	unsigned target = opcode & 7;
	uint8_t value = memory_read(cpu, address);
	uint8_t result;

	if (group == 0) {
		result = rotate_shift(cpu, operation, value);
		memory_write(cpu, address, result);
		if (target != 6) write_register(cpu, target, result);
	} else if (group == 1) {
		uint8_t mask = (uint8_t) (1u << operation);
		uint8_t flags = (low(cpu->af) & FLAG_C) | FLAG_H;

		if (!(value & mask)) flags |= FLAG_Z | FLAG_PV;
		if (operation == 7 && (value & mask)) flags |= FLAG_S;
		flags |= high(address) & (FLAG_Y | FLAG_X);
		set_low(&cpu->af, flags);
		return 20;
	} else {
		result = group == 2 ? (uint8_t) (value & ~(1u << operation)) :
			(uint8_t) (value | (1u << operation));
		memory_write(cpu, address, result);
		if (target != 6) write_register(cpu, target, result);
	}
	return 23;
}

static unsigned execute_prefetched(struct karashi_z80 *cpu, uint8_t opcode) {
	uint64_t cycles_before = cpu->cycles;
	unsigned cycles;

	cpu->prefetched_opcode = opcode;
	cpu->prefetched_valid = true;
	cpu->prefix_active = true;
	cycles = karashi_z80_step(cpu);
	cpu->prefix_active = false;
	cpu->cycles = cycles_before;
	if (cpu->ei_delay) cpu->ei_delay++;
	return cycles + 4;
}

static unsigned execute_index(struct karashi_z80 *cpu, uint16_t *index) {
	uint8_t opcode = fetch_opcode(cpu);
	unsigned x = opcode >> 6;
	unsigned y = (opcode >> 3) & 7;
	unsigned z = opcode & 7;
	unsigned p = y >> 1;
	unsigned q = y & 1;
	uint16_t address;
	uint16_t value16;
	int8_t displacement;
	uint8_t value;

	if (opcode == 0xdd) return execute_index(cpu, &cpu->ix) + 4;
	if (opcode == 0xfd) return execute_index(cpu, &cpu->iy) + 4;
	if (opcode == 0xcb) {
		displacement = (int8_t) fetch8(cpu);
		address = (uint16_t) (*index + displacement);
		return execute_index_cb(cpu, address);
	}

	if (x == 0) {
		if (z == 1 && !q && p == 2) {
			*index = fetch16(cpu);
			return 14;
		}
		if (z == 1 && q) {
			value16 = p == 2 ? *index : *pair(cpu, p);
			add_index16(cpu, index, value16);
			return 15;
		}
		if (z == 2 && p == 2) {
			address = fetch16(cpu);
			if (!q) {
				memory_write(cpu, address, low(*index));
				memory_write(cpu, address + 1, high(*index));
			} else {
				*index = (uint16_t) (memory_read(cpu, address) |
					(uint16_t) memory_read(cpu, address + 1) << 8);
			}
			return 20;
		}
		if (z == 3 && p == 2) {
			if (!q) (*index)++;
			else (*index)--;
			return 10;
		}
		if ((z == 4 || z == 5 || z == 6) && (y == 4 || y == 5 || y == 6)) {
			if (y == 6) {
				displacement = (int8_t) fetch8(cpu);
				address = (uint16_t) (*index + displacement);
				if (z == 4) memory_write(cpu, address, increment8(cpu, memory_read(cpu, address)));
				else if (z == 5) memory_write(cpu, address, decrement8(cpu, memory_read(cpu, address)));
				else memory_write(cpu, address, fetch8(cpu));
				return z == 6 ? 19 : 23;
			}
			value = read_index_register(cpu, *index, y);
			if (z == 4) value = increment8(cpu, value);
			else if (z == 5) value = decrement8(cpu, value);
			else value = fetch8(cpu);
			write_index_register(cpu, index, y, value);
			return z == 6 ? 11 : 8;
		}
	} else if (x == 1 && opcode != 0x76 &&
		(y == 4 || y == 5 || y == 6 || z == 4 || z == 5 || z == 6)) {
		if (y == 6 || z == 6) {
			displacement = (int8_t) fetch8(cpu);
			address = (uint16_t) (*index + displacement);
			if (z == 6) value = memory_read(cpu, address);
			else if (y == 6 && z == 4) value = high(cpu->hl);
			else if (y == 6 && z == 5) value = low(cpu->hl);
			else value = read_index_register(cpu, *index, z);
			if (y == 6) memory_write(cpu, address, value);
			else if (z == 6 && y == 4) set_high(&cpu->hl, value);
			else if (z == 6 && y == 5) set_low(&cpu->hl, value);
			else write_index_register(cpu, index, y, value);
			return 19;
		}
		write_index_register(cpu, index, y, read_index_register(cpu, *index, z));
		return 8;
	} else if (x == 2 && (z == 4 || z == 5 || z == 6)) {
		if (z == 6) {
			displacement = (int8_t) fetch8(cpu);
			value = memory_read(cpu, (uint16_t) (*index + displacement));
			arithmetic(cpu, y, value);
			return 19;
		}
		arithmetic(cpu, y, read_index_register(cpu, *index, z));
		return 8;
	} else if (x == 3) {
		if (z == 1 && !q && p == 2) {
			*index = pop16(cpu);
			return 14;
		}
		if (z == 1 && q && p == 2) {
			cpu->pc = *index;
			return 8;
		}
		if (z == 1 && q && p == 3) {
			cpu->sp = *index;
			return 10;
		}
		if (z == 3 && y == 4) {
			value = memory_read(cpu, cpu->sp);
			memory_write(cpu, cpu->sp, low(*index));
			set_low(index, value);
			value = memory_read(cpu, cpu->sp + 1);
			memory_write(cpu, cpu->sp + 1, high(*index));
			set_high(index, value);
			return 23;
		}
		if (z == 5 && !q && p == 2) {
			push16(cpu, *index);
			return 15;
		}
	}

	return execute_prefetched(cpu, opcode);
}

static void accumulator_rotate(struct karashi_z80 *cpu, unsigned operation) {
	uint8_t a = high(cpu->af);
	uint8_t carry_in = (low(cpu->af) & FLAG_C) != 0;
	uint8_t carry_out;
	uint8_t result;

	switch (operation) {
		case 0:
			carry_out = a >> 7;
			result = (uint8_t) ((a << 1) | carry_out);
			break;
		case 1:
			carry_out = a & 1;
			result = (uint8_t) ((a >> 1) | (carry_out << 7));
			break;
		case 2:
			carry_out = a >> 7;
			result = (uint8_t) ((a << 1) | carry_in);
			break;
		default:
			carry_out = a & 1;
			result = (uint8_t) ((a >> 1) | (carry_in << 7));
			break;
	}
	set_high(&cpu->af, result);
	set_low(&cpu->af, (uint8_t) ((low(cpu->af) & (FLAG_S | FLAG_Z | FLAG_PV)) |
		(result & (FLAG_Y | FLAG_X)) | carry_out));
}

static void decimal_adjust(struct karashi_z80 *cpu) {
	uint8_t a = high(cpu->af);
	uint8_t old_flags = low(cpu->af);
	uint8_t correction = 0;
	bool carry = (old_flags & FLAG_C) != 0;
	uint8_t result;

	if ((old_flags & FLAG_H) || (a & 0x0f) > 9) correction |= 0x06;
	if (carry || a > 0x99) {
		correction |= 0x60;
		carry = true;
	}
	result = (old_flags & FLAG_N) ? (uint8_t) (a - correction) : (uint8_t) (a + correction);
	set_high(&cpu->af, result);
	set_low(&cpu->af, (uint8_t) (szpxy(result) | (old_flags & FLAG_N) |
		(((a ^ result) & 0x10) ? FLAG_H : 0) | (carry ? FLAG_C : 0)));
}

static unsigned service_interrupt(struct karashi_z80 *cpu) {
	uint16_t vector;

	if (cpu->nmi_pending) {
		cpu->nmi_pending = false;
		cpu->halted = false;
		cpu->iff2 = cpu->iff1;
		cpu->iff1 = false;
		increment_r(cpu);
		push16(cpu, cpu->pc);
		cpu->pc = 0x0066;
		return 11;
	}
	if (!cpu->irq_pending || !cpu->iff1 || cpu->ei_delay) return 0;
	cpu->halted = false;
	cpu->iff1 = false;
	cpu->iff2 = false;
	increment_r(cpu);
	push16(cpu, cpu->pc);
	if (cpu->interrupt_mode == 2) {
		vector = (uint16_t) ((uint16_t) cpu->i << 8 | 0xff);
		cpu->pc = (uint16_t) (memory_read(cpu, vector) | (uint16_t) memory_read(cpu, vector + 1) << 8);
		return 19;
	}
	cpu->pc = 0x0038;
	return 13;
}

void karashi_z80_init(struct karashi_z80 *cpu, const struct karashi_z80_bus *bus) {
	memset(cpu, 0, sizeof(*cpu));
	if (bus) cpu->bus = *bus;
	karashi_z80_reset(cpu);
}

void karashi_z80_reset(struct karashi_z80 *cpu) {
	struct karashi_z80_bus bus = cpu->bus;
	memset(cpu, 0, sizeof(*cpu));
	cpu->bus = bus;
	cpu->af = 0xffff;
	cpu->sp = 0xffff;
}

void karashi_z80_irq(struct karashi_z80 *cpu, bool asserted) {
	cpu->irq_pending = asserted;
}

void karashi_z80_nmi(struct karashi_z80 *cpu) {
	cpu->nmi_pending = true;
}

unsigned karashi_z80_step(struct karashi_z80 *cpu) {
	uint8_t opcode;
	unsigned x;
	unsigned y;
	unsigned z;
	unsigned p;
	unsigned q;
	unsigned cycles;
	uint16_t address;
	uint16_t temporary;
	uint8_t value;
	uint8_t flags;

	cycles = cpu->prefix_active ? 0 : service_interrupt(cpu);
	if (cycles) {
		cpu->cycles += cycles;
		return cycles;
	}
	if (cpu->halted) {
		increment_r(cpu);
		cpu->cycles += 4;
		if (cpu->ei_delay) cpu->ei_delay--;
		return 4;
	}

	opcode = fetch_opcode(cpu);
	x = opcode >> 6;
	y = (opcode >> 3) & 7;
	z = opcode & 7;
	p = y >> 1;
	q = y & 1;
	cycles = 4;

	if (x == 0) {
		switch (z) {
			case 0:
				if (y == 1) {
					temporary = cpu->af;
					cpu->af = cpu->af_alt;
					cpu->af_alt = temporary;
				} else if (y == 2) {
					value = fetch8(cpu);
					set_high(&cpu->bc, (uint8_t) (high(cpu->bc) - 1));
					if (high(cpu->bc) != 0) {
						cpu->pc = (uint16_t) (cpu->pc + (int8_t) value);
						cycles = 13;
					} else {
						cycles = 8;
					}
				} else if (y == 3) {
					value = fetch8(cpu);
					cpu->pc = (uint16_t) (cpu->pc + (int8_t) value);
					cycles = 12;
				} else if (y >= 4) {
					value = fetch8(cpu);
					if (condition(cpu, y - 4)) {
						cpu->pc = (uint16_t) (cpu->pc + (int8_t) value);
						cycles = 12;
					} else {
						cycles = 7;
					}
				}
				break;
			case 1:
				if (!q) {
					*pair(cpu, p) = fetch16(cpu);
					cycles = 10;
				} else {
					add16(cpu, *pair(cpu, p));
					cycles = 11;
				}
				break;
			case 2:
				if (!q) {
					if (p == 0) memory_write(cpu, cpu->bc, high(cpu->af));
					else if (p == 1) memory_write(cpu, cpu->de, high(cpu->af));
					else if (p == 2) {
						address = fetch16(cpu);
						memory_write(cpu, address, low(cpu->hl));
						memory_write(cpu, address + 1, high(cpu->hl));
					} else memory_write(cpu, fetch16(cpu), high(cpu->af));
				} else {
					if (p == 0) set_high(&cpu->af, memory_read(cpu, cpu->bc));
					else if (p == 1) set_high(&cpu->af, memory_read(cpu, cpu->de));
					else if (p == 2) {
						address = fetch16(cpu);
						cpu->hl = (uint16_t) (memory_read(cpu, address) |
							(uint16_t) memory_read(cpu, address + 1) << 8);
					} else set_high(&cpu->af, memory_read(cpu, fetch16(cpu)));
				}
				cycles = p < 2 ? 7 : (p == 2 ? 16 : 13);
				break;
			case 3:
				if (!q) (*pair(cpu, p))++;
				else (*pair(cpu, p))--;
				cycles = 6;
				break;
			case 4:
				write_register(cpu, y, increment8(cpu, read_register(cpu, y)));
				cycles = y == 6 ? 11 : 4;
				break;
			case 5:
				write_register(cpu, y, decrement8(cpu, read_register(cpu, y)));
				cycles = y == 6 ? 11 : 4;
				break;
			case 6:
				write_register(cpu, y, fetch8(cpu));
				cycles = y == 6 ? 10 : 7;
				break;
			default:
				if (y < 4) accumulator_rotate(cpu, y);
				else if (y == 4) decimal_adjust(cpu);
				else if (y == 5) {
					set_high(&cpu->af, (uint8_t) ~high(cpu->af));
					set_low(&cpu->af, (uint8_t) ((low(cpu->af) & (FLAG_S | FLAG_Z | FLAG_PV | FLAG_C)) |
						(high(cpu->af) & (FLAG_Y | FLAG_X)) | FLAG_H | FLAG_N));
				} else if (y == 6) {
					set_low(&cpu->af, (uint8_t) ((low(cpu->af) & (FLAG_S | FLAG_Z | FLAG_PV)) |
						(high(cpu->af) & (FLAG_Y | FLAG_X)) | FLAG_C));
				} else {
					flags = low(cpu->af);
					set_low(&cpu->af, (uint8_t) ((flags & (FLAG_S | FLAG_Z | FLAG_PV)) |
						(high(cpu->af) & (FLAG_Y | FLAG_X)) | ((flags & FLAG_C) ? FLAG_H : FLAG_C)));
				}
				break;
		}
	} else if (x == 1) {
		if (y == 6 && z == 6) {
			cpu->halted = true;
			cycles = 4;
		} else {
			write_register(cpu, y, read_register(cpu, z));
			cycles = (y == 6 || z == 6) ? 7 : 4;
		}
	} else if (x == 2) {
		arithmetic(cpu, y, read_register(cpu, z));
		cycles = z == 6 ? 7 : 4;
	} else {
		switch (z) {
			case 0:
				if (condition(cpu, y)) {
					cpu->pc = pop16(cpu);
					cycles = 11;
				} else cycles = 5;
				break;
			case 1:
				if (!q) {
					*pair2(cpu, p) = pop16(cpu);
					cycles = 10;
				} else if (p == 0) {
					cpu->pc = pop16(cpu);
					cycles = 10;
				} else if (p == 1) {
					temporary = cpu->bc; cpu->bc = cpu->bc_alt; cpu->bc_alt = temporary;
					temporary = cpu->de; cpu->de = cpu->de_alt; cpu->de_alt = temporary;
					temporary = cpu->hl; cpu->hl = cpu->hl_alt; cpu->hl_alt = temporary;
				} else if (p == 2) {
					cpu->pc = cpu->hl;
				} else {
					cpu->sp = cpu->hl;
					cycles = 6;
				}
				break;
			case 2:
				address = fetch16(cpu);
				if (condition(cpu, y)) cpu->pc = address;
				cycles = 10;
				break;
			case 3:
				if (y == 0) {
					cpu->pc = fetch16(cpu);
					cycles = 10;
				} else if (y == 1) {
					cycles = execute_cb(cpu);
				} else if (y == 2) {
					value = fetch8(cpu);
					port_write(cpu, (uint16_t) ((uint16_t) high(cpu->af) << 8 | value), high(cpu->af));
					cycles = 11;
				} else if (y == 3) {
					value = fetch8(cpu);
					set_high(&cpu->af, port_read(cpu, (uint16_t) ((uint16_t) high(cpu->af) << 8 | value)));
					cycles = 11;
				} else if (y == 4) {
					value = memory_read(cpu, cpu->sp);
					memory_write(cpu, cpu->sp, low(cpu->hl));
					set_low(&cpu->hl, value);
					value = memory_read(cpu, cpu->sp + 1);
					memory_write(cpu, cpu->sp + 1, high(cpu->hl));
					set_high(&cpu->hl, value);
					cycles = 19;
				} else if (y == 5) {
					temporary = cpu->de; cpu->de = cpu->hl; cpu->hl = temporary;
				} else if (y == 6) {
					cpu->iff1 = false;
					cpu->iff2 = false;
				} else {
					cpu->iff1 = true;
					cpu->iff2 = true;
					cpu->ei_delay = 2;
				}
				break;
			case 4:
				address = fetch16(cpu);
				if (condition(cpu, y)) {
					push16(cpu, cpu->pc);
					cpu->pc = address;
					cycles = 17;
				} else cycles = 10;
				break;
			case 5:
				if (!q) {
					push16(cpu, *pair2(cpu, p));
					cycles = 11;
				} else if (p == 0) {
					address = fetch16(cpu);
					push16(cpu, cpu->pc);
					cpu->pc = address;
					cycles = 17;
				} else if (p == 1) {
					cycles = execute_index(cpu, &cpu->ix);
				} else if (p == 2) {
					cycles = execute_ed(cpu);
				} else {
					cycles = execute_index(cpu, &cpu->iy);
				}
				break;
			case 6:
				arithmetic(cpu, y, fetch8(cpu));
				cycles = 7;
				break;
			default:
				push16(cpu, cpu->pc);
				cpu->pc = (uint16_t) (y * 8);
				cycles = 11;
				break;
		}
	}

	if (cpu->ei_delay) cpu->ei_delay--;
	cpu->cycles += cycles;
	return cycles;
}

bool karashi_z80_save(const struct karashi_z80 *cpu, struct karashi_state_writer *writer) {
	const uint16_t words[12] = {
		cpu->af, cpu->bc, cpu->de, cpu->hl, cpu->af_alt, cpu->bc_alt,
		cpu->de_alt, cpu->hl_alt, cpu->ix, cpu->iy, cpu->sp, cpu->pc
	};
	unsigned index;

	for (index = 0; index < 12; index++) karashi_state_write_u16(writer, words[index]);
	karashi_state_write_u64(writer, cpu->cycles);
	karashi_state_write_u8(writer, cpu->i);
	karashi_state_write_u8(writer, cpu->r);
	karashi_state_write_u8(writer, cpu->interrupt_mode);
	karashi_state_write_u8(writer, cpu->ei_delay);
	karashi_state_write_bool(writer, cpu->iff1);
	karashi_state_write_bool(writer, cpu->iff2);
	karashi_state_write_bool(writer, cpu->halted);
	karashi_state_write_bool(writer, cpu->irq_pending);
	karashi_state_write_bool(writer, cpu->nmi_pending);
	karashi_state_write_bool(writer, cpu->prefetched_valid);
	karashi_state_write_bool(writer, cpu->prefix_active);
	return karashi_state_write_u8(writer, cpu->prefetched_opcode);
}

bool karashi_z80_load(struct karashi_z80 *cpu, struct karashi_state_reader *reader) {
	uint16_t *const words[12] = {
		&cpu->af, &cpu->bc, &cpu->de, &cpu->hl, &cpu->af_alt, &cpu->bc_alt,
		&cpu->de_alt, &cpu->hl_alt, &cpu->ix, &cpu->iy, &cpu->sp, &cpu->pc
	};
	unsigned index;

	for (index = 0; index < 12; index++) karashi_state_read_u16(reader, words[index]);
	karashi_state_read_u64(reader, &cpu->cycles);
	karashi_state_read_u8(reader, &cpu->i);
	karashi_state_read_u8(reader, &cpu->r);
	karashi_state_read_u8(reader, &cpu->interrupt_mode);
	karashi_state_read_u8(reader, &cpu->ei_delay);
	karashi_state_read_bool(reader, &cpu->iff1);
	karashi_state_read_bool(reader, &cpu->iff2);
	karashi_state_read_bool(reader, &cpu->halted);
	karashi_state_read_bool(reader, &cpu->irq_pending);
	karashi_state_read_bool(reader, &cpu->nmi_pending);
	karashi_state_read_bool(reader, &cpu->prefetched_valid);
	karashi_state_read_bool(reader, &cpu->prefix_active);
	return karashi_state_read_u8(reader, &cpu->prefetched_opcode) && cpu->interrupt_mode <= 2;
}
