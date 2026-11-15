#ifndef KARASHI_MD_H
#define KARASHI_MD_H

#include "m68k.h"
#include "mars.h"
#include "eeprom.h"
#include "mdvdp.h"
#include "sn76489.h"
#include "spi_eeprom.h"
#include "ym2612.h"
#include "z80.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KARASHI_MD_WORK_RAM_SIZE 65536
#define KARASHI_MD_Z80_RAM_SIZE 8192
#define KARASHI_MD_SRAM_SIZE 65536
#define KARASHI_MD_SAMPLE_RATE 48000
#define KARASHI_MD_AUDIO_CAPACITY 1024
#define KARASHI_MD_FM_CAPACITY 1200
#define KARASHI_MD_CONTROLLER_PORTS 4
#define KARASHI_MD_NTSC_CLOCK 53693175
#define KARASHI_MD_PAL_CLOCK 53203424
#define KARASHI_MD_FM_MCLK 1008
#define KARASHI_MD_MARS_SLICE 570

enum {
	KARASHI_MD_PAD_UP = 0x001,
	KARASHI_MD_PAD_DOWN = 0x002,
	KARASHI_MD_PAD_LEFT = 0x004,
	KARASHI_MD_PAD_RIGHT = 0x008,
	KARASHI_MD_PAD_B = 0x010,
	KARASHI_MD_PAD_C = 0x020,
	KARASHI_MD_PAD_A = 0x040,
	KARASHI_MD_PAD_START = 0x080,
	KARASHI_MD_PAD_Z = 0x100,
	KARASHI_MD_PAD_Y = 0x200,
	KARASHI_MD_PAD_X = 0x400,
	KARASHI_MD_PAD_MODE = 0x800
};

enum karashi_md_region {
	KARASHI_MD_REGION_AUTO = 0,
	KARASHI_MD_REGION_USA,
	KARASHI_MD_REGION_EUROPE,
	KARASHI_MD_REGION_JAPAN
};

enum karashi_md_pad {
	KARASHI_MD_PAD_NONE = 0,
	KARASHI_MD_PAD_THREE,
	KARASHI_MD_PAD_SIX
};

enum karashi_md_mapper {
	KARASHI_MD_MAPPER_STANDARD = 0,
	KARASHI_MD_MAPPER_SSF2,
	KARASHI_MD_MAPPER_SF004,
	KARASHI_MD_MAPPER_T5740
};

enum karashi_md_eeprom_mapping {
	KARASHI_MD_EEPROM_NONE = 0,
	KARASHI_MD_EEPROM_ACCLAIM,
	KARASHI_MD_EEPROM_SEGA,
	KARASHI_MD_EEPROM_EA,
	KARASHI_MD_EEPROM_ACCLAIM_WORD,
	KARASHI_MD_EEPROM_J_CART,
	KARASHI_MD_EEPROM_SPI
};

enum karashi_md_sram_mode {
	KARASHI_MD_SRAM_BOTH = 0,
	KARASHI_MD_SRAM_EVEN,
	KARASHI_MD_SRAM_ODD
};

struct karashi_md {
	struct karashi_m68k cpu;
	struct karashi_z80 z80;
	struct karashi_mdvdp vdp;
	struct karashi_sn76489 psg;
	struct karashi_ym2612 ym;
	struct karashi_eeprom eeprom;
	struct karashi_spi_eeprom spi_eeprom;
	const uint8_t *rom;
	size_t rom_size;
	uint8_t work_ram[KARASHI_MD_WORK_RAM_SIZE];
	uint8_t z80_ram[KARASHI_MD_Z80_RAM_SIZE];
	uint8_t sram[KARASHI_MD_SRAM_SIZE];
	uint16_t controller[KARASHI_MD_CONTROLLER_PORTS];
	uint8_t pad_type[KARASHI_MD_CONTROLLER_PORTS];
	uint8_t pad_phase[KARASHI_MD_CONTROLLER_PORTS];
	uint64_t pad_time[KARASHI_MD_CONTROLLER_PORTS];
	uint64_t pad_latency[KARASHI_MD_CONTROLLER_PORTS];
	uint8_t io_data[3];
	uint8_t io_control[3];
	uint8_t rom_bank[8];
	uint32_t sram_start;
	uint32_t sram_end;
	uint32_t sram_size;
	uint32_t eeprom_size;
	uint16_t eeprom_address_mask;
	uint16_t eeprom_page_mask;
	uint64_t line_start;
	uint64_t fm_clock;
	uint64_t psg_clock;
	uint32_t audio_clock;
	uint32_t master_clock;
	uint16_t z80_bank;
	unsigned audio_frames;
	unsigned fm_frames;
	unsigned width;
	unsigned height;
	uint8_t version;
	uint8_t region;
	uint8_t mapper;
	bool pal;
	bool sf004_linear;
	bool jcart_present;
	bool jcart_th;
	bool eeprom_present;
	bool eeprom_read_enabled;
	uint8_t eeprom_mapping;
	uint8_t eeprom_mode;
	uint8_t eeprom_address_bytes;
	uint8_t sram_mode;
	bool sram_present;
	bool sram_persistent;
	bool sram_enabled;
	bool sram_write_protected;
	bool z80_bus_requested;
	bool z80_reset_asserted;
	bool mars_present;
	bool tmss_mapped;
	const uint8_t *tmss;
	size_t tmss_size;
	uint8_t tmss_key[4];
	uint8_t backdrop[KARASHI_MD_WIDTH];
	struct karashi_mars mars;
	karashi_pixel video[KARASHI_MD_WIDTH * KARASHI_MD_MAX_HEIGHT];
	int16_t audio[KARASHI_MD_AUDIO_CAPACITY * 2];
	int16_t fm[KARASHI_MD_FM_CAPACITY * 2];
};

void karashi_md_init(struct karashi_md *machine, const uint8_t *rom, size_t rom_size);
void karashi_md_attach_mars(
	struct karashi_md *machine, const uint8_t *boot_68k, const uint8_t *master, size_t master_size,
	const uint8_t *slave, size_t slave_size
);
void karashi_md_set_tmss(struct karashi_md *machine, const uint8_t *tmss, size_t tmss_size);
void karashi_md_set_region(struct karashi_md *machine, enum karashi_md_region region);
void karashi_md_set_pad(struct karashi_md *machine, unsigned port, enum karashi_md_pad type);
void karashi_md_rebind(struct karashi_md *machine);
void karashi_md_reset(struct karashi_md *machine);
void karashi_md_set_controller(struct karashi_md *machine, unsigned port, uint16_t state);
void karashi_md_run_frame(struct karashi_md *machine);
double karashi_md_frame_rate(const struct karashi_md *machine);
bool karashi_md_save(const struct karashi_md *machine, struct karashi_state_writer *writer);
bool karashi_md_load(struct karashi_md *machine, struct karashi_state_reader *reader);

#endif
