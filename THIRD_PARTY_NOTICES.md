# Third-party notices

This project's own code is released under the MIT License (see `LICENSE`).
It includes third-party code that keeps its original licence and notices.

## Musashi — Motorola 680x0 emulation core

- Location: `MacintoshESPEmulator/components/musashi/`
- Author: Karl Stenerud
- Licence: MIT-style. The full notice is at the top of `m68k.h` and must be
  kept in all copies.
- Modified for this project (68030 PMMU instruction stub, native speed-up
  hooks, ESP32 memory placement, separate read paths, configuration).

## SoftFloat — IEEE floating-point library (Musashi FPU)

- Location: `MacintoshESPEmulator/components/musashi/softfloat/`
- Author: John R. Hauser (Release 2b), repackaged by the MAME project
- Licence: the SoftFloat licence. Derivative works, including commercial ones,
  are allowed provided the source notes that it is derivative and keeps the
  four licence paragraphs, which are retained at the top of `softfloat.h` and
  `softfloat.c`. The software is provided as is, without warranty.

## m68kmmu.h — PMMU code from the MAME project

- Location: `MacintoshESPEmulator/components/musashi/m68kmmu.h`
- Original author: R. Belmont; Copyright Nicola Salmoria and the MAME Team.
- The original header is retained and refers to mamedev.org for licensing.
  The upstream MAME project distributes its current version of this file
  under the BSD-3-Clause licence; check the MAME source for the exact terms.
- This copy was largely rewritten for this project into an instruction-only
  68030 PMMU stub (no address translation).

## Hardware references (no code copied)

- MAME (V8/Spice/Egret/Cuda machine behaviour), Mini vMac (screen-size patch
  precedent), Inside Macintosh / Guide to the Macintosh Family Hardware.

## Build-time Python libraries (not distributed here)

- `machfs` and `macresources` by Elliot Nunn, used by `tools/build_disk.py`.
  Install them yourself (e.g. `pip install machfs`).

## Not included and not licensed

Apple ROM images, Mac OS System software, disk images and third-party Mac
applications are copyrighted by their owners and are not part of this
repository. `ROM/` is git-ignored; supply your own legally obtained copies and
never publish firmware binaries that embed them.
