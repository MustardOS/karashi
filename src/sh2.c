#include "sh2.h"

#include <string.h>

/*
 * Written from the Renesas SH7604 hardware manual and the SH-1/SH-2 software manual.
 * The on-chip modules cover what the SEGA 32X uses: interrupt controller, division unit,
 * DMA controller, free-running timer, watchdog timer, cache control and the cache data array.
 */

enum {
	SH2_T = 0x00000001,
	SH2_S = 0x00000002,
	SH2_I = 0x000000f0,
	SH2_Q = 0x00000100,
	SH2_M = 0x00000200,
	SH2_SR_MASK = SH2_T | SH2_S | SH2_I | SH2_Q | SH2_M
};

enum {
	CHCR_DE = 0x0001,
	CHCR_TE = 0x0002,
	CHCR_IE = 0x0004,
	CHCR_AR = 0x0200,
	DMAOR_DME = 0x0001,
	DMAOR_NMIF = 0x0002,
	DMAOR_AE = 0x0004,
	DVCR_OVF = 0x0001,
	DVCR_OVFIE = 0x0002,
	FTCSR_CCLRA = 0x01,
	FTCSR_OVF = 0x02,
	FTCSR_OCFB = 0x04,
	FTCSR_OCFA = 0x08,
	WTCSR_TME = 0x20,
	WTCSR_INTERVAL = 0x40,
	WTCSR_OVF = 0x80,
	SCR_TIE = 0x80,
	SCR_RIE = 0x40,
	SCR_TE = 0x20,
	SCR_RE = 0x10,
	SCR_TEIE = 0x04,
	SSR_TDRE = 0x80,
	SSR_RDRF = 0x40,
	SSR_ORER = 0x20,
	SSR_ERRORS = 0x38,
	SSR_TEND = 0x04
};

static uint8_t io_byte(struct karashi_sh2 *cpu, unsigned offset);
static void flush_timers(struct karashi_sh2 *cpu);
static void io_write(struct karashi_sh2 *cpu, unsigned offset, unsigned size, uint32_t value);

static uint32_t serial_time(const struct karashi_sh2 *cpu) {
	unsigned scale = 1u << (2 * (cpu->serial[0] & 3));
	uint32_t time = (uint32_t) (cpu->serial[1] + 1u) * scale * (cpu->serial[0] & 0x80 ? 64u : 640u);
	return time < 16 ? 16 : time;
}

static void serial_start(struct karashi_sh2 *cpu) {
	if (cpu->serial_busy) return;
	cpu->serial_shift = cpu->serial[3];
	cpu->serial_busy = true;
	cpu->serial_countdown = serial_time(cpu);
	cpu->serial[4] = (uint8_t) ((cpu->serial[4] | SSR_TDRE) & ~SSR_TEND);
}

static void serial_advance(struct karashi_sh2 *cpu, unsigned cycles) {
	if (!cpu->serial_busy) return;
	if (cpu->serial_countdown > cycles) {
		cpu->serial_countdown -= cycles;
		return;
	}
	cpu->serial_busy = false;
	if (cpu->bus.serial) cpu->bus.serial(cpu->bus.context, cpu->serial_shift);
	if (!(cpu->serial[4] & SSR_TDRE) && (cpu->serial[2] & SCR_TE)) serial_start(cpu);
	else cpu->serial[4] |= SSR_TEND;
}

void karashi_sh2_serial_receive(struct karashi_sh2 *cpu, uint8_t value) {
	cpu->interrupt_check = true;
	if (!(cpu->serial[2] & SCR_RE)) return;
	if (cpu->serial[4] & SSR_RDRF) {
		cpu->serial[4] |= SSR_ORER;
		return;
	}
	cpu->serial[5] = value;
	cpu->serial[4] |= SSR_RDRF;
}
static uint32_t io_long(struct karashi_sh2 *cpu, unsigned offset);
static void io_write(struct karashi_sh2 *cpu, unsigned offset, unsigned size, uint32_t value);

static uint32_t external(uint32_t address) {
	return address & 0x1fffffff;
}

static bool on_chip(uint32_t address) {
	return address >= 0xfffffe00;
}

static bool cache_data(uint32_t address) {
	return (address >> 29) == 6;
}

static bool cache_control_area(uint32_t address) {
	return (address >> 29) == 2 || (address >> 29) == 3;
}

static uint8_t *direct(const struct karashi_sh2 *cpu, uint32_t address) {
	unsigned region = (address >> 24) & 0x1f;
	uint8_t *base = cpu->bus.direct[region];

	if (!(address >> 30) && base) return base + (address & cpu->bus.direct_mask[region]);
	if (!(address >> 30) && (address & 0x1fffffff) < 0x4000 && cpu->bus.boot) {
		return (uint8_t *) (uintptr_t) (cpu->bus.boot + (address & cpu->bus.boot_mask));
	}
	return NULL;
}

static uint8_t *direct_store(const struct karashi_sh2 *cpu, uint32_t address) {
	return cpu->bus.direct_write[(address >> 24) & 0x1f] ? direct(cpu, address) : NULL;
}

static uint8_t read8(struct karashi_sh2 *cpu, uint32_t address) {
	const uint8_t *data = direct(cpu, address);

	if (data) return *data;
	if (on_chip(address)) return io_byte(cpu, address & 0x1ff);
	if (cache_data(address)) return cpu->cache_ram[address & (KARASHI_SH2_CACHE_RAM - 1)];
	if (cache_control_area(address) || (address >> 29) == 7) return 0;
	return cpu->bus.read ? cpu->bus.read(cpu->bus.context, external(address)) : 0xff;
}

static void address_error(struct karashi_sh2 *cpu) {
	cpu->address_error = true;
}

static uint16_t read16(struct karashi_sh2 *cpu, uint32_t address) {
	const uint8_t *data;

	if (address & 1) {
		address_error(cpu);
		return 0;
	}
	data = direct(cpu, address);
	if (data) return (uint16_t) ((uint16_t) data[0] << 8 | data[1]);
	if (on_chip(address)) {
		if (address >= 0xffffff00) return (uint16_t) (io_long(cpu, address & 0x1fc) >> (address & 2 ? 0 : 16));
		return (uint16_t) ((uint16_t) io_byte(cpu, address & 0x1ff) << 8 | io_byte(cpu, (address + 1) & 0x1ff));
	}
	if (!cache_data(address) && !cache_control_area(address) && (address >> 29) != 7 && cpu->bus.read16) {
		return cpu->bus.read16(cpu->bus.context, external(address));
	}
	return (uint16_t) ((uint16_t) read8(cpu, address) << 8 | read8(cpu, address + 1));
}

static uint32_t read32(struct karashi_sh2 *cpu, uint32_t address) {
	const uint8_t *data;

	if (address & 3) {
		address_error(cpu);
		return 0;
	}
	data = direct(cpu, address);
	if (data) return (uint32_t) data[0] << 24 | (uint32_t) data[1] << 16 | (uint32_t) data[2] << 8 | data[3];
	if (on_chip(address) && address >= 0xffffff00) return io_long(cpu, address & 0x1fc);
	if (!on_chip(address) && !cache_data(address) && !cache_control_area(address) && (address >> 29) != 7
	    && cpu->bus.read32) {
		return cpu->bus.read32(cpu->bus.context, external(address));
	}
	return (uint32_t) read16(cpu, address) << 16 | read16(cpu, address + 2);
}

static void write8(struct karashi_sh2 *cpu, uint32_t address, uint8_t value) {
	uint8_t *data = direct_store(cpu, address);

	cpu->stores++;
	if (data) *data = value;
	else if (on_chip(address)) io_write(cpu, address & 0x1ff, 1, value);
	else if (cache_data(address)) cpu->cache_ram[address & (KARASHI_SH2_CACHE_RAM - 1)] = value;
	else if (cache_control_area(address) || (address >> 29) == 7) return;
	else if (cpu->bus.write) cpu->bus.write(cpu->bus.context, external(address), value);
}

static void write16(struct karashi_sh2 *cpu, uint32_t address, uint16_t value) {
	uint8_t *data;

	cpu->stores++;
	if (address & 1) {
		address_error(cpu);
		return;
	}
	data = direct_store(cpu, address);
	if (data) {
		data[0] = (uint8_t) (value >> 8);
		data[1] = (uint8_t) value;
	} else if (on_chip(address)) io_write(cpu, address & 0x1ff, 2, value);
	else if (!cache_data(address) && !cache_control_area(address) && (address >> 29) != 7 && cpu->bus.write16) {
		cpu->bus.write16(cpu->bus.context, external(address), value);
	} else {
		write8(cpu, address, (uint8_t) (value >> 8));
		write8(cpu, address + 1, (uint8_t) value);
	}
}

