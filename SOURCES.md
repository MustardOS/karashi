# Engineering Sources

## Processor Manuals

- Zilog, *Z80 CPU User Manual*, UM0080, September 2016: <https://www.zilog.com/docs/z80/z80cpu_um.pdf>
- NXP, *M68000 Family Programmer's Reference Manual*: <https://www.nxp.com/docs/en/reference-manual/M68000PRM.pdf>
- NXP, *M68000 User's Manual*: <https://www.nxp.com/docs/en/reference-manual/MC68000UM.pdf>
- Renesas, *SuperH RISC engine SH7604 Hardware Manual*, revision 4.00, September 2001: <https://www.renesas.com/en/document/mah/superh-risc-engine-sh7604>
- Renesas, *SH-1/SH-2/SH-DSP Software Manual*, revision 5.00, June 2004. It is distributed through the documentation pages for Renesas SH-2 products.
- Renesas, *SH-2E Software Manual*, revision 2.00, May 2006. Its SH-2-compatible MAC.L and MAC.W descriptions provide the 48-bit and 32-bit saturation rules.

## Sound

- Texas Instruments, *SN76489AN Digital Complex Sound Generator* data manual. A scanned copy is indexed through the public TI datasheet archives.
- Yamaha, *YM2608 (OPNA) Application Manual*. Its FM section covers the YM2612 register map, algorithms, detune steps, envelope rates and LFO values.
- Yamaha, *YM2413 FM Operator Type-LL Application Manual*, for the OPLL register and operator definitions.
- The published YM2413B die extraction and instrument-ROM decoding are the hardware evidence for Karashi's built-in melodic and rhythm instrument bytes: <https://siliconpr0n.org/archive/doku.php?id=vendor:yamaha:opl2#ym2413_instrument_rom>.
- YM2612 implementation status and unresolved work are tracked in root-level `ROADMAP.md`.

## Console Hardware

- B. Brown, *SEGA SC-3000 Programming Manual*, Nomac Publishing, 1984, chapter 1 pages 9-11 and chapter 5 pages 48-52: <https://www.sc3000-multicart.com/downloads/SegaSC3000ProgrammersManual.pdf>
- SEGA, *32X Hardware Manual*, for the 32X adapter, VDP, PWM, communication and interrupt registers.
- The 32X register layouts in Karashi were written from knowledge of that manual, not from a copy at hand.
- They need checking against a copy before production.

- SEGA service manuals and developer hardware references may be used as specifications where their provenance and revision are recorded.
- Community hardware notes may identify a question, but a behaviour must be confirmed against hardware, a public primary document or an independently authored test.

## Community Hardware Notes

- SMS Power development documents: <https://www.smspower.org/>
- Charles MacDonald, *Sega Master System VDP documentation* and *Sega Master System Technical Information*.
- Texas Instruments, *TMS9918A/TMS9928A/TMS9929A Video Display Processors* data manual.
- Behaviour taken from community notes remains unresolved in root-level `ROADMAP.md` until it is confirmed.

## API

- Libretro API header: <https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h>
- Vendored header SHA-256 on 2026-11-12: `5b6d8fe1056a39591ee6c479512eed4a6f6efd8f89f64f10cf8b4757a2aa48a0`.

## Testing Vectors

- SingleStepTests 68000 vectors: <https://github.com/SingleStepTests/m68000>
- They are black-box observations of single instructions.
- `tools/m68kvec.c` reads them in place.
- They are not stored in this repository.
- The vector set inspected on 2026-11-14 was revision `64b253116a3de04aaac4346c43680960dc9b67e5`.

## Reference Emulators

- PicoDrive: <https://github.com/notaz/picodrive>
- BlastEm: <https://github.com/libretro/blastem>
- Genesis Plus GX: <https://github.com/libretro/Genesis-Plus-GX>
- Genesis Plus GX Wide: <https://github.com/libretro/Genesis-Plus-GX-Wide>
- Clown68000: <https://github.com/Clownacy/clown68000>
- ClownZ80: <https://github.com/Clownacy/clownz80>
- ClownMDEmu Core: <https://github.com/Clownacy/clownmdemu-core>
- mdcomp: <https://github.com/flamewing/mdcomp>
- megadev: <https://github.com/drojaazu/megadev>
- Nuked-OPLL: <https://github.com/nukeykt/Nuked-OPLL>

## Other Notes

Master System region and timing analysis:
- Genesis Plus GX revision `2499cd3e3d1d8b7d0130d8b2825adb4cc608b2c0`, particularly `core/system.c`, `core/system.h`, `core/loadrom.c` and `core/cart_hw/sms_cart.c`, was used to cross-check the 262-line NTSC and 313-line PAL clocks and the known PAL-required cartridge CRCs.
- Karashi Auto mode uses real market-region filename tags: Europe-only, Australia-only and New Zealand-only content selects PAL, while USA, World, Brazil and explicitly multi-region 60 Hz content retains NTSC. The explicit Region option remains available for user override.
- Runtime option changes use the vendored Libretro `RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE` and `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` contracts so the frontend immediately updates frame pacing and audio cadence.

