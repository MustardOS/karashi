# Roadmap

## M0: foundation

- [x] Project identity, licence and source-assisted development policy.
- [x] Libretro lifecycle and system detection.
- [x] Native unit-test target.
- [x] CI for native x86_64 tests and an AArch64 cross-build with architecture and Libretro export checks.
- [x] Deterministic scheduler.
- [x] Versioned state container.

## M1: SG-1000

- [x] Z80 base, CB and ED instruction implementation with native tests.
- [x] Z80 IX/IY, DDCB and FDCB prefix implementation.
- [x] ZEXDOC passes and ZEXALL passes all but `BIT n,(HL)`.
- [x] Initial TMS9918 display modes, sprites and interrupts.
- [x] Initial SN76489 tone and noise audio.
- [x] SG-1000 memory map and controllers.
- [x] Original diagnostic ROM boot through Libretro on RG35XX-H.
- [x] SG-1000 save states.
- [x] Comparison with a reference core on 181 SG-1000 games.
- [x] Initial `.sc` loading through the shared SG-1000 hardware path.
- [x] SG-1000 cartridge RAM layouts for the Taiwanese conversions.
  - [x] RAM at 0x2000 and 0xC000 for the 14 MSX conversions.
  - [x] DahJee and Jumbo logo stall.
- [x] SC-3000 keyboard and memory.
  - [x] Documented 2 KiB system RAM and 8255 keyboard matrix.
  - [x] Libretro keyboard polling and both joystick ports through matrix column 7.

## M2: Master System and Game Gear

- [x] Scanline timing for the 8-bit machines.
- [x] SEGA VDP mode 4 with line and frame interrupts.
- [x] SEGA and Codemasters cartridge paging with tests.
- [x] Master System region check, controllers and Pause.
- [x] Game Gear display, stereo and I/O.
- [x] Cartridge save RAM and the first save states.
- [x] Boot test with 50 Master System and 35 Game Gear games.
- [x] Screenshot comparison with a reference core on 552 Master System and 482 Game Gear games.
- [x] Audio comparison with a reference core.
  - [x] Phase-insensitive spectrum comparison over 20 Master System images.
  - [x] Resolve the eight early one-sided silence cases with a later capture.
- [x] California Games II selector timing and PAL identification.
- [x] PAL timing and the Region option.
  - [x] Real-region Auto selection for Europe-only, Australia-only and New Zealand-only content.
  - [x] Complete known PAL-required CRC coverage with explicitly verified NTSC revisions retained.
  - [x] Live core-option region changes with state-preserving clock updates and frontend AV reconfiguration.
- [x] 224 and 240 line display modes.
- [x] YM2413 FM sound.
  - [x] Instrument table confirmed against the published YM2413B die extraction.
- [x] Korean mappers and Game Gear EEPROM saves.
  - [x] Korean and MSX-style mappers.
  - [x] Sangokushi 3 board.
  - [x] Game Gear EEPROM.
- [x] Game Gear header selection of Master System mode.
- [x] Measured mode list for 14 Game Gear images.
- [x] Light gun, paddle and sports pad.
- [x] Band-limited PSG output.

## M3: Mega Drive and Genesis

- [x] 68000 documented instruction tests.
  - [x] Every MC68000 instruction with documented cycle counts.
  - [x] Illegal, privilege, trace, zero divide, CHK, TRAPV, TRAP and address error exceptions.
  - [x] 317,500 single-instruction test vectors with no wrong state, frame, timing or final-prefetch result.
  - [x] Full-set early-path scan across 1,725 Mega Drive and Genesis images.
  - [x] Prefetch queue and exact address error stack frames.
    - [x] Two-word instruction prefetch queue, discontinuity refill, saved state and self-modifying-code regression coverage.
    - [x] Fourteen-byte MC68000 frame layout, full logical fault address and vector-verified R/W and function-code encoding.
    - [x] Full external-vector comparison of supplied address-error registers and memory enabled in `m68kvec`.
    - [x] Fault-boundary guards for shared operand-read paths and MOVE destination flag ordering.
    - [x] Saved PC, instruction-register, status-word and partial micro-operation verification against all supplied external address-error vectors.
    - [x] Per-instruction final-prefetch verification across all supplied vectors, including STOP and control-flow discontinuities.