static void write32(struct karashi_sh2 *cpu, uint32_t address, uint32_t value) {
	uint8_t *data;

	cpu->stores++;
	if (address & 3) {
		address_error(cpu);
		return;
	}
	data = direct_store(cpu, address);
	if (data) {
		data[0] = (uint8_t) (value >> 24);
		data[1] = (uint8_t) (value >> 16);
		data[2] = (uint8_t) (value >> 8);
		data[3] = (uint8_t) value;
	} else if (on_chip(address)) io_write(cpu, address & 0x1ff, 4, value);
	else if (!cache_data(address) && !cache_control_area(address) && (address >> 29) != 7 && cpu->bus.write32) {
		cpu->bus.write32(cpu->bus.context, external(address), value);
	} else {
		write16(cpu, address, (uint16_t) (value >> 16));
		write16(cpu, address + 2, (uint16_t) value);
	}
}

static uint32_t transfer_read(struct karashi_sh2 *cpu, uint32_t address, unsigned size) {
	if (size == 1) return read8(cpu, address);
	if (size == 2) return read16(cpu, address);
	return read32(cpu, address);
}

static void transfer_write(struct karashi_sh2 *cpu, uint32_t address, unsigned size, uint32_t value) {
	if (size == 1) write8(cpu, address, (uint8_t) value);
	else if (size == 2) write16(cpu, address, (uint16_t) value);
	else write32(cpu, address, value);
}

static bool dma_ready(const struct karashi_sh2 *cpu, unsigned channel) {
	const struct karashi_sh2_dmac *dma = &cpu->dma[channel];

	return (cpu->dma_operation & (DMAOR_DME | DMAOR_NMIF | DMAOR_AE)) == DMAOR_DME
	       && (dma->control & (CHCR_DE | CHCR_TE)) == CHCR_DE;
}

static uint32_t step_address(uint32_t address, unsigned mode, unsigned size) {
	if (mode == 1) return address + size;
	if (mode == 2) return address - size;
	return address;
}

static void dma_unit(struct karashi_sh2 *cpu, unsigned channel) {
	struct karashi_sh2_dmac *dma = &cpu->dma[channel];
	unsigned size_code = (dma->control >> 10) & 3;
	unsigned size = size_code == 3 ? 16 : 1u << size_code;
	unsigned source_mode = (dma->control >> 12) & 3;
	unsigned destination_mode = (dma->control >> 14) & 3;
	unsigned part;

	if (size == 16) {
		for (part = 0; part < 4; part++) {
			transfer_write(cpu, dma->destination + (destination_mode ? part * 4 : 0), 4,
			               transfer_read(cpu, dma->source + part * 4, 4));
		}
		dma->source += 16;
		dma->destination = step_address(dma->destination, destination_mode, 16);
		dma->count = (dma->count - 4) & 0xffffff;
	} else {
		transfer_write(cpu, dma->destination, size, transfer_read(cpu, dma->source, size));
		dma->source = step_address(dma->source, source_mode, size);
		dma->destination = step_address(dma->destination, destination_mode, size);
		dma->count = (dma->count - 1) & 0xffffff;
	}
	if (cpu->address_error) {
		cpu->address_error = false;
		cpu->dma_operation |= DMAOR_AE;
		return;
	}
	if (!dma->count) dma->control |= CHCR_TE;
}

static void dma_auto(struct karashi_sh2 *cpu, unsigned channel) {
	unsigned limit = 0x1000000;

	while (dma_ready(cpu, channel) && (cpu->dma[channel].control & CHCR_AR) && limit--) dma_unit(cpu, channel);
}

bool karashi_sh2_dreq(struct karashi_sh2 *cpu, unsigned channel) {
	if (channel > 1 || !dma_ready(cpu, channel) || (cpu->dma[channel].control & CHCR_AR)) return false;
	dma_unit(cpu, channel);
	cpu->interrupt_check = true;
	return true;
}

static void divide(struct karashi_sh2 *cpu) {
	int64_t dividend = (int64_t) ((uint64_t) cpu->dividend_high << 32 | cpu->dividend_low);
	int32_t divisor = (int32_t) cpu->divisor;
	int64_t quotient;

	if (divisor == 0) {
		cpu->divide_control |= DVCR_OVF;
		cpu->dividend_low = dividend < 0 ? 0x80000000 : 0x7fffffff;
		return;
	}
	if (dividend == INT64_MIN && divisor == -1) quotient = INT64_MAX;
	else quotient = dividend / divisor;
	if (quotient > INT32_MAX || quotient < INT32_MIN) {
		cpu->divide_control |= DVCR_OVF;
		cpu->dividend_low = quotient < 0 ? 0x80000000 : 0x7fffffff;
		return;
	}
	cpu->dividend_high = (uint32_t) (int32_t) (dividend - quotient * divisor);
	cpu->dividend_low = (uint32_t) (int32_t) quotient;
}

static uint32_t io_long(struct karashi_sh2 *cpu, unsigned offset) {
	flush_timers(cpu);
	switch (offset & 0x1fc) {
		case 0x100:
		case 0x120: return cpu->divisor;
		case 0x104:
		case 0x124:
		case 0x114:
		case 0x134:
		case 0x11c:
		case 0x13c: return cpu->dividend_low;
		case 0x108:
		case 0x128: return cpu->divide_control;
		case 0x10c:
		case 0x12c: return cpu->divide_vector;
		case 0x110:
		case 0x130:
		case 0x118:
		case 0x138: return cpu->dividend_high;
		case 0x180: return cpu->dma[0].source;
		case 0x184: return cpu->dma[0].destination;
		case 0x188: return cpu->dma[0].count;
		case 0x18c: return cpu->dma[0].control;
		case 0x190: return cpu->dma[1].source;
		case 0x194: return cpu->dma[1].destination;
		case 0x198: return cpu->dma[1].count;
		case 0x19c: return cpu->dma[1].control;
		case 0x1a0: return cpu->dma[0].vector;
		case 0x1a8: return cpu->dma[1].vector;
		case 0x1b0: return cpu->dma_operation;
		default:
			if (offset >= 0x1e0) return cpu->bus_control[(offset - 0x1e0) >> 2];
			return 0;
	}
}

static uint16_t io_word(struct karashi_sh2 *cpu, unsigned offset) {
	switch (offset & 0xfe) {
		case 0x60: return cpu->priority_b;
		case 0x62: return cpu->vector_a;
		case 0x64: return cpu->vector_b;
		case 0x66: return cpu->vector_c;
		case 0x68: return cpu->vector_d;
		case 0xe0: return cpu->interrupt_control;
		case 0xe2: return cpu->priority_a;
		case 0xe4: return cpu->vector_wdt;
		default: return 0;
	}
}

static uint8_t io_byte(struct karashi_sh2 *cpu, unsigned offset) {
	uint16_t value;

	flush_timers(cpu);
	if (offset >= 0x100) return (uint8_t) (io_long(cpu, offset & 0x1fc) >> ((3 - (offset & 3)) * 8));
	switch (offset) {
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x05: return cpu->serial[offset];
		case 0x04: return cpu->serial[4];
		case 0x10: return cpu->frt_enable;
		case 0x11: return cpu->frt_status;
		case 0x12:
			cpu->frt_temp = (uint8_t) cpu->frt_counter;
			return (uint8_t) (cpu->frt_counter >> 8);
		case 0x13: return cpu->frt_temp;
		case 0x14: return (uint8_t) (cpu->frt_compare[(cpu->frt_output >> 4) & 1] >> 8);
		case 0x15: return (uint8_t) cpu->frt_compare[(cpu->frt_output >> 4) & 1];
		case 0x16: return cpu->frt_control;
		case 0x17: return cpu->frt_output;
		case 0x71: return cpu->dma[0].request;
		case 0x72: return cpu->dma[1].request;
		case 0x80: return (uint8_t) (cpu->wdt_control | 0x18);
		case 0x81: return cpu->wdt_counter;
		case 0x83: return (uint8_t) (cpu->wdt_reset | 0x1f);
		case 0x92: return cpu->cache_control;
		default:
			value = io_word(cpu, offset);
			return (uint8_t) (offset & 1 ? value : value >> 8);
	}
}

