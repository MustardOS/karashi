<p align="left"><img src="assets/karashi-logo.svg" alt="Karashi" width="480"></p>

An independent Libretro core for SEGA 8-bit and 16-bit systems.

## Systems

- **SG-1000** runs most content it has been compared with.
- **SC-3000** has its documented 2 KiB RAM, keyboard matrix and joystick interface.
- **Master System** has been compared with a reference core.
- **Game Gear** has been compared with a reference core.
- **Mega Drive and Genesis** have been compared with a reference core.
- **Mega CD and Sega CD** are planned _after_ the Mega Drive is accurate.
- **32X** runs most content however it is not yet at full speed.

## Features

- A portable Z80 interpreter.
- TMS9918 video and SEGA VDP mode 4 with scanline timing.
- NTSC and PAL timing with a Region option that applies without restarting content.
- 192, 224 and 240 line pictures.
- Band-limited SN76489 audio with Game Gear stereo.
- SEGA, Codemasters, Korean and MSX-style cartridge mappers.
- Cartridge save RAM.
- Save states with a versioned format and a checksum.
- Two controllers with named buttons.
- The Light Phaser, paddle and Sports Pad on either Master System port.
- SC-3000 keyboard input through the documented Intel 8255 matrix.
- Pickles controller discovery, SRAM, autosave, save-state and run-ahead compatibility.
- A complete Motorola 68000 interpreter with documented cycle counts.
- The Mega Drive VDP with window, shadow and highlight, sprite limits and DMA.
- YM2612 FM sound mixed with the PSG.
- Three and six button Mega Drive controllers.
- Two SH-2 CPUs with their on-chip modules, the 32X frame buffers and PWM sound.

## BIOS

- Is required for most, if not all, content for general accuracy.

| System | Files |
|---|---|
| Mega Drive | `bios_MD.bin` |
| 32X | `bios_MD.bin`, `32X_G_BIOS.BIN`, `32X_M_BIOS.BIN`, `32X_S_BIOS.BIN` |
| Master System | `bios_U.sms`, `bios_E.sms` or `bios_J.sms` by region, or `bios.sms` |
| Game Gear | `bios.gg` |
| SG-1000 | _none_ |

- Game Gear cartridges that run in Master System mode boot without a BIOS.
- Some cartridges never pass a BIOS check similar to real consoles.

## Building

```sh
make -j2
make check
```

- `make check` runs the component tests and loads the core through the Libretro interface.
- `make tools` builds the capture, benchmark, probe and exerciser tools.
- `m68kvec` runs 68000 single instruction test vectors.
- `ramdiff` compares the system RAM of two cores frame by frame.
- `tools/compare.py` compares frames with a reference core.
- `tools/audio_compare.py` compares average samples with a reference core.
- `tools/hashes.py` checks that two builds give the same video and audio.
- `KARASHI_REFERENCE_SYSTEM_DIR` can give the reference process a separate system directory.

```sh
make BUILD_DIR=build-aarch64 CC=aarch64-buildroot-linux-gnu-gcc
```

- The default optimisation level is `-O3`.
- Pass other flags through the environment, as in `CFLAGS="-O2 -g" make`.
- `make CFLAGS=...` replaces the required `-fPIC`, `-fvisibility=hidden` and `-std=c11` flags.
- A profile-guided build is optional and takes three steps:

```sh
make BUILD_DIR=build-aarch64-pgo CC=aarch64-buildroot-linux-gnu-gcc PGO=generate PGO_DIR=/tmp/kpgo
# run the instrumented core on the device over representative content, then copy /tmp/kpgo back
make BUILD_DIR=build-aarch64-pgo CC=aarch64-buildroot-linux-gnu-gcc PGO=use PGO_DIR=$PWD/build-aarch64-pgo/pgo-data
```

- The generate and use builds must share the same `BUILD_DIR` path.
- Profiles go stale when the source changes, and GCC warns about each changed function.

## Documents

- [ATTRIBUTION.md](ATTRIBUTION.md) holds the upstream acknowledgements and licence findings.
- [SOURCES.md](SOURCES.md) holds the approved public specifications.
- [ROADMAP.md](ROADMAP.md) holds implementation status, validation gates and unresolved work.

