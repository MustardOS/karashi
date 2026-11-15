#ifndef KARASHI_MARS_H
#define KARASHI_MARS_H

#include "pixel.h"
#include "sh2.h"
#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KARASHI_MARS_SDRAM_SIZE 0x40000
#define KARASHI_MARS_FRAME_SIZE 0x20000
#define KARASHI_MARS_WIDTH 320
#define KARASHI_MARS_FIFO_SIZE 8
#define KARASHI_MARS_PWM_CAPACITY 2048

struct karashi_mars;

struct karashi_mars_port {
	struct karashi_mars *mars;
	unsigned index;
	bool sleeping;
	uint64_t wake_stores;
	uint64_t wake_writes;
};

struct karashi_mars {
	struct karashi_sh2 cpu[2];
	struct karashi_mars_port port[2];
	const uint8_t *rom;
	size_t rom_size;
	size_t rom_capacity;
	const uint8_t *boot_68k;
	const uint8_t *boot_sh2[2];
	size_t boot_sh2_size[2];
	uint8_t sdram[KARASHI_MARS_SDRAM_SIZE];
	uint8_t frame[2][KARASHI_MARS_FRAME_SIZE];
	uint16_t palette[256];
	karashi_pixel colours[256];
	uint16_t comm[8];
	uint16_t fifo[KARASHI_MARS_FIFO_SIZE];
	uint16_t adapter;
	uint16_t interrupt_control;
	uint16_t bank;
	uint16_t dreq_control;
	uint32_t hint_vector;
	uint32_t dreq_source;
	uint32_t dreq_destination;
	uint16_t dreq_length;
	uint16_t sega_tv;
	uint16_t interrupt_mask[2];
	uint8_t pending[2];
	uint16_t hcount;
	uint16_t hint_counter;
	uint16_t bitmap_mode;
	uint16_t shift;
	uint16_t fill_length;
	uint16_t fill_address;
	uint16_t fill_data;
	uint16_t pwm_control;
	uint16_t pwm_cycle;
	uint16_t pwm_fifo[2][3];
	uint16_t pwm_width[2];
	uint8_t pwm_count[2];
	uint8_t pwm_timer;
	uint8_t fifo_read;
	uint8_t fifo_count;
	uint8_t display;
	uint8_t select;
	bool swap_pending;
	bool pal;
	bool running;
	bool in_pwm;
	uint16_t line;
	uint16_t height;
	uint64_t line_start;
	uint64_t clock;
	uint64_t pwm_time;
	uint64_t writes;
	unsigned pwm_frames;
	int16_t pwm_audio[KARASHI_MARS_PWM_CAPACITY * 2];
};

void karashi_mars_init(struct karashi_mars *mars, const uint8_t *rom, size_t rom_size);
void karashi_mars_set_boot(
	struct karashi_mars *mars, const uint8_t *boot_68k, const uint8_t *master, size_t master_size,
	const uint8_t *slave, size_t slave_size
);
void karashi_mars_set_rom_capacity(struct karashi_mars *mars, size_t capacity);
void karashi_mars_rebind(struct karashi_mars *mars);
void karashi_mars_reset(struct karashi_mars *mars, bool pal);
bool karashi_mars_owns(const struct karashi_mars *mars, uint32_t address);
uint8_t karashi_mars_read8(struct karashi_mars *mars, uint32_t address);
uint16_t karashi_mars_read16(struct karashi_mars *mars, uint32_t address);
void karashi_mars_write8(struct karashi_mars *mars, uint32_t address, uint8_t value);
void karashi_mars_write16(struct karashi_mars *mars, uint32_t address, uint16_t value);
void karashi_mars_run(struct karashi_mars *mars, uint64_t target);
void karashi_mars_begin_line(struct karashi_mars *mars, unsigned line, unsigned height, uint64_t line_start);
void karashi_mars_end_active(struct karashi_mars *mars);
void karashi_mars_end_frame(struct karashi_mars *mars);
unsigned karashi_mars_height(const struct karashi_mars *mars);
void karashi_mars_render_line(
	const struct karashi_mars *mars, unsigned line, karashi_pixel *output, const uint8_t *backdrop
);
void karashi_mars_mix_audio(struct karashi_mars *mars, int16_t *audio, unsigned frames);
bool karashi_mars_save(const struct karashi_mars *mars, struct karashi_state_writer *writer);
bool karashi_mars_load(struct karashi_mars *mars, struct karashi_state_reader *reader);

#endif