static void io_write_long(struct karashi_sh2 *cpu, unsigned offset, uint32_t value) {
	switch (offset & 0x1fc) {
		case 0x100:
		case 0x120: cpu->divisor = value; break;
		case 0x104:
		case 0x124:
			cpu->dividend_low = value;
			cpu->dividend_high = value & 0x80000000 ? 0xffffffff : 0;
			divide(cpu);
			break;
		case 0x108:
		case 0x128: cpu->divide_control = (uint16_t) (value & 3); break;
		case 0x10c:
		case 0x12c: cpu->divide_vector = value & 0x7f; break;
		case 0x110:
		case 0x130:
		case 0x118:
		case 0x138: cpu->dividend_high = value; break;
		case 0x114:
		case 0x134:
		case 0x11c:
		case 0x13c:
			cpu->dividend_low = value;
			divide(cpu);
			break;
		case 0x180: cpu->dma[0].source = value; break;
		case 0x184: cpu->dma[0].destination = value; break;
		case 0x188: cpu->dma[0].count = value & 0xffffff; break;
		case 0x18c:
			cpu->dma[0].control = (value & 0xfffd) | (cpu->dma[0].control & value & CHCR_TE);
			dma_auto(cpu, 0);
			break;
		case 0x190: cpu->dma[1].source = value; break;
		case 0x194: cpu->dma[1].destination = value; break;
		case 0x198: cpu->dma[1].count = value & 0xffffff; break;
		case 0x19c:
			cpu->dma[1].control = (value & 0xfffd) | (cpu->dma[1].control & value & CHCR_TE);
			dma_auto(cpu, 1);
			break;
		case 0x1a0: cpu->dma[0].vector = value & 0x7f; break;
		case 0x1a8: cpu->dma[1].vector = value & 0x7f; break;
		case 0x1b0:
			cpu->dma_operation = (uint16_t) ((value & 0x9) | (cpu->dma_operation & value & 6));
			dma_auto(cpu, 0);
			dma_auto(cpu, 1);
			break;
		default:
			if (offset >= 0x1e0) cpu->bus_control[(offset - 0x1e0) >> 2] = (uint16_t) value;
			break;
	}
}

static void io_write_byte(struct karashi_sh2 *cpu, unsigned offset, uint8_t value) {
	switch (offset) {
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03:
		case 0x05: cpu->serial[offset] = value; break;
		case 0x04: {
			bool start = (cpu->serial[4] & SSR_TDRE) && !(value & SSR_TDRE);
			cpu->serial[4] = (uint8_t) ((cpu->serial[4] & (value | 0x07) & 0xfe) | (value & 1));
			if (start && (cpu->serial[2] & SCR_TE)) serial_start(cpu);
			break;
		}
		case 0x10: cpu->frt_enable = (uint8_t) (value | 1); break;
		case 0x11: cpu->frt_status = (uint8_t) ((cpu->frt_status & value & 0x8e) | (value & FTCSR_CCLRA)); break;
		case 0x12: cpu->frt_temp = value; break;
		case 0x13: cpu->frt_counter = (uint16_t) (cpu->frt_temp << 8 | value); break;
		case 0x14: cpu->frt_temp = value; break;
		case 0x15: cpu->frt_compare[(cpu->frt_output >> 4) & 1] = (uint16_t) (cpu->frt_temp << 8 | value); break;
		case 0x16: cpu->frt_control = value; break;
		case 0x17: cpu->frt_output = (uint8_t) (value | 0xe0); break;
		case 0x71: cpu->dma[0].request = value & 3; break;
		case 0x72: cpu->dma[1].request = value & 3; break;
		case 0x92: cpu->cache_control = (uint8_t) (value & ~0x10); break;
		default: break;
	}
}

static void io_write_word(struct karashi_sh2 *cpu, unsigned offset, uint16_t value) {
	switch (offset & 0xfe) {
		case 0x60: cpu->priority_b = value & 0xff00; break;
		case 0x62: cpu->vector_a = value & 0x7f7f; break;
		case 0x64: cpu->vector_b = value & 0x7f7f; break;
		case 0x66: cpu->vector_c = value & 0x7f7f; break;
		case 0x68: cpu->vector_d = value & 0x7f00; break;
		case 0x80:
			if ((value >> 8) == 0xa5) {
				cpu->wdt_control = (uint8_t) ((cpu->wdt_control & value & WTCSR_OVF) | (value & 0x67));
			} else if ((value >> 8) == 0x5a) cpu->wdt_counter = (uint8_t) value;
			break;
		case 0x82:
			if ((value >> 8) == 0xa5) cpu->wdt_reset &= (uint8_t) (value | 0x7f);
			else if ((value >> 8) == 0x5a) cpu->wdt_reset = (uint8_t) ((cpu->wdt_reset & 0x80) | (value & 0x60));
			break;
		case 0xe0: cpu->interrupt_control = (uint16_t) ((cpu->interrupt_control & 0x8000) | (value & 0x0101)); break;
		case 0xe2: cpu->priority_a = value & 0xfff0; break;
		case 0xe4: cpu->vector_wdt = value & 0x7f7f; break;
		default:
			io_write_byte(cpu, offset & 0xfe, (uint8_t) (value >> 8));
			io_write_byte(cpu, (offset & 0xfe) + 1, (uint8_t) value);
			break;
	}
}

static void io_store(struct karashi_sh2 *cpu, unsigned offset, unsigned size, uint32_t value) {
	if (offset >= 0x100) {
		uint32_t current;
		unsigned shift;
		if (size == 4) {
			io_write_long(cpu, offset, value);
			return;
		}
		current = io_long(cpu, offset & 0x1fc);
		shift = (unsigned) (size == 2 ? (2 - (offset & 2)) * 8 : (3 - (offset & 3)) * 8);
		current &= ~((size == 2 ? 0xffffu : 0xffu) << shift);
		current |= (value & (size == 2 ? 0xffffu : 0xffu)) << shift;
		if ((offset & 0x1fc) == 0x104 || (offset & 0x1fc) == 0x124) {
			if (size == 2 && !(offset & 2)) {
				cpu->dividend_low = current;
				return;
			}
		}
		io_write_long(cpu, offset & 0x1fc, current);
		return;
	}
	if (size == 1) {
		if (offset >= 0x60 && offset < 0x6a) {
			uint16_t word = io_word(cpu, offset);
			word = (uint16_t) (offset & 1 ? (word & 0xff00) | value : (word & 0x00ff) | (value << 8));
			io_write_word(cpu, offset, word);
		} else if (offset >= 0xe0 && offset < 0xe6) {
			uint16_t word = io_word(cpu, offset);
			word = (uint16_t) (offset & 1 ? (word & 0xff00) | value : (word & 0x00ff) | (value << 8));
			io_write_word(cpu, offset, word);
		} else io_write_byte(cpu, offset, (uint8_t) value);
	} else if (size == 2) io_write_word(cpu, offset, (uint16_t) value);
	else {
		io_write_word(cpu, offset, (uint16_t) (value >> 16));
		io_write_word(cpu, offset + 2, (uint16_t) value);
	}
}

static uint32_t frt_distance(uint16_t from, uint16_t to) {
	uint32_t distance = (uint16_t) (to - from);
	return distance ? distance : 0x10000;
}

static void frt_count(struct karashi_sh2 *cpu, uint32_t ticks) {
	while (ticks) {
		uint32_t to_a = frt_distance(cpu->frt_counter, cpu->frt_compare[0]);
		uint32_t to_b = frt_distance(cpu->frt_counter, cpu->frt_compare[1]);
		uint32_t to_overflow = 0x10000u - cpu->frt_counter;
		uint32_t step = ticks;
		if (to_a < step) step = to_a;
		if (to_b < step) step = to_b;
		if (to_overflow < step) step = to_overflow;
		cpu->frt_counter = (uint16_t) (cpu->frt_counter + step);
		ticks -= step;
		if (step == to_overflow) cpu->frt_status |= FTCSR_OVF;
		if (step == to_b) cpu->frt_status |= FTCSR_OCFB;
		if (step == to_a) {
			cpu->frt_status |= FTCSR_OCFA;
			if (cpu->frt_status & FTCSR_CCLRA) cpu->frt_counter = 0;
		}
	}
}

