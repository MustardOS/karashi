# Attribution and Licensing

Karashi contains no copied implementation code from the projects below. They are acknowledged as important prior work and may be used for direct comparison and engineering analysis.

| Project | Contribution | Licence |
|---|---|---|
| PicoDrive by notaz and contributors | Fast Mega Drive, Mega-CD and 32X emulation on constrained hardware | Custom source-available, non-commercial redistribution terms |
| BlastEm by Michael Pavone and contributors | Accuracy-focused Mega Drive and Genesis emulation | GNU GPL version 3 |
| Genesis Plus GX by Charles MacDonald, Eke-Eke and contributors | Portable SEGA 8-bit and 16-bit emulation with broad peripheral support | Custom source-available, non-commercial redistribution terms, with separately licensed components |
| Genesis Plus GX Wide and contributors | Experimental widescreen behaviour based on Genesis Plus GX | Genesis Plus GX-derived licensing |
| Clown68000 by Clownacy | Portable MC68000 interpreter and exception-processing reference | GNU Affero General Public License version 3 |
| ClownZ80 by Clownacy | Portable Z80 interpreter and instruction-timing reference | GNU Affero General Public License version 3 |
| ClownMDEmu Core by Clownacy | Portable Mega Drive, Mega-CD, VDP and sound-system reference | GNU Affero General Public License version 3 |
| mdcomp by flamewing and contributors | Mega Drive compression-format implementations and development tooling | 0BSD for `src/asm`; GNU LGPL version 3 for the remainder |
| megadev by Damian Rogers | Mega Drive and Mega-CD development kit and technical documentation | MIT |
| emu2413 by Mitsutaka Okazaki and contributors | Portable YM2413 model used during the initial FM implementation | MIT |
| Nuked-OPLL by Nuke.YKT | Cycle-level YM2413 reference and independent instrument-ROM comparison | GNU GPL version 2 or later |
| Sandopolis by pixel-clover and contributors | Portable SEGA emulator, cartridge-memory comparison and property-test reference | MIT |

## Included Data

- `src/ym2413.c` contains the factual YM2413B instrument ROM bytes decoded from the published silicon image.
- The official `libretro.h` API header is distributed under the MIT licence by the Libretro and RetroArch contributors.
- Public Mega Drive test ROMs obtained through the Exodus technical documentation collection.