Source assisted Mega Drive VDP analysis:
- PicoDrive revision `ec68ce858330cb1f90ece789f2191d82b5bec833`, particularly `pico/videoport.c` and `pico/draw.c`.
- Genesis Plus GX revision `2499cd3e3d1d8b7d0130d8b2825adb4cc608b2c0`, particularly `core/vdp_ctrl.c` and `core/vdp_render.c`.
- The same Genesis Plus GX revision in `core/genesis.c`, `core/mem68k.c` and `core/sound/sound.c`, plus PicoDrive `pico/memory.c`, was used to compare Z80 bus ownership, open-bus reads and the 32-cycle YM2612 data-write busy interval.
- SEGA, *Genesis Software Manual*, revision 02/20/92, page 24, documents the four-word FIFO, active-scan limits, byte-wide VRAM access, word-wide CRAM/VSRAM access and maximum CPU wait.
- The comparison confirmed the FIFO access phase used by two independent implementations, H32/H40 access-rate differences, immediate visibility for ordinary memory-to-VDP DMA, per-line horizontal-scroll masking, four-bit fine horizontal scrolling and the H32/H40 partial-column vertical-scroll rule.
- Karashi's implementation and tests remain independently written. No upstream implementation code or comments were imported; hardware timing facts were re-expressed after cross-validation.

Mega Drive main-bus timing:
- [Kabuto’s hardware measurements](https://www.plutiedev.com/mirror/kabuto-hardware-notes) describe a two-clock delay per 128 main CPU clocks and contention from Z80 bank accesses. Karashi models refresh at instruction boundaries and accumulates fractional Z80 arbitration waits.
- PicoDrive’s `pico/memory.c` and `pico/pico_cmn.c` model bank contention as 3.25 Z80 clocks and eight 68000 clocks per access. The two cores give different 68000 throughput under sustained Z80 cartridge reads.
- Genesis Plus GX’s `core/m68k/m68kcpu.h`, `core/m68k/m68kcpu.c` and `core/memz80.c` were used to cross-check E-clock autovector alignment, refresh and Z80 bank waits against observed counter rates.

Source-assisted 32X performance analysis
- PicoDrive revision `ec68ce858330cb1f90ece789f2191d82b5bec833`, particularly `cpu/sh2/compiler.c`, `pico/32x/32x.c`, `pico/32x/memory.c`, `pico/32x/draw.c` and `pico/32x/pwm.c`.
- The comparison identified direct read maps, event-aware dual-SH-2 scheduling, polling wakeups and dynamic recompilation as its principal performance techniques.
- Karashi independently added read-only direct windows for each SH-2 boot ROM and the current writable frame buffer.
- The portable interpreter and 24-cycle synchronisation remain the reference path.

Source-assisted Mega Drive serial EEPROM analysis:
- Genesis Plus GX revision `2499cd3e3d1d8b7d0130d8b2825adb4cc608b2c0`, particularly `core/cart_hw/eeprom_i2c.c`;
- BlastEm revision `ec73825ece2a134beb7825a5fd6fd21a398ff12e`, particularly `rom.db`;
- Krikzz, *Serial EEPROM emulation*: <https://krikzz.com/pub/support/everdrive-md/v2/gen_eeprom.pdf>.
- T-5740 and SPI analysis used Genesis Plus GX revision `2499cd3e3d1d8b7d0130d8b2825adb4cc608b2c0`, particularly `core/cart_hw/md_cart.c` and `core/cart_hw/eeprom_spi.c`.
- Referenced the three switchable 512 KiB pages, the startup mirror at byte reads from `0x0181xx`, the SPI line-write register at `0xA13009` and data-read register at `0xA1300B`, the four signal bits and a 64 KiB 25xx/95xx device with 128-byte pages.
- The T-5740 start-up mirror is installed on the byte-read path; normal word and DMA reads retain the selected 512 KiB ROM-bank mapping.

J-Cart analysis:
- Genesis Plus GX revision, particularly `core/input_hw/gamepad.c` and `core/cart_hw/eeprom_i2c.c`.
- It identifies the two additional controller ports, their shared TH output, the combined word read, fixed-low D14 and content that used serial EEPROM hardware without J-Cart controllers.

Controller timing analysis:
- Genesis Plus GX revision, particularly `core/input_hw/gamepad.c`, to cross-check the measured 172-cycle delay between releasing a low TH output and observing the controller pullup.

Specific SRAM analysis:
- Genesis Plus GX revision, particularly `core/cart_hw/md_cart.c` and `core/cart_hw/sram.c`.
- Upper-2-MiB mirrored SRAM window.
- 1-MiB SRAM aperture embedded in common 4-MiB dumps.
- Save geometries and malformed save header.
- Volatile internal RAM declaration.

The Exodus Mega Drive test-ROM catalogue at <https://techdocs.exodusemulator.com/Console/SegaMegaDrive/Software.html> supplied the public validation inputs. 

Source assisted processor and MD integration analysis:
- Clown68000 revision `e404884e1533c601e4e7426e91627a2f2f2a618c`, particularly `source/interpreter/clown68000.c`;
- ClownZ80 revision `8be583511f94fbd8b7d5e884d5b8764562925351`, particularly `source/interpreter/interpreter.c` and `source/interpreter/interpreter.h`;
- ClownMDEmu Core revision `3a86302df010d82c89b689d6936eb2a8a79deb4f`, particularly `source/bus-main-m68k.c`, `source/bus-z80.c`, `source/sync.c`, `source/fm.c` and `source/psg.c`.

Clown68000 independently confirms the 14 byte group 0 frame sequence and that the fault address is retained before 24-bit bus masking.
- The source explicitly leaves function code and `IN` details incomplete and does not emulate instruction prefetch.
- SingleStepTests remains the behavioural evidence for those fields and the saved PC/IR.
- Synchronises the Z80 to the current master clock position before changing BUSREQ.
- Gates execution while BUSREQ or reset is active, and routes the bank window through the 68000 bus.

Future analysis:
- mdcomp revision `72c6df405a75d322c5b3722da46c3abb864d3793`
 - Tooling reference for common Mega Drive compression schemes.
- megadev revision `7a7246c14b845ad2f1bd3c7d73afb04cf67d83ef`
 - Mega CD development reference, particularly `docs/megacd_dev.md`, `docs/cdrom.md` and `docs/main_bios.md`.