static void advance_timers(struct karashi_sh2 *cpu, unsigned cycles) {
	static const uint16_t wdt_divider[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
	unsigned select = cpu->frt_control & 3;

	serial_advance(cpu, cycles);
	if (select != 3) {
		unsigned divider = 8u << (select * 2);
		uint32_t ticks;
		cpu->frt_clock += cycles;
		if (cpu->frt_clock >= divider) {
			ticks = cpu->frt_clock / divider;
			cpu->frt_clock %= divider;
			frt_count(cpu, ticks);
		}
	}
	if (cpu->wdt_control & WTCSR_TME) {
		unsigned divider = wdt_divider[cpu->wdt_control & 7];
		cpu->wdt_clock += cycles;
		while (cpu->wdt_clock >= divider) {
			cpu->wdt_clock -= divider;
			if (!++cpu->wdt_counter) {
				if (cpu->wdt_control & WTCSR_INTERVAL) cpu->wdt_reset |= 0x80;
				else cpu->wdt_control |= WTCSR_OVF;
			}
		}
	}
}

static uint32_t timer_limit(const struct karashi_sh2 *cpu) {
	static const uint16_t wdt_divider[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
	uint64_t limit = 1u << 20;
	unsigned select = cpu->frt_control & 3;

	if (select != 3 && (cpu->frt_enable & 0x0e)) {
		uint64_t divider = 8u << (select * 2);
		uint64_t ticks = frt_distance(cpu->frt_counter, cpu->frt_compare[0]);
		uint64_t other = frt_distance(cpu->frt_counter, cpu->frt_compare[1]);
		uint64_t overflow = 0x10000u - cpu->frt_counter;
		uint64_t cycles;
		if (other < ticks) ticks = other;
		if (overflow < ticks) ticks = overflow;
		cycles = ticks * divider - cpu->frt_clock;
		if (cycles < limit) limit = cycles;
	}
	if (cpu->wdt_control & WTCSR_TME) {
		uint64_t cycles = (uint64_t) (256u - cpu->wdt_counter) * wdt_divider[cpu->wdt_control & 7] - cpu->wdt_clock;
		if (cycles < limit) limit = cycles;
	}
	if (cpu->serial_busy && cpu->serial_countdown < limit) limit = cpu->serial_countdown;
	return limit ? (uint32_t) limit : 1;
}

static void flush_timers(struct karashi_sh2 *cpu) {
	uint32_t debt = cpu->timer_debt;

	if (debt) {
		cpu->timer_debt = 0;
		advance_timers(cpu, debt);
		cpu->interrupt_check = true;
	}
	cpu->timer_limit = timer_limit(cpu);
}

static void io_write(struct karashi_sh2 *cpu, unsigned offset, unsigned size, uint32_t value) {
	flush_timers(cpu);
	cpu->interrupt_check = true;
	io_store(cpu, offset, size, value);
	cpu->timer_limit = timer_limit(cpu);
}

static unsigned pending_interrupt(const struct karashi_sh2 *cpu, unsigned *vector) {
	unsigned level = cpu->irl;
	unsigned priority;
	unsigned channel;

	*vector = 64 + cpu->irl / 2;
	priority = cpu->priority_a >> 12;
	if ((cpu->divide_control & (DVCR_OVF | DVCR_OVFIE)) == (DVCR_OVF | DVCR_OVFIE) && priority > level) {
		level = priority;
		*vector = cpu->divide_vector & 0x7f;
	}
	priority = (cpu->priority_a >> 8) & 15;
	for (channel = 0; channel < 2; channel++) {
		if ((cpu->dma[channel].control & (CHCR_TE | CHCR_IE)) == (CHCR_TE | CHCR_IE) && priority > level) {
			level = priority;
			*vector = cpu->dma[channel].vector & 0x7f;
		}
	}
	priority = (cpu->priority_a >> 4) & 15;
	if ((cpu->wdt_control & (WTCSR_OVF | WTCSR_INTERVAL)) == WTCSR_OVF && priority > level) {
		level = priority;
		*vector = (cpu->vector_wdt >> 8) & 0x7f;
	}
	priority = cpu->priority_b >> 12;
	if (priority > level) {
		uint8_t status = cpu->serial[4];
		uint8_t control = cpu->serial[2];
		if ((status & SSR_ERRORS) && (control & SCR_RIE)) {
			level = priority;
			*vector = (cpu->vector_a >> 8) & 0x7f;
		} else if ((status & SSR_RDRF) && (control & SCR_RIE)) {
			level = priority;
			*vector = cpu->vector_a & 0x7f;
		} else if ((status & SSR_TDRE) && (control & SCR_TIE)) {
			level = priority;
			*vector = (cpu->vector_b >> 8) & 0x7f;
		} else if ((status & SSR_TEND) && (control & SCR_TEIE)) {
			level = priority;
			*vector = cpu->vector_b & 0x7f;
		}
	}
	priority = (cpu->priority_b >> 8) & 15;
	if (priority > level) {
		if ((cpu->frt_status & FTCSR_OCFA) && (cpu->frt_enable & 0x08)) {
			level = priority;
			*vector = cpu->vector_c & 0x7f;
		} else if ((cpu->frt_status & FTCSR_OCFB) && (cpu->frt_enable & 0x04)) {
			level = priority;
			*vector = cpu->vector_c & 0x7f;
		} else if ((cpu->frt_status & FTCSR_OVF) && (cpu->frt_enable & 0x02)) {
			level = priority;
			*vector = (cpu->vector_d >> 8) & 0x7f;
		}
	}
	return level;
}

static int32_t sign8(uint32_t value) {
	return (int8_t) value;
}

static int32_t sign12(uint32_t value) {
	value &= 0xfff;
	return value & 0x800 ? (int32_t) (value | 0xfffff000) : (int32_t) value;
}

static void set_t(struct karashi_sh2 *cpu, bool value) {
	if (value) cpu->sr |= SH2_T;
	else cpu->sr &= ~SH2_T;
}

static void delay(struct karashi_sh2 *cpu, uint32_t target) {
	cpu->delay_target = target;
	cpu->delay_pending = true;
}

static unsigned take_exception(struct karashi_sh2 *cpu, unsigned vector, uint32_t saved_pc, unsigned cycles) {
	uint32_t saved_sr = cpu->sr;

	cpu->address_error = false;
	cpu->r[15] -= 4;
	write32(cpu, cpu->r[15], saved_sr);
	cpu->r[15] -= 4;
	write32(cpu, cpu->r[15], saved_pc);
	cpu->pc = read32(cpu, cpu->vbr + vector * 4);
	if (cpu->address_error) cpu->fault = KARASHI_SH2_FAULT_ADDRESS;
	cpu->address_error = false;
	cpu->delay_pending = false;
	cpu->stopped = false;
	cpu->interrupt_check = true;
	return cycles;
}

static uint32_t add_carry(struct karashi_sh2 *cpu, uint32_t destination, uint32_t source) {
	uint64_t result = (uint64_t) destination + source + ((cpu->sr & SH2_T) != 0);

	set_t(cpu, result > UINT32_MAX);
	return (uint32_t) result;
}

static uint32_t subtract_borrow(struct karashi_sh2 *cpu, uint32_t destination, uint32_t source) {
	unsigned borrow = (cpu->sr & SH2_T) != 0;
	uint32_t result = destination - source - borrow;

	set_t(cpu, (uint64_t) source + borrow > destination);
	return result;
}

static void shift_one(struct karashi_sh2 *cpu, unsigned reg, bool left, bool arithmetic, bool rotate, bool carry) {
	uint32_t value = cpu->r[reg];
	bool old_t = (cpu->sr & SH2_T) != 0;
	bool bit = left ? (value & 0x80000000) != 0 : (value & 1) != 0;

	if (left) value <<= 1;
	else value = arithmetic ? (uint32_t) ((int32_t) value >> 1) : value >> 1;
	if (rotate) {
		if (left) value |= carry ? old_t : bit;
		else if (carry ? old_t : bit) value |= 0x80000000;
	}
	cpu->r[reg] = value;
	set_t(cpu, bit);
}

static unsigned illegal(struct karashi_sh2 *cpu) {
	if (cpu->fault == KARASHI_SH2_FAULT_NONE) cpu->fault = KARASHI_SH2_FAULT_ILLEGAL;
	return 1;
}

static bool changes_pc_in_delay_slot(uint16_t opcode) {
	if (opcode == 0x000b || opcode == 0x002b) return true;
	if ((opcode & 0xf0ff) == 0x0003 || (opcode & 0xf0ff) == 0x0023) return true;
	if ((opcode & 0xf0ff) == 0x400b || (opcode & 0xf0ff) == 0x402b) return true;
	if ((opcode >> 12) == 10 || (opcode >> 12) == 11 || (opcode >> 8) == 0xc3) return true;
	return (opcode >> 8) == 0x89 || (opcode >> 8) == 0x8b
	       || (opcode >> 8) == 0x8d || (opcode >> 8) == 0x8f;
}

static void divide_step(struct karashi_sh2 *cpu, unsigned n, unsigned m) {
	bool old_q = (cpu->sr & SH2_Q) != 0;
	bool q = (cpu->r[n] & 0x80000000) != 0;
	bool divisor_negative = (cpu->sr & SH2_M) != 0;
	uint32_t previous;
	bool carry_or_borrow;

	cpu->r[n] = cpu->r[n] << 1 | ((cpu->sr & SH2_T) != 0);
	previous = cpu->r[n];
	if (!old_q && !divisor_negative) {
		cpu->r[n] -= cpu->r[m];
		carry_or_borrow = cpu->r[n] > previous;
		q = q ? !carry_or_borrow : carry_or_borrow;
	} else if (!old_q && divisor_negative) {
		cpu->r[n] += cpu->r[m];
		carry_or_borrow = cpu->r[n] < previous;
		q = q ? carry_or_borrow : !carry_or_borrow;
	} else if (!divisor_negative) {
		cpu->r[n] += cpu->r[m];
		carry_or_borrow = cpu->r[n] < previous;
		q = q ? !carry_or_borrow : carry_or_borrow;
	} else {
		cpu->r[n] -= cpu->r[m];
		carry_or_borrow = cpu->r[n] > previous;
		q = q ? carry_or_borrow : !carry_or_borrow;
	}
	if (q) cpu->sr |= SH2_Q;
	else cpu->sr &= ~SH2_Q;
	set_t(cpu, q == divisor_negative);
}

static unsigned accumulate_long(struct karashi_sh2 *cpu, unsigned n, unsigned m) {
	int64_t left = (int32_t) read32(cpu, cpu->r[n]);
	int64_t right;
	int64_t result;
	uint64_t stored;

	cpu->r[n] += 4;
	right = (int32_t) read32(cpu, cpu->r[m]);
	cpu->r[m] += 4;
	if (cpu->sr & SH2_S) {
		uint64_t raw = (uint64_t) (cpu->mach & 0xffff) << 32 | cpu->macl;
		int64_t accumulator = (int64_t) (raw & ((UINT64_C(1) << 47) - 1));

		if (raw & (UINT64_C(1) << 47)) accumulator -= INT64_C(1) << 47;

		result = accumulator + left * right;
		if (result > INT64_C(140737488355327)) result = INT64_C(140737488355327);
		if (result < -INT64_C(140737488355328)) result = -INT64_C(140737488355328);
		stored = (uint64_t) result;
	} else {
		stored = ((uint64_t) cpu->mach << 32 | cpu->macl) + (uint64_t) (left * right);
	}
	cpu->mach = (uint32_t) (stored >> 32);
	cpu->macl = (uint32_t) stored;
	return 3;
}

static unsigned accumulate_word(struct karashi_sh2 *cpu, unsigned n, unsigned m) {
	int64_t left = (int16_t) read16(cpu, cpu->r[n]);
	int64_t right;
	int64_t product;

	cpu->r[n] += 2;
	right = (int16_t) read16(cpu, cpu->r[m]);
	cpu->r[m] += 2;
	product = left * right;
	if (cpu->sr & SH2_S) {
		int64_t result = (int32_t) cpu->macl + product;

		if (result > INT32_MAX) {
			cpu->macl = INT32_MAX;
			cpu->mach |= 1;
		} else if (result < INT32_MIN) {
			cpu->macl = (uint32_t) INT32_MIN;
			cpu->mach |= 1;
		} else cpu->macl = (uint32_t) result;
	} else {
		uint64_t result = ((uint64_t) cpu->mach << 32 | cpu->macl) + (uint64_t) product;

		cpu->mach = (uint32_t) (result >> 32);
		cpu->macl = (uint32_t) result;
	}
	return 3;
}

static unsigned group0(struct karashi_sh2 *cpu, uint16_t opcode, unsigned n) {
	unsigned m = opcode >> 4 & 15;

	if (opcode == 0x0008) { cpu->sr &= ~SH2_T; return 1; }
	if (opcode == 0x0009) return 1;
	if (opcode == 0x000b) {
		delay(cpu, cpu->pr);
		return 2;
	}
	if (opcode == 0x002b) {
		uint32_t target = read32(cpu, cpu->r[15]);
		cpu->r[15] += 4;
		cpu->sr = read32(cpu, cpu->r[15]) & SH2_SR_MASK;
		cpu->interrupt_check = true;
		cpu->r[15] += 4;
		delay(cpu, target);
		return 4;
	}
	if (opcode == 0x0018) { cpu->sr |= SH2_T; return 1; }
	if (opcode == 0x0019) { cpu->sr &= ~(SH2_M | SH2_Q | SH2_T); return 1; }
	if (opcode == 0x001b) {
		cpu->stopped = true;
		return 3;
	}
	if (opcode == 0x0028) { cpu->mach = cpu->macl = 0; return 1; }
	if ((opcode & 0xf0ff) == 0x0003) {
		cpu->pr = cpu->pc + 2;
		delay(cpu, cpu->pc + 2 + cpu->r[n]);
		return 2;
	}
	if ((opcode & 0xf0ff) == 0x0023) {
		delay(cpu, cpu->pc + 2 + cpu->r[n]);
		return 2;
	}
	switch (opcode & 0xf00f) {
		case 0x0004: write8(cpu, cpu->r[0] + cpu->r[n], (uint8_t) cpu->r[m]); return 1;
		case 0x0005: write16(cpu, cpu->r[0] + cpu->r[n], (uint16_t) cpu->r[m]); return 1;
		case 0x0006: write32(cpu, cpu->r[0] + cpu->r[n], cpu->r[m]); return 1;
		case 0x0007: cpu->macl = cpu->r[n] * cpu->r[m]; return 2;
		case 0x000c: cpu->r[n] = (uint32_t) (int32_t) (int8_t) read8(cpu, cpu->r[0] + cpu->r[m]); return 1;
		case 0x000d: cpu->r[n] = (uint32_t) (int32_t) (int16_t) read16(cpu, cpu->r[0] + cpu->r[m]); return 1;
		case 0x000e: cpu->r[n] = read32(cpu, cpu->r[0] + cpu->r[m]); return 1;
		case 0x000f: return accumulate_long(cpu, n, m);
		default: break;
	}
	switch (opcode & 0x00ff) {
		case 0x02: cpu->r[n] = cpu->sr; return 1;
		case 0x0a: cpu->r[n] = cpu->mach; return 1;
		case 0x12: cpu->r[n] = cpu->gbr; return 1;
		case 0x1a: cpu->r[n] = cpu->macl; return 1;
		case 0x22: cpu->r[n] = cpu->vbr; return 1;
		case 0x29: cpu->r[n] = cpu->sr & SH2_T ? 1 : 0; return 1;
		case 0x2a: cpu->r[n] = cpu->pr; return 1;
		default: return illegal(cpu);
	}
}

static unsigned group2(struct karashi_sh2 *cpu, uint16_t opcode, unsigned n, unsigned m) {
	uint32_t value;

	switch (opcode & 15) {
		case 0: write8(cpu, cpu->r[n], (uint8_t) cpu->r[m]); return 1;
		case 1: write16(cpu, cpu->r[n], (uint16_t) cpu->r[m]); return 1;
		case 2: write32(cpu, cpu->r[n], cpu->r[m]); return 1;
		case 4: cpu->r[n] -= 1; write8(cpu, cpu->r[n], (uint8_t) cpu->r[m]); return 1;
		case 5: cpu->r[n] -= 2; write16(cpu, cpu->r[n], (uint16_t) cpu->r[m]); return 1;
		case 6: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->r[m]); return 1;
		case 7:
			if (cpu->r[n] & 0x80000000) cpu->sr |= SH2_Q;
			else cpu->sr &= ~SH2_Q;
			if (cpu->r[m] & 0x80000000) cpu->sr |= SH2_M;
			else cpu->sr &= ~SH2_M;
			set_t(cpu, ((cpu->sr & SH2_Q) != 0) != ((cpu->sr & SH2_M) != 0));
			return 1;
		case 8: set_t(cpu, !(cpu->r[n] & cpu->r[m])); return 1;
		case 9: cpu->r[n] &= cpu->r[m]; return 1;
		case 10: cpu->r[n] ^= cpu->r[m]; return 1;
		case 11: cpu->r[n] |= cpu->r[m]; return 1;
		case 12:
			value = cpu->r[n] ^ cpu->r[m];
			set_t(cpu, !(value & 0xff) || !(value & 0xff00) || !(value & 0xff0000) || !(value & 0xff000000));
			return 1;
		case 13: cpu->r[n] = cpu->r[n] >> 16 | cpu->r[m] << 16; return 1;
		case 14: cpu->macl = (cpu->r[n] & 0xffff) * (cpu->r[m] & 0xffff); return 2;
		case 15: cpu->macl = (uint32_t) ((int32_t) (int16_t) cpu->r[n] * (int32_t) (int16_t) cpu->r[m]); return 2;
		default: return illegal(cpu);
	}
}

static unsigned group3(struct karashi_sh2 *cpu, uint16_t opcode, unsigned n, unsigned m) {
	uint32_t destination = cpu->r[n];
	uint32_t source = cpu->r[m];
	uint32_t result;
	uint64_t product;
	int64_t signed_product;

	switch (opcode & 15) {
		case 0: set_t(cpu, destination == source); return 1;
		case 2: set_t(cpu, destination >= source); return 1;
		case 3: set_t(cpu, (int32_t) destination >= (int32_t) source); return 1;
		case 4: divide_step(cpu, n, m); return 1;
		case 5:
			product = (uint64_t) destination * source;
			cpu->mach = (uint32_t) (product >> 32);
			cpu->macl = (uint32_t) product;
			return 2;
		case 6: set_t(cpu, destination > source); return 1;
		case 7: set_t(cpu, (int32_t) destination > (int32_t) source); return 1;
		case 8: cpu->r[n] = destination - source; return 1;
		case 10: cpu->r[n] = subtract_borrow(cpu, destination, source); return 1;
		case 11:
			result = destination - source;
			cpu->r[n] = result;
			set_t(cpu, ((destination ^ source) & (destination ^ result) & 0x80000000) != 0);
			return 1;
		case 12: cpu->r[n] = destination + source; return 1;
		case 13:
			signed_product = (int64_t) (int32_t) destination * (int32_t) source;
			cpu->mach = (uint32_t) ((uint64_t) signed_product >> 32);
			cpu->macl = (uint32_t) signed_product;
			return 2;
		case 14: cpu->r[n] = add_carry(cpu, destination, source); return 1;
		case 15:
			result = destination + source;
			cpu->r[n] = result;
			set_t(cpu, (~(destination ^ source) & (destination ^ result) & 0x80000000) != 0);
			return 1;
		default: return illegal(cpu);
	}
}

static unsigned group4(struct karashi_sh2 *cpu, uint16_t opcode, unsigned n) {
	if ((opcode & 0xf00f) == 0x400f) return accumulate_word(cpu, n, opcode >> 4 & 15);
	switch (opcode & 0x00ff) {
		case 0x00: shift_one(cpu, n, true, false, false, false); return 1;
		case 0x01: shift_one(cpu, n, false, false, false, false); return 1;
		case 0x02: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->mach); return 1;
		case 0x03: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->sr); return 2;
		case 0x04: shift_one(cpu, n, true, false, true, false); return 1;
		case 0x05: shift_one(cpu, n, false, false, true, false); return 1;
		case 0x06: cpu->mach = read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 1;
		case 0x07: cpu->sr = read32(cpu, cpu->r[n]) & SH2_SR_MASK; cpu->r[n] += 4; cpu->interrupt_check = true; return 3;
		case 0x08: cpu->r[n] <<= 2; return 1;
		case 0x09: cpu->r[n] >>= 2; return 1;
		case 0x0a: cpu->mach = cpu->r[n]; return 1;
		case 0x0b: cpu->pr = cpu->pc + 2; delay(cpu, cpu->r[n]); return 2;
		case 0x10: cpu->r[n]--; set_t(cpu, cpu->r[n] == 0); return 1;
		case 0x11: set_t(cpu, (int32_t) cpu->r[n] >= 0); return 1;
		case 0x12: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->macl); return 1;
		case 0x13: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->gbr); return 2;
		case 0x16: cpu->macl = read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 1;
		case 0x17: cpu->gbr = read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 3;
		case 0x15: set_t(cpu, (int32_t) cpu->r[n] > 0); return 1;
		case 0x18: cpu->r[n] <<= 8; return 1;
		case 0x19: cpu->r[n] >>= 8; return 1;
		case 0x1b:
			{
				uint8_t value = read8(cpu, cpu->r[n]);
				set_t(cpu, value == 0);
				write8(cpu, cpu->r[n], value | 0x80);
				return 4;
			}
		case 0x1a: cpu->macl = cpu->r[n]; return 1;
		case 0x1e: cpu->gbr = cpu->r[n]; return 1;
		case 0x20: shift_one(cpu, n, true, false, false, false); return 1;
		case 0x21: shift_one(cpu, n, false, true, false, false); return 1;
		case 0x22: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->pr); return 1;
		case 0x23: cpu->r[n] -= 4; write32(cpu, cpu->r[n], cpu->vbr); return 2;
		case 0x24: shift_one(cpu, n, true, false, true, true); return 1;
		case 0x25: shift_one(cpu, n, false, false, true, true); return 1;
		case 0x26: cpu->pr = read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 1;
		case 0x27: cpu->vbr = read32(cpu, cpu->r[n]); cpu->r[n] += 4; return 3;
		case 0x28: cpu->r[n] <<= 16; return 1;
		case 0x29: cpu->r[n] >>= 16; return 1;
		case 0x2a: cpu->pr = cpu->r[n]; return 1;
		case 0x2b: delay(cpu, cpu->r[n]); return 2;
		case 0x0e: cpu->sr = cpu->r[n] & SH2_SR_MASK; cpu->interrupt_check = true; return 1;
		case 0x2e: cpu->vbr = cpu->r[n]; return 1;
		default: return illegal(cpu);
	}
}