- [ ] Z80 bus ownership and memory map.
  - [x] Z80 RAM plus bus-request and reset register foundations.
  - [x] Z80 run in step with the 68000, banked cartridge reads, YM2612 and PSG ports.
  - [x] Open-bus bits on the bus-request register.
  - [x] Bank-window reads and writes routed through the complete 68000 memory map.
  - [x] Exact Z80-side PSG decode at 0x7F11.
  - [ ] Cycle-verified BUSREQ acknowledgement and release timing.
- [ ] VDP FIFO, DMA, planes, sprites and interrupts.
  - [x] VDP ports, VRAM, CRAM, VSRAM, initial H/V interrupts, two scrolling planes and linked sprites.
  - [x] Window, priorities, shadow and highlight, line and column scrolling, interlace mode 2.
  - [x] Sprite line limits, masking, collision and overflow flags.
  - [x] 68000, fill and copy DMA with the transfer time charged to the 68000.
  - [x] End-of-active-display VSRAM capture isolated from HBlank preparation for the following line.
  - [x] H32 and H40 partial-column vertical-scroll behaviour.
  - [x] State version 20 prevents pre-raster-fix states from silently restoring divergent emulated state.
  - [x] Screenshot comparison with a reference core on the established 1,722-game set.
  - [x] FIFO timing and DMA busy flag.
    - [x] Initial four-entry FIFO, VRAM versus CRAM/VSRAM access width, CPU back-pressure, status flags and saved state.
    - [x] Initial DMA busy lifetime separated from immediate memory visibility.
    - [x] Resolve the Joe Montana II Z80-bus/YM2612 initialisation regression before deployment.
- [ ] YM2612 and PSG mixing.
  - [x] PSG write and 48 kHz output foundation.
  - [x] Initial YM2612 operators, algorithms, envelopes, timers, DAC, LFO and stereo mixing.
  - [x] YM2612 data-write busy timing and saved deadline.
  - [x] YM2612 and PSG write-time synchronisation on the master-clock timeline.
- [x] Cartridge metadata, SRAM, EEPROM and production-set mappers.
  - [x] SRAM, common large-ROM banking and confirmed Acclaim serial EEPROM mapping.
  - [x] Acclaim combined-word and split-lane wiring, upper-ROM/EEPROM read selection and device-specific save sizes.
  - [x] Header-declared 24C01 EEPROM on 0x200001.
  - [x] Ten-frame CPU path completes on all 1,725 current base images.
  - [x] SF-004 cartridge mapping used by Star Odyssey.
  - [x] T-5740 512 KiB banking and 25xx/95xx SPI EEPROM used by Pier Solar.
  - [x] T-5740 byte-only start-up mirroring separated from word and DMA reads.
  - [x] Codemasters J-Cart two-controller expansion and four Libretro input ports.
  - [x] Product-specific mirrored SRAM windows for Barkley Shut Up and Jam 2 and Triple Play.
  - [x] Missing-header SRAM geometry for PGA Tour Golf, Winter Challenge and Buck Rogers.
  - [x] Invalid-header handling for Psy-O-Blade save RAM and Feng Kuang Tao Hua Yuan volatile RAM.
  - [x] Libretro save-memory size follows the detected serial device capacity.
  - [x] Uncommon Xin Qigai Wangzi SRAM window and invalid external-RAM address fallback.
  - [x] Sonic 3 & Knuckles combined-image FRAM and Sonic 2/Pugsy false-SRAM exclusions.
  - [x] 16-bit, even-byte and odd-byte SRAM lanes, volatile/battery metadata, physical save capacities and write protection.
  - [x] Headerless cartridges no longer invent fallback SRAM.
  - [x] Cartridge metadata is directly observable through `mdprobe`.
  - [x] Exodus Mega Drive test-ROM smoke suite completes without CPU exceptions.
- [x] Three-button and six-button controllers.
  - [x] Initial two-port three-button protocol.
  - [x] Initial two-port six-button protocol.
  - [x] Controller TH direction-switching latency and saved deadline.
