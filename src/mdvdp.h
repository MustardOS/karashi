#ifndef KARASHI_MDVDP_H
#define KARASHI_MDVDP_H

#include "pixel.h"
#include "state.h"

#include <stdbool.h>
#include <stdint.h>

#define KARASHI_MD_WIDTH 320
#define KARASHI_MD_HEIGHT 224
#define KARASHI_MD_MAX_HEIGHT 240
#define KARASHI_MD_VRAM_SIZE 65536
#define KARASHI_MD_CRAM_SIZE 64
#define KARASHI_MD_VSRAM_SIZE 40
#define KARASHI_MD_LINE_MCLK 3420
#define KARASHI_MD_HBLANK_MCLK 2640
#define KARASHI_MD_VINT_DELAY_MCLK 112

typedef uint16_t (*karashi_mdvdp_dma_read_t)(void *context, uint32_t address);

struct karashi_mdvdp {
	uint8_t vram[KARASHI_MD_VRAM_SIZE];
	uint16_t cram[KARASHI_MD_CRAM_SIZE];
	uint16_t vsram[KARASHI_MD_VSRAM_SIZE];
	uint8_t registers[32];
	karashi_pixel palette[KARASHI_MD_CRAM_SIZE * 3];
	uint32_t address;
	uint64_t fifo_finish[4];
	uint64_t dma_finish;
	uint64_t port_clock;
	uint16_t status;
	uint16_t line;
	uint16_t lines;
	uint16_t hv_latch;
	uint16_t last_data;
	uint8_t code;
	uint8_t fifo_head;
	uint8_t fifo_count;
	uint8_t hint_counter;
	bool control_pending;
	bool vint_pending;
	bool vint_flag;
	bool hint_pending;
	bool fill_pending;
	bool pal;
	bool odd_frame;
	bool hv_latched;
	bool dot_overflow;
	karashi_mdvdp_dma_read_t dma_read;
	void *dma_context;
};

void karashi_mdvdp_reset(struct karashi_mdvdp *vdp);
void karashi_mdvdp_set_pal(struct karashi_mdvdp *vdp, bool pal);
unsigned karashi_mdvdp_write_control(struct karashi_mdvdp *vdp, uint16_t value);
unsigned karashi_mdvdp_write_control_at(
	struct karashi_mdvdp *vdp, uint16_t value, unsigned line_mclk, uint64_t master_clock
);
void karashi_mdvdp_write_data(struct karashi_mdvdp *vdp, uint16_t value);
unsigned karashi_mdvdp_write_data_at(
	struct karashi_mdvdp *vdp, uint16_t value, unsigned line_mclk, uint64_t master_clock
);
uint16_t karashi_mdvdp_read_control(struct karashi_mdvdp *vdp);
uint16_t karashi_mdvdp_read_status(struct karashi_mdvdp *vdp, unsigned line_mclk);
uint16_t karashi_mdvdp_read_status_at(
	struct karashi_mdvdp *vdp, unsigned line_mclk, uint64_t master_clock
);
uint16_t karashi_mdvdp_read_data(struct karashi_mdvdp *vdp);
uint16_t karashi_mdvdp_hv_counter(const struct karashi_mdvdp *vdp);
uint16_t karashi_mdvdp_hv_counter_at(const struct karashi_mdvdp *vdp, unsigned line_mclk);
unsigned karashi_mdvdp_begin_line(struct karashi_mdvdp *vdp, unsigned line);
void karashi_mdvdp_end_active(struct karashi_mdvdp *vdp);
unsigned karashi_mdvdp_irq_level(const struct karashi_mdvdp *vdp);
void karashi_mdvdp_raise_vint(struct karashi_mdvdp *vdp);
void karashi_mdvdp_acknowledge(struct karashi_mdvdp *vdp, unsigned level);
unsigned karashi_mdvdp_width(const struct karashi_mdvdp *vdp);
unsigned karashi_mdvdp_height(const struct karashi_mdvdp *vdp);
void karashi_mdvdp_render_line(struct karashi_mdvdp *vdp, unsigned line, karashi_pixel *output);
void karashi_mdvdp_render_line_mask(
	struct karashi_mdvdp *vdp, unsigned line, karashi_pixel *output, uint8_t *backdrop
);
bool karashi_mdvdp_save(const struct karashi_mdvdp *vdp, struct karashi_state_writer *writer);
bool karashi_mdvdp_load(struct karashi_mdvdp *vdp, struct karashi_state_reader *reader);

#endif