static unsigned group6(struct karashi_sh2 *cpu, uint16_t opcode, unsigned n, unsigned m) {
	uint32_t value;

	switch (opcode & 15) {
		case 0: cpu->r[n] = (uint32_t) (int32_t) (int8_t) read8(cpu, cpu->r[m]); return 1;
		case 1: cpu->r[n] = (uint32_t) (int32_t) (int16_t) read16(cpu, cpu->r[m]); return 1;
		case 2: cpu->r[n] = read32(cpu, cpu->r[m]); return 1;
		case 3: cpu->r[n] = cpu->r[m]; return 1;
		case 4:
			value = (uint32_t) (int32_t) (int8_t) read8(cpu, cpu->r[m]);
			if (n != m) cpu->r[m]++;
			cpu->r[n] = value;
			return 1;
		case 5:
			value = (uint32_t) (int32_t) (int16_t) read16(cpu, cpu->r[m]);
			if (n != m) cpu->r[m] += 2;
			cpu->r[n] = value;
			return 1;
		case 6:
			value = read32(cpu, cpu->r[m]);
			if (n != m) cpu->r[m] += 4;
			cpu->r[n] = value;
			return 1;
		case 7: cpu->r[n] = ~cpu->r[m]; return 1;
		case 8: cpu->r[n] = (cpu->r[m] & 0xffff0000) | (cpu->r[m] & 0xff) << 8 | (cpu->r[m] >> 8 & 0xff); return 1;
		case 9: cpu->r[n] = cpu->r[m] << 16 | cpu->r[m] >> 16; return 1;
		case 10: cpu->r[n] = subtract_borrow(cpu, 0, cpu->r[m]); return 1;
		case 11: cpu->r[n] = 0u - cpu->r[m]; return 1;
		case 12: cpu->r[n] = cpu->r[m] & 0xff; return 1;
		case 13: cpu->r[n] = cpu->r[m] & 0xffff; return 1;
		case 14: cpu->r[n] = (uint32_t) (int32_t) (int8_t) cpu->r[m]; return 1;
		case 15: cpu->r[n] = (uint32_t) (int32_t) (int16_t) cpu->r[m]; return 1;
		default: return illegal(cpu);
	}
}