- [x] Region from the cartridge header and PAL from European file names.
- [x] NTSC and PAL timing acceptance tests.
- [x] Serial EEPROM families beyond the Acclaim and header-declared mappings.
  - [x] Product-identified SEGA X24C01 boards with D1 clock and D0 data wiring.
  - [x] Product-identified EA X24C01 boards with D6 clock and D7 data wiring.
  - [x] Codemasters J-Cart I2C boards and 24Cxx devices through 8 KiB with one- and two-byte addresses.
  - [x] T-5740/Pier Solar 25xx/95xx SPI EEPROM board.
  - [x] Known Mega Drive serial-save product databases and all serial declarations in the 1,725-image production set audited with no uncovered board family.
- [ ] SVP coprocessor for Virtua Racing.

## M4: 32X expansion

- [x] Isolated documented SH-2 integer set, memory, division, saturating MAC, delay-slot, trap, illegal-exception and state foundation.
- [x] External interrupts, address-error exception entry and on-chip peripherals.
- [x] Master and slave SH-2 scheduling, interrupts and serial link.
- [x] SDRAM, frame buffers, palette and Mega Drive video composition.
- [x] PWM audio and deterministic save state.
- [x] `.32x` content through Libretro with the real BIOS.
- [x] Authored component test without SEGA firmware.
- [x] Read-only direct maps for SH-2 boot ROM and frame-buffer reads, with 60-image deterministic regression coverage.
- [ ] Full speed on the RG35XX-H.
  - [ ] Cached portable SH-2 block interpreter with write invalidation and instruction-equivalence tests.
  - [ ] Optional AArch64 SH-2 recompiler after the portable block path is stable.
  - [ ] Event-aware coarse scheduling that preserves the 60-image video and audio hashes.
- [ ] After Burner, WWF WrestleMania, Sangokushi IV, Metal Head, Pitfall and the Ecco demo.
- [ ] SH-2 cache and bus timing.
- [x] Required 32X BIOS.

## M5: compatibility and performance

- [x] Screenshot comparison and video and audio hash tools.
- [x] Tile row cache, colour cache, ROM block table and RGB565 output.
- [x] Pickles lifecycle, controller discovery, SRAM, save-state and deterministic run-ahead verification.
- [x] Z80 dispatch and line renderer profiling on the Cortex-A53.
- [x] Initial Mega Drive renderer benchmark and tile-segment optimisation on Cortex-A53.
- [x] Guarded 68000 idle-loop skipping with instruction-equivalence coverage.
- [x] Exact rational audio-resampling accumulator and bounded comparison workers.
- [x] RG35XX-H frame-time measurements.
- [ ] RG35XX-H power measurements when the running kernel exposes a usable power interface.
- [x] Cortex-A53 profiling before any NEON work.
- [x] Rewind through serialization and raw RAM cheats through standard libretro interfaces.
- [ ] Optional overscan and widescreen experiments isolated from the accuracy path.

### First production candidate acceptance

- [x] Complete native component and Libretro lifecycle suite.
- [x] ASan, UBSan and LeakSanitizer suite on the host.
- [x] AArch64 architecture and Libretro export verification.
- [x] Deterministic live-device state replay across 8-bit, Mega Drive, J-Cart and 32X paths.
- [x] State-version-18 controller-timing candidate deployed with a retained rollback build.
- [ ] Repeat the full 1,722-image Mega Drive comparison on the release candidate.
- [ ] Validate save creation, restart and reload interactively for parallel SRAM, I2C, SPI and 93C46 content.
- [ ] Complete physical-controller and audible-audio acceptance on the RG35XX-H.
- [ ] Keep 32X outside the first production manifest until its M4 speed and compatibility gates pass.
- [ ] Review the remaining known unsupported hardware and prototype failures before changing the core from pre-alpha.

## M6: remaining expansion hardware

- [ ] Mega-CD and Sega CD.
  - [ ] Use megadev revision `7a7246c14b845ad2f1bd3c7d73afb04cf67d83ef` as an MIT-licensed development-document reference for BIOS, gate-array, CD-ROM and subsystem bring-up.
- [x] Required BIOS for Master System, Game Gear and Mega Drive.
