# Attribution

MX68K builds on the work of many authors. This file records the provenance
of the third-party code vendored in this repository. This is a good-faith
summary by the maintainer — if you are an upstream author and believe a
component here is misattributed or mislicensed, please open an issue.

- **px68k** — `Core/px68k/`. By hissorii (<https://github.com/hissorii/px68k>),
  based on WinX68k by Kenjo (けろぴー). No explicit license is published
  upstream (only a liability disclaimer); included in good faith with full
  attribution.

- **C68K** (M68000 CPU core) — `Core/c68k/`. Copyright 2003–2004 Stephane
  Dallongeville (support headers additionally credit Theo Berkau and
  Guillaume Duhamel), as distributed with Yabause. MX68K vendors the
  [kenyahiro fork of px68k](https://github.com/kenyahiro/px68k)'s copy
  (`m68000/c68k/`) for Apple Silicon build support. **GPL-2.0-or-later** —
  full text in `LICENSES/GPL-2.0.txt`.

- **Debabelizer** (d68k disassembler) — `Core/px68k/x68k/d68k.c`, `d68k.h`.
  Copyright 1999 Karl Stenerud. Freeware; copyright notice must remain
  unaltered.

- **fmgen** (FM sound generator) — `Core/px68k/fmgen/`. Copyright (C) cisc
  1998, 2003. Distributed under cisc's terms (see source file headers):
  free to modify/redistribute/use with attribution, as free software, with
  modifications marked; commercial incorporation requires the author's
  prior agreement. Included in good faith.

- **win32api compatibility layer** — `Core/px68k/win32api/`. `dosio.c`,
  `dosio.h`, `fake.c` are Copyright (c) 2003 NONAKA Kimihiro (BSD-style,
  4-clause, with an advertising clause). `peace.c`, `peace.h` are Copyright
  2000 Masaru OKI (3-clause BSD).

- **XM6 SASI/SCSI/MO/CD-ROM port** — `Bridge/scsi_disk.cpp/h`,
  `scsi_spc.cpp/h`, ported from XM6 `vm/disk.cpp/h`, `vm/scsi.cpp/h`.
  Copyright (C) 2001-2006 ＰＩ．The original author granted MX68K
  permission to treat these four files as GPL2-licensed — see
  `NOTICE-THIRD-PARTY.md` for details.

- **MPX68K** — <https://github.com/YosAwed/MPX68K>, by GOROman, YosAwed and
  contributors. `Core/px68k/x68k/crtc_timing.c` and
  `Patches/Core/p671_crtc_rastercopy_compat_trigger.patch` port MPX68K code
  (merge `0d096dab8048e64f0e850f35c500e9e746929347`), with mechanical
  adaptations noted in-file.

- **px68k-libretro** — <https://github.com/libretro/px68k-libretro> (GPL-2.0).
  `Bridge/EmulatorBridge.c` includes a verbatim port from uraraworks'
  px68k-libretro fork PR#2 (merge `cd88e2ea…`), noted in-file.

- **ZIPFoundation** — <https://github.com/weichsel/ZIPFoundation>. MIT.
  Swift Package dependency, `MX68K-iOS` target only.

- **MX68K application layer** — `MX68K/`, `Bridge/` (excluding the XM6 port
  above). Copyright (C) 2026 HackCat0916 and contributors.
  GPL-2.0-or-later.