static unsigned group8(struct karashi_sh2 *cpu, uint16_t opcode, unsigned n) {
	int32_t displacement = sign8(opcode) * 2;

	switch (opcode >> 8) {
		case 0x88: set_t(cpu, cpu->r[0] == (uint32_t) sign8(opcode)); return 1;
		case 0x89: if (cpu->sr & SH2_T) cpu->pc += 2 + displacement; return cpu->sr & SH2_T ? 3 : 1;
		case 0x8b: if (!(cpu->sr & SH2_T)) cpu->pc += 2 + displacement; return cpu->sr & SH2_T ? 1 : 3;
		case 0x8d: if (cpu->sr & SH2_T) delay(cpu, cpu->pc + 2 + displacement); return cpu->sr & SH2_T ? 2 : 1;
		case 0x8f: if (!(cpu->sr & SH2_T)) delay(cpu, cpu->pc + 2 + displacement); return cpu->sr & SH2_T ? 1 : 2;
		default:
			n = (opcode >> 4) & 15;
			switch (opcode >> 8) {
				case 0x80: write8(cpu, cpu->r[n] + (opcode & 15), (uint8_t) cpu->r[0]); return 1;
				case 0x81: write16(cpu, cpu->r[n] + (opcode & 15) * 2, (uint16_t) cpu->r[0]); return 1;
				case 0x84: cpu->r[0] = (uint32_t) (int32_t) (int8_t) read8(cpu, cpu->r[n] + (opcode & 15)); return 1;
				case 0x85: cpu->r[0] = (uint32_t) (int32_t) (int16_t) read16(cpu, cpu->r[n] + (opcode & 15) * 2); return 1;
				default: break;
			}
			return illegal(cpu);
	}
}

static unsigned group12(struct karashi_sh2 *cpu, uint16_t opcode, uint32_t instruction_pc) {
	uint32_t displacement = opcode & 0xff;
	uint32_t address;
	uint8_t value;

	switch (opcode >> 8) {
		case 0xc0: write8(cpu, cpu->gbr + displacement, (uint8_t) cpu->r[0]); return 1;
		case 0xc1: write16(cpu, cpu->gbr + displacement * 2, (uint16_t) cpu->r[0]); return 1;
		case 0xc2: write32(cpu, cpu->gbr + displacement * 4, cpu->r[0]); return 1;
		case 0xc3: return take_exception(cpu, displacement, cpu->pc, 6);
		case 0xc4: cpu->r[0] = (uint32_t) (int32_t) (int8_t) read8(cpu, cpu->gbr + displacement); return 1;
		case 0xc5: cpu->r[0] = (uint32_t) (int32_t) (int16_t) read16(cpu, cpu->gbr + displacement * 2); return 1;
		case 0xc6: cpu->r[0] = read32(cpu, cpu->gbr + displacement * 4); return 1;
		case 0xc7: cpu->r[0] = ((instruction_pc + 4) & ~3u) + displacement * 4; return 1;
		case 0xc8: set_t(cpu, !(cpu->r[0] & displacement)); return 1;
		case 0xc9: cpu->r[0] &= displacement; return 1;
		case 0xca: cpu->r[0] ^= displacement; return 1;
		case 0xcb: cpu->r[0] |= displacement; return 1;
		case 0xcc: set_t(cpu, !(read8(cpu, cpu->gbr + cpu->r[0]) & displacement)); return 3;
		case 0xcd:
		case 0xce:
		case 0xcf:
			address = cpu->gbr + cpu->r[0];
			value = read8(cpu, address);
			if ((opcode >> 8) == 0xcd) value &= (uint8_t) displacement;
			else if ((opcode >> 8) == 0xce) value ^= (uint8_t) displacement;
			else value |= (uint8_t) displacement;
			write8(cpu, address, value);
			return 3;
		default: return illegal(cpu);
	}
}

void karashi_sh2_init(struct karashi_sh2 *cpu, const struct karashi_sh2_bus *bus) {
	memset(cpu, 0, sizeof(*cpu));
	if (bus) cpu->bus = *bus;
	karashi_sh2_reset(cpu);
}

void karashi_sh2_reset(struct karashi_sh2 *cpu) {
	struct karashi_sh2_bus bus = cpu->bus;

	memset(cpu, 0, sizeof(*cpu));
	cpu->bus = bus;
	cpu->sr = SH2_I;
	cpu->frt_enable = 0x01;
	cpu->frt_compare[0] = cpu->frt_compare[1] = 0xffff;
	cpu->frt_output = 0xe0;
	cpu->wdt_reset = 0x1f;
	cpu->serial[1] = 0xff;
	cpu->serial[3] = 0xff;
	cpu->serial[4] = 0x84;
	cpu->interrupt_check = true;
	karashi_sh2_forget_loops(cpu);
	cpu->pc = read32(cpu, 0);
	cpu->r[15] = read32(cpu, 4);
	if (cpu->address_error) cpu->fault = KARASHI_SH2_FAULT_ADDRESS;
	cpu->address_error = false;
}

bool karashi_sh2_interrupt_ready(const struct karashi_sh2 *cpu) {
	unsigned vector;
	return pending_interrupt(cpu, &vector) > ((cpu->sr & SH2_I) >> 4);
}

void karashi_sh2_wait(struct karashi_sh2 *cpu, uint64_t target) {
	if (target <= cpu->cycles) return;
	flush_timers(cpu);
	advance_timers(cpu, (unsigned) (target - cpu->cycles));
	cpu->timer_limit = timer_limit(cpu);
	cpu->interrupt_check = true;
	cpu->cycles = target;
}

void karashi_sh2_forget_loops(struct karashi_sh2 *cpu) {
	cpu->polling = false;
	cpu->poll_pc = 1;
	cpu->delay_loop_pc = 1;
	cpu->timer_limit = timer_limit(cpu);
}

static bool delay_loop(struct karashi_sh2 *cpu, uint32_t target, uint32_t branch_pc) {
	uint16_t branch = read16(cpu, branch_pc);
	unsigned reg = 16;
	unsigned cost = 0;
	uint32_t address;

	if ((branch >> 8) == 0x8b) cost = 3;
	else if ((branch >> 8) == 0x8f && read16(cpu, branch_pc + 2) == 0x0009) cost = 3;
	else return false;
	for (address = target; address < branch_pc; address += 2) {
		uint16_t opcode = read16(cpu, address);
		if (opcode == 0x0009) cost++;
		else if ((opcode & 0xf0ff) == 0x4010 && reg == 16) {
			reg = (opcode >> 8) & 15;
			cost++;
		} else return false;
	}
	if (reg == 16 || cpu->address_error) {
		cpu->address_error = false;
		return false;
	}
	cpu->delay_loop_pc = target;
	cpu->delay_loop_reg = (uint8_t) reg;
	cpu->delay_loop_cost = (uint8_t) cost;
	return true;
}

static void back_edge(struct karashi_sh2 *cpu, uint32_t target, uint32_t branch_pc) {
	if (cpu->poll_pc == target) {
		if (cpu->stores == cpu->poll_stores && cpu->sr == cpu->poll_sr
		    && !memcmp(cpu->r, cpu->poll_regs, sizeof(cpu->r))) {
			cpu->polling = true;
		} else if (cpu->delay_loop_pc == target || delay_loop(cpu, target, branch_pc)) {
			uint32_t remaining = cpu->r[cpu->delay_loop_reg];
			uint64_t room = cpu->budget > cpu->cycles ? (cpu->budget - cpu->cycles) / cpu->delay_loop_cost : 0;
			uint64_t skip = remaining > 1 ? remaining - 1 : 0;
			if (skip > room) skip = room;
			if (skip && !karashi_sh2_interrupt_ready(cpu)) {
				cpu->r[cpu->delay_loop_reg] -= (uint32_t) skip;
				karashi_sh2_wait(cpu, cpu->cycles + skip * cpu->delay_loop_cost);
			}
		}
	}
	cpu->poll_pc = target;
	cpu->poll_sr = cpu->sr;
	cpu->poll_stores = cpu->stores;
	memcpy(cpu->poll_regs, cpu->r, sizeof(cpu->r));
}

void karashi_sh2_skip_idle(struct karashi_sh2 *cpu, uint64_t target) {
	unsigned vector;
	uint64_t skipped;

	if (!cpu->idle) return;
	cpu->idle = false;
	if (target <= cpu->cycles || pending_interrupt(cpu, &vector) > ((cpu->sr & SH2_I) >> 4)) return;
	skipped = target - cpu->cycles;
	cpu->cycles = target;
	flush_timers(cpu);
	advance_timers(cpu, (unsigned) skipped);
	cpu->timer_limit = timer_limit(cpu);
	cpu->interrupt_check = true;
}

void karashi_sh2_set_irl(struct karashi_sh2 *cpu, unsigned level) {
	if (cpu->irl != (level & 15)) cpu->interrupt_check = true;
	cpu->irl = (uint8_t) (level & 15);
}

#if defined(__GNUC__)
#define FAST_PATH inline __attribute__((always_inline))
#else
#define FAST_PATH inline
#endif

static FAST_PATH unsigned execute(struct karashi_sh2 *cpu) {
	uint16_t opcode;
	uint32_t instruction_pc;
	uint32_t delayed_target = cpu->delay_target;
	bool finish_delay = cpu->delay_pending;
	unsigned n;
	unsigned m;
	unsigned cycles;
	unsigned vector;
	unsigned level;

	if (cpu->fault != KARASHI_SH2_FAULT_NONE) return 0;
	if (!finish_delay && cpu->interrupt_check) {
		level = pending_interrupt(cpu, &vector);
		cpu->interrupt_check = level > ((cpu->sr & SH2_I) >> 4);
		if (cpu->interrupt_check) {
			cycles = take_exception(cpu, vector, cpu->pc, 13);
			cpu->sr = (cpu->sr & ~SH2_I) | (level << 4);
			cpu->cycles += cycles;
			return cycles;
		}
	}
	if (cpu->stopped) {
		cpu->cycles += 4;
		cpu->timer_debt += 4;
		if (cpu->timer_debt >= cpu->timer_limit) flush_timers(cpu);
		return 4;
	}
	cpu->delay_pending = false;
	instruction_pc = cpu->pc;
	{
		const uint8_t *code = cpu->pc & 1 ? NULL : direct(cpu, cpu->pc);
		opcode = code ? (uint16_t) ((uint16_t) code[0] << 8 | code[1]) : read16(cpu, cpu->pc);
	}
	cpu->opcode = opcode;
	cpu->pc += 2;
	if (cpu->address_error) {
		cycles = take_exception(cpu, 9, cpu->pc, 5);
		cpu->cycles += cycles;
		return cycles;
	}
	if (finish_delay && changes_pc_in_delay_slot(opcode)) {
		cycles = take_exception(cpu, 6, delayed_target, 5);
		cpu->cycles += cycles;
		return cycles;
	}
	n = opcode >> 8 & 15;
	m = opcode >> 4 & 15;
	switch (opcode >> 12) {
		case 0: cycles = group0(cpu, opcode, n); break;
		case 1: write32(cpu, cpu->r[n] + (opcode & 15) * 4, cpu->r[m]); cycles = 1; break;
		case 2: cycles = group2(cpu, opcode, n, m); break;
		case 3: cycles = group3(cpu, opcode, n, m); break;
		case 4: cycles = group4(cpu, opcode, n); break;
		case 5: cpu->r[n] = read32(cpu, cpu->r[m] + (opcode & 15) * 4); cycles = 1; break;
		case 6: cycles = group6(cpu, opcode, n, m); break;
		case 7: cpu->r[n] += (uint32_t) sign8(opcode); cycles = 1; break;
		case 8: cycles = group8(cpu, opcode, n); break;
		case 9: cpu->r[n] = (uint32_t) (int32_t) (int16_t) read16(cpu, instruction_pc + 4 + (opcode & 0xff) * 2); cycles = 1; break;
		case 10:
			delay(cpu, instruction_pc + 4 + (uint32_t) (sign12(opcode) * 2));
			cpu->idle = opcode == 0xaffe && read16(cpu, cpu->pc) == 0x0009;
			cycles = 2;
			break;
		case 11: cpu->pr = instruction_pc + 4; delay(cpu, cpu->pr + (uint32_t) (sign12(opcode) * 2)); cycles = 2; break;
		case 12: cycles = group12(cpu, opcode, instruction_pc); break;
		case 13: cpu->r[n] = read32(cpu, ((instruction_pc + 4) & ~3u) + (opcode & 0xff) * 4); cycles = 1; break;
		case 14: cpu->r[n] = (uint32_t) sign8(opcode); cycles = 1; break;
		default: cycles = illegal(cpu); break;
	}
	if (cpu->fault == KARASHI_SH2_FAULT_ILLEGAL) {
		cpu->fault = KARASHI_SH2_FAULT_NONE;
		cycles = take_exception(cpu, finish_delay ? 6 : 4, finish_delay ? delayed_target : instruction_pc, 5);
		finish_delay = false;
	} else if (cpu->address_error) {
		cycles = take_exception(cpu, 9, finish_delay ? delayed_target : cpu->pc, 5);
		finish_delay = false;
	}
	if (finish_delay) cpu->pc = delayed_target;
	cpu->cycles += cycles;
	cpu->timer_debt += cycles;
	if (cpu->timer_debt >= cpu->timer_limit) flush_timers(cpu);
	if (cpu->pc < instruction_pc && instruction_pc - cpu->pc <= 48 && cpu->fault == KARASHI_SH2_FAULT_NONE) {
		back_edge(cpu, cpu->pc, finish_delay ? instruction_pc - 2 : instruction_pc);
	}
	return cycles;
}

bool karashi_sh2_save(const struct karashi_sh2 *cpu, struct karashi_state_writer *writer) {
	unsigned index;

	for (index = 0; index < 16; index++) karashi_state_write_u32(writer, cpu->r[index]);
	karashi_state_write_u32(writer, cpu->pc);
	karashi_state_write_u32(writer, cpu->pr);
	karashi_state_write_u32(writer, cpu->gbr);
	karashi_state_write_u32(writer, cpu->vbr);
	karashi_state_write_u32(writer, cpu->mach);
	karashi_state_write_u32(writer, cpu->macl);
	karashi_state_write_u32(writer, cpu->sr);
	karashi_state_write_u32(writer, cpu->delay_target);
	karashi_state_write_u64(writer, cpu->cycles);
	karashi_state_write_u8(writer, (uint8_t) cpu->fault);
	karashi_state_write_bool(writer, cpu->delay_pending);
	karashi_state_write_bool(writer, cpu->stopped);
	karashi_state_write_u8(writer, cpu->irl);
	karashi_state_write_u32(writer, cpu->divisor);
	karashi_state_write_u32(writer, cpu->dividend_high);
	karashi_state_write_u32(writer, cpu->dividend_low);
	karashi_state_write_u32(writer, cpu->divide_vector);
	karashi_state_write_u16(writer, cpu->divide_control);
	for (index = 0; index < 2; index++) {
		karashi_state_write_u32(writer, cpu->dma[index].source);
		karashi_state_write_u32(writer, cpu->dma[index].destination);
		karashi_state_write_u32(writer, cpu->dma[index].count);
		karashi_state_write_u32(writer, cpu->dma[index].control);
		karashi_state_write_u32(writer, cpu->dma[index].vector);
		karashi_state_write_u8(writer, cpu->dma[index].request);
	}
	karashi_state_write_u16(writer, cpu->dma_operation);
	karashi_state_write_u16(writer, cpu->frt_counter);
	karashi_state_write_u16(writer, cpu->frt_compare[0]);
	karashi_state_write_u16(writer, cpu->frt_compare[1]);
	karashi_state_write_u8(writer, cpu->frt_enable);
	karashi_state_write_u8(writer, cpu->frt_status);
	karashi_state_write_u8(writer, cpu->frt_control);
	karashi_state_write_u8(writer, cpu->frt_output);
	karashi_state_write_u8(writer, cpu->frt_temp);
	karashi_state_write_u32(writer, cpu->frt_clock);
	karashi_state_write_u8(writer, cpu->wdt_control);
	karashi_state_write_u8(writer, cpu->wdt_counter);
	karashi_state_write_u8(writer, cpu->wdt_reset);
	karashi_state_write_u32(writer, cpu->wdt_clock);
	karashi_state_write_u16(writer, cpu->interrupt_control);
	karashi_state_write_u16(writer, cpu->priority_a);
	karashi_state_write_u16(writer, cpu->priority_b);
	karashi_state_write_u16(writer, cpu->vector_a);
	karashi_state_write_u16(writer, cpu->vector_b);
	karashi_state_write_u16(writer, cpu->vector_c);
	karashi_state_write_u16(writer, cpu->vector_d);
	karashi_state_write_u16(writer, cpu->vector_wdt);
	karashi_state_write_u8(writer, cpu->cache_control);
	karashi_state_write_bytes(writer, cpu->serial, sizeof(cpu->serial));
	karashi_state_write_u8(writer, cpu->serial_shift);
	karashi_state_write_bool(writer, cpu->serial_busy);
	karashi_state_write_u32(writer, cpu->serial_countdown);
	karashi_state_write_u32(writer, cpu->timer_debt);
	for (index = 0; index < 8; index++) karashi_state_write_u16(writer, cpu->bus_control[index]);
	return karashi_state_write_bytes(writer, cpu->cache_ram, sizeof(cpu->cache_ram));
}

bool karashi_sh2_load(struct karashi_sh2 *cpu, struct karashi_state_reader *reader) {
	unsigned index;
	uint8_t fault;
	bool loaded = true;

	for (index = 0; index < 16; index++) loaded = karashi_state_read_u32(reader, &cpu->r[index]) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->pc) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->pr) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->gbr) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->vbr) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->mach) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->macl) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->sr) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->delay_target) && loaded;
	loaded = karashi_state_read_u64(reader, &cpu->cycles) && loaded;
	loaded = karashi_state_read_u8(reader, &fault) && loaded;
	loaded = karashi_state_read_bool(reader, &cpu->delay_pending) && loaded;
	loaded = karashi_state_read_bool(reader, &cpu->stopped) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->irl) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->divisor) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->dividend_high) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->dividend_low) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->divide_vector) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->divide_control) && loaded;
	for (index = 0; index < 2; index++) {
		loaded = karashi_state_read_u32(reader, &cpu->dma[index].source) && loaded;
		loaded = karashi_state_read_u32(reader, &cpu->dma[index].destination) && loaded;
		loaded = karashi_state_read_u32(reader, &cpu->dma[index].count) && loaded;
		loaded = karashi_state_read_u32(reader, &cpu->dma[index].control) && loaded;
		loaded = karashi_state_read_u32(reader, &cpu->dma[index].vector) && loaded;
		loaded = karashi_state_read_u8(reader, &cpu->dma[index].request) && loaded;
	}
	loaded = karashi_state_read_u16(reader, &cpu->dma_operation) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->frt_counter) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->frt_compare[0]) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->frt_compare[1]) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->frt_enable) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->frt_status) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->frt_control) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->frt_output) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->frt_temp) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->frt_clock) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->wdt_control) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->wdt_counter) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->wdt_reset) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->wdt_clock) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->interrupt_control) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->priority_a) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->priority_b) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->vector_a) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->vector_b) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->vector_c) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->vector_d) && loaded;
	loaded = karashi_state_read_u16(reader, &cpu->vector_wdt) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->cache_control) && loaded;
	loaded = karashi_state_read_bytes(reader, cpu->serial, sizeof(cpu->serial)) && loaded;
	loaded = karashi_state_read_u8(reader, &cpu->serial_shift) && loaded;
	loaded = karashi_state_read_bool(reader, &cpu->serial_busy) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->serial_countdown) && loaded;
	loaded = karashi_state_read_u32(reader, &cpu->timer_debt) && loaded;
	for (index = 0; index < 8; index++) loaded = karashi_state_read_u16(reader, &cpu->bus_control[index]) && loaded;
	loaded = karashi_state_read_bytes(reader, cpu->cache_ram, sizeof(cpu->cache_ram)) && loaded;
	if (!loaded || fault > KARASHI_SH2_FAULT_ILLEGAL) return false;
	cpu->sr &= SH2_SR_MASK;
	cpu->irl &= 15;
	cpu->frt_control &= 0x83;
	cpu->wdt_control &= 0xe7;
	cpu->fault = (enum karashi_sh2_fault) fault;
	cpu->address_error = false;
	karashi_sh2_forget_loops(cpu);
	cpu->interrupt_check = true;
	for (index = 0; index < 2; index++) cpu->dma[index].count &= 0xffffff;
	return true;
}

unsigned karashi_sh2_step(struct karashi_sh2 *cpu) {
	return execute(cpu);
}

void karashi_sh2_run(struct karashi_sh2 *cpu, uint64_t target) {
	while (cpu->cycles < target) {
		if (!execute(cpu) || cpu->idle || cpu->polling) return;
	}
}
