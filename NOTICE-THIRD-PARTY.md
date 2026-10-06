# Third-Party Notices

MX68K is a macOS/iOS port of the open-source Sharp X68000 emulator px68k. In
addition to px68k, MX68K incorporates source code ported from other
open-source X68000 emulators, listed below with their original copyright
notices and license terms. For the complete list of all third-party
components (px68k, C68K, fmgen, win32api, mt32emu, Nuked-SC55, Musashi,
SoftFloat 2a / Motorola FPSP, ZIPFoundation, and the XM6 port below), see `ATTRIBUTION.md`; for the overall license structure, see
`LICENSE`.

---

## XM6 (version 2.06)

**Copyright (C) 2001-2006 ＰＩ．(Twitter: @xm6_original)**

The following MX68K source files are ported from the `vm/` directory of the
XM6 source distribution (X68000 EMULATOR "XM6" version 2.06):

| MX68K file | Ported from (XM6) |
|------------|-------------------|
| `Bridge/scsi_disk.cpp` | `vm/disk.cpp` (SASI/SCSI hard-disk, MO and CD-ROM classes; CD-DA excluded) |
| `Bridge/scsi_disk.h`   | `vm/disk.h` (SASI/SCSI hard-disk, MO and CD-ROM classes; CD-DA excluded) |
| `Bridge/scsi_spc.cpp`  | `vm/scsi.cpp` (MB89352 SPC + phase machine; HDD / MO / CD-ROM paths, CD-DA excluded) |
| `Bridge/scsi_spc.h`    | `vm/scsi.h` (MB89352 SPC + phase machine; HDD / MO / CD-ROM paths, CD-DA excluded) |

The `disk` port covers the hard-disk classes `DiskTrack`, `DiskCache`,
`Disk`, `SASIHD` and `SCSIHD`, the MO class `SCSIMO` (added in P668), and the
CD-ROM classes `CDTrack` and `SCSICD` (added in P676).

`CDDABuf` and the CD-DA playback path are **not** ported, because upstream XM6
itself leaves them unimplemented: the `CDDABuf` class body is sealed behind
`#if 0` in `vm/disk.h`, `SCSICD::PlayAudio` / `PlayAudioMSF` /
`PlayAudioTrack` only set `DISK_INVALIDCDB`, and `SCSICD::GetBuf` is an empty
function. `SCSICD::Load` and `SCSIMO::Load` are likewise not ported, matching
`SCSIHD` — MX68K's SCSI state save/load path is a stub.

The `scsi` port covers the `SCSI` (MB89352 "SPC") controller class: its
register machine, SCSI phase state machine, interrupt logic, and the
hard-disk / MO / CD-ROM command dispatch (which delegates to the ported
`Disk` / `SASIHD` / `SCSIHD` / `SCSIMO` / `SCSICD` classes above), including
the live media-access accessors (`Open` / `Eject` / `IsValid` / `IsReady` /
`IsLocked` / `GetPath`) for both MO and CD-ROM. Only the CD-DA path
(`PlayAudio10` / `PlayAudioMSF` / `PlayAudioTrack` and the `cdda` frame
event) is excluded, and `Save` / `Load` / `ApplyCfg` / `AssertDiag` are
minimal stubs.

In both ports, only Win32/XM6 environment types and macros were substituted;
the emulation logic is preserved unchanged.

`Bridge/scsi_compat_shim.h` is an original MX68K file that provides the
minimal type/macro shims required to compile the ported code; it also
replicates the `ASSERT` and `MAKEID` macro definitions from XM6 `vm/xm6.h`.

### XM6 usage terms (使用規定 / license)

Quoted from the XM6 version 2.06 source distribution (`XM6src.txt`):

> ・vmディレクトリ下のファイルを再利用する場合は、ドキュメントにオリジナルの
>   著作権表示を明記してください。また商用利用は禁止します。

English summary (non-authoritative translation): When reusing files under the
`vm` directory, the original copyright notice must be stated in the
documentation, and commercial use is prohibited.

This notice satisfies the copyright-attribution requirement above.

### License-change permission for the ported files

MX68K incorporates C68K (Copyright 2003-2004 Stephane Dallongeville, as
distributed with Yabause; MX68K vendors the copy in the
[kenyahiro fork of px68k](https://github.com/kenyahiro/px68k)'s
`m68000/c68k/`), which is licensed under GPL-2.0-or-later. GPL requires
that the corresponding source of the entire combined work be made
available under GPL-compatible terms, which is not compatible with the
XM6 `vm/` license's blanket commercial-use prohibition quoted above.

To resolve this, HackCat0916 (the MX68K author) contacted ＰＩ．, the original
author of XM6, and requested permission to use the four ported files listed
above under GPL-compatible terms. In September 2026, ＰＩ．granted this
permission: scsi.h, scsi.cpp, disk.h and disk.cpp may be treated as
GPL2-licensed, on the condition that this documentation record that the
license-change permission was granted by the original author. (A record of
this exchange is kept on file by the MX68K author and is not reproduced
verbatim here.)

**`Bridge/scsi_disk.cpp`, `Bridge/scsi_disk.h`, `Bridge/scsi_spc.cpp` and
`Bridge/scsi_spc.h` (ported from XM6 `vm/disk.cpp`, `vm/disk.h`,
`vm/scsi.cpp` and `vm/scsi.h` respectively) are licensed under GPL-2.0
per this permission.** The copyright-attribution requirement above is
already satisfied by GPL-2.0 §1's own notice-preservation requirement,
not an additional condition. The original XM6 `vm/` commercial-use
restriction quoted above no longer applies to these four files; it is
retained above for historical/attribution accuracy and continues to
describe XM6 itself.

### Additions

When source ported from other XM6 `vm/` files is added to MX68K, it must be
recorded in this file as well.

---

## mt32emu (munt, libmt32emu 2.8.3)

`ThirdParty/mt32emu/` is a vendored copy of the mt32emu library from munt
(<https://github.com/munt/munt>, commit
`6e7c01fba7e1d50c8fa705834889fd0eac136075`), used to emulate a Roland
MT-32 / CM-32L as MX68K's optional internal MIDI synthesizer. The upstream
sources are unmodified; `src/config.h` is written by MX68K. See
`ThirdParty/mt32emu/MX68K_VENDOR.txt` for the exact file selection.

**License:** GNU Lesser General Public License, version 2.1 or (at your
option) any later version. Full text: `ThirdParty/mt32emu/COPYING.LESSER.txt`
(identical copy in `LICENSES/LGPL-2.1.txt`). Authors are listed in
`ThirdParty/mt32emu/AUTHORS.txt` and in each source file header. The
complete corresponding source of this library is included in this
repository under `ThirdParty/mt32emu/`.

**No Roland ROM images are included.** The MT-32 / CM-32L Control and PCM
ROMs are copyrighted by Roland Corporation; users must provide their own
legally obtained copies.

### `src/sha1/sha1.cpp`, `src/sha1/sha1.h` — BSD-3-Clause

These two files are not LGPL; they carry the following license, reproduced
here in full as its conditions require:

```
Copyright (c) 2011, Micael Hildenborg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
   * Neither the name of Micael Hildenborg nor the
     names of its contributors may be used to endorse or promote products
     derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY Micael Hildenborg ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Micael Hildenborg BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## Nuked-SC55 (Roland SC-55 mk1 emulator)

`ThirdParty/nuked_sc55/` is a vendored copy of Nuked-SC55 by nukeykt
(<https://github.com/nukeykt/Nuked-SC55>), used to emulate a Roland SC-55
(mk1) as MX68K's optional internal MIDI synthesizer (macOS only). It is taken
from MPX68K (<https://github.com/YosAwed/MPX68K>, commit
`3e286a025bdde55b71109edfa3336e8c54e7ae28`, `X68000 Shared/SC55/`), which
modified the upstream sources for headless use (serialized host API
`X68SC55_*`, no SDL, no ROM file I/O); those modifications are marked at the
top of the affected files. MX68K uses the MPX68K-modified sources unmodified.
See `ThirdParty/nuked_sc55/MX68K_VENDOR.txt` for the exact file selection.

**Copyright:** Copyright (C) 2021, 2024 nukeykt.

**License:** GNU General Public License, version 2 or (at your option) any
later version. Full text: `ThirdParty/nuked_sc55/LICENSE`. The complete
corresponding source is included in this repository under
`ThirdParty/nuked_sc55/`.

**No Roland ROM images are included.** The SC-55 program and wave ROMs are
copyrighted by Roland Corporation; users must provide their own legally
obtained copies.

---

## Musashi (Motorola 680x0 emulator)

`ThirdParty/Musashi/` is a vendored copy of Musashi by Karl Stenerud
(<https://github.com/kstenerud/Musashi>, commit
`313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd`), integrated as an alternative CPU
core backend (`Bridge/mx_cpu_musashi.c`) that powers X68030 support. It is
used at run time when the X68030 machine is selected; the default CPU core
for X68000/X68000XVI remains C68K. The upstream files
are unmodified except for one configuration line in `m68kconf.h`
(`M68K_EMULATE_INT_ACK` turned on); `m68kops.c`/`m68kops.h` are generated from
the MIT-licensed `m68k_in.c` by the upstream `m68kmake` tool.

Not vendored: the upstream `softfloat/` directory (SoftFloat Release 2b),
`m68kfpu.c` and `m68kmmu.h`. In their place, `m68kmmu.h` and
`softfloat/milieu.h` in `ThirdParty/Musashi/` are minimal stubs written by
MX68K (each marked "MX68K作成のスタブ"), `softfloat/softfloat.h` is an MX68K
forwarding shim to `ThirdParty/softfloat_2a/softfloat.h`, and `m68kfpu.c` is
an MX68K-modified port of MAME's `m68kfpu.cpp` (see "MC68881/68882 FPU
emulation" below) — none of these is upstream Musashi code. See
`ThirdParty/Musashi/MX68K_VENDOR.txt` for the exact file selection and how to
re-vendor (`Scripts/vendor_musashi.sh`).

**Copyright:** Copyright © 1998-2001 Karl Stenerud.

**License:** MIT (from `ThirdParty/Musashi/readme.txt`):

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## MC68881/68882 FPU emulation

MX68K emulates MC68881/68882 FPU instructions for the X68030 (Musashi EC030
core). At this stage the FPU is enabled only with the environment variable
`MX68K_MUSASHI_FPU=1` (experimental); otherwise FPU instructions take the
line-1111 exception as before. The implementation has two parts: the
instruction-level emulation in `ThirdParty/Musashi/m68kfpu.c` (a modified
port of MAME's `m68kfpu.cpp`, below), and the arithmetic engine in
`ThirdParty/softfloat_2a/` (SoftFloat 2a from Hatari, further below), which
`m68kfpu.c` calls for all arithmetic and transcendental functions.

### MAME `m68kfpu.cpp` (Karl Stenerud, R. Belmont) — modified port

`ThirdParty/Musashi/m68kfpu.c` is a port of MAME's
`src/devices/cpu/m68000/m68kfpu.cpp` (<https://github.com/mamedev/mame>, commit
`4ae561d0019312c0af7a519c2d22f52968e029e1`), rewritten by MX68K in C for the
Musashi CPU structure and connected to `ThirdParty/softfloat_2a/` instead of
SoftFloat 3e. It is a **modified version**: the 68040-specific parts are
omitted, MAME's synthesized transcendental functions are replaced by calls to
`softfloat_fpsp.c`, FSAVE/FRESTORE frames follow the selected chip
(68881/68882), packed-decimal FMOVE.P is not supported, and the instruction
decoding inside `m68040_fpu_op0`/`op1` was written by MX68K (MAME's
`m68k_in.lst`, which carries no license notice, was not used as a source).
The file keeps MAME's `license:BSD-3-Clause` / `copyright-holders` lines and
the license text below at its top.

**Copyright:** Karl Stenerud, R. Belmont (MAME `m68kfpu.cpp`).

**License:** BSD-3-Clause (text from MAME `docs/legal/BSD-3-Clause` at the
pinned commit):

```
Copyright Karl Stenerud, R. Belmont

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### Arithmetic engine — SoftFloat 2a (Hatari)

`ThirdParty/softfloat_2a/` is a vendored copy of the SoftFloat code in Hatari
(<https://github.com/hatari/hatari>, commit
`b166d8c8319da5418490d556ad40706c0995b6f8`, directory `src/cpu/softfloat/`):
`softfloat.c`, `softfloat.h`, `softfloat-macros.h`, `softfloat-specialize.h`,
`softfloat_fpsp.c` and `softfloat_fpsp_tables.h`. Each file is checked against
a pinned SHA256 by `Scripts/vendor_softfloat_2a.sh`, which also fails if any
file contains the SoftFloat Release 2b indemnification wording. See
`ThirdParty/softfloat_2a/MX68K_VENDOR.txt` for the exact file selection.

### SoftFloat Release 2a (John R. Hauser), with QEMU modifications

`softfloat.c` and the three headers derive from SoftFloat Release 2a, as
modified by the QEMU project (which relicensed its copy back to 2a in 2015)
and specialized for the 68K (`SOFTFLOAT_68K`) by Previous/Hatari. MX68K
keeps their file headers unmodified; those headers state three licenses for
different portions: the SoftFloat-2a license, the BSD license (Copyright (c)
2006, Fabrice Bellard), and GPL-2.0-or-later.

**Copyright:** John R. Hauser (original release 2a); Fabrice Bellard (BSD
portions); QEMU contributors.

**License:** SoftFloat-2a / BSD / GPL-2.0-or-later (per portion, as stated in
each file). The SoftFloat-2a notice (also in `LICENSES/SoftFloat-2a.txt`):

```
This C source file is part of the SoftFloat IEC/IEEE Floating-point
Arithmetic Package, Release 2a.

Written by John R. Hauser.  This work was made possible in part by the
International Computer Science Institute, located at Suite 600, 1947 Center
Street, Berkeley, California 94704.  Funding was partially provided by the
National Science Foundation under grant MIP-9311980.  The original version
of this code was written as part of a project to build a fixed-point vector
processor in collaboration with the University of California at Berkeley,
overseen by Profs. Nelson Morgan and John Wawrzynek.  More information
is available through the Web page `http://HTTP.CS.Berkeley.EDU/~jhauser/
arithmetic/SoftFloat.html'.

THIS SOFTWARE IS DISTRIBUTED AS IS, FOR FREE.  Although reasonable effort
has been made to avoid it, THIS SOFTWARE MAY CONTAIN FAULTS THAT WILL AT
TIMES RESULT IN INCORRECT BEHAVIOR.  USE OF THIS SOFTWARE IS RESTRICTED TO
PERSONS AND ORGANIZATIONS WHO CAN AND WILL TAKE FULL RESPONSIBILITY FOR ANY
AND ALL LOSSES, COSTS, OR OTHER PROBLEMS ARISING FROM ITS USE.

Derivative works are acceptable, even for commercial purposes, so long as
(1) they include prominent notice that the work is derivative, and (2) they
include prominent notice akin to these four paragraphs for those parts of
this code that are retained.
```

The BSD (Fabrice Bellard) and GPL-2.0-or-later notices are reproduced in the
header of `ThirdParty/softfloat_2a/softfloat.c`; the GPL text is in
`LICENSES/GPL-2.0.txt`.

### Transcendental functions — Andreas Grabher (Previous), derived from the Motorola M68040 FPSP

`softfloat_fpsp.c` (FSIN/FCOS/FTAN/FATAN/FETOX/FLOGN etc.) and
`softfloat_fpsp_tables.h` (their constant tables) were written by Andreas
Grabher for Previous (NeXT Computer emulator, GPL-2.0-or-later as a whole)
and are carried by Hatari. Derivation path: **Motorola M68040 FPSP →
NetBSD `sys/arch/m68k/fpsp/` (1994) → Previous (Andreas Grabher) → Hatari →
MX68K.** The routines are C rewrites of the FPSP assembler routines, and the
tables transcribe FPSP constants. These are therefore **modified versions**
of the FPSP.

The upstream files carry no notice of their own and omit Motorola's notice.
The FPSP notice permits use, modification and distribution "so long as this
entire notice is retained without alteration in any modified and/or
redistributed versions, and that such modified versions are clearly
identified as such". MX68K therefore restores the notice: a comment block at
the top of both files identifies them as modified versions and reproduces
the notice in full (the rest of each file is unchanged from Hatari), and the
notice is also in `LICENSES/Motorola-M68040-FPSP.txt`. Source of the notice:
NetBSD `sys/arch/m68k/fpsp/copyright.s` (<https://github.com/NetBSD/src>,
commit `20826bd07b369c4debd94fc54206982473ee6f95`).

**Copyright:** M68040 Software Package Copyright (c) 1993, 1994 Motorola Inc.;
C implementation by Andreas Grabher.

**License (Motorola FPSP notice):**

```
MOTOROLA MICROPROCESSOR & MEMORY TECHNOLOGY GROUP
M68000 Hi-Performance Microprocessor Division
M68040 Software Package

M68040 Software Package Copyright (c) 1993, 1994 Motorola Inc.
All rights reserved.

THE SOFTWARE is provided on an "AS IS" basis and without warranty.
To the maximum extent permitted by applicable law,
MOTOROLA DISCLAIMS ALL WARRANTIES WHETHER EXPRESS OR IMPLIED,
INCLUDING IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A
PARTICULAR PURPOSE and any warranty against infringement with
regard to the SOFTWARE (INCLUDING ANY MODIFIED VERSIONS THEREOF)
and any accompanying written materials.

To the maximum extent permitted by applicable law,
IN NO EVENT SHALL MOTOROLA BE LIABLE FOR ANY DAMAGES WHATSOEVER
(INCLUDING WITHOUT LIMITATION, DAMAGES FOR LOSS OF BUSINESS
PROFITS, BUSINESS INTERRUPTION, LOSS OF BUSINESS INFORMATION, OR
OTHER PECUNIARY LOSS) ARISING OF THE USE OR INABILITY TO USE THE
SOFTWARE.  Motorola assumes no responsibility for the maintenance
and support of the SOFTWARE.

You are hereby granted a copyright license to use, modify, and
distribute the SOFTWARE so long as this entire notice is retained
without alteration in any modified and/or redistributed versions,
and that such modified versions are clearly identified as such.
No licenses are granted by implication, estoppel or otherwise
under any patents or trademarks of Motorola, Inc.
```

### Open questions (stated for transparency)

Two points could not be settled from primary sources and rest on the best
evidence available. (1) The SoftFloat-2a / BSD / GPL-2.0-or-later labelling of
Grabher's transcendental functions was added by QEMU when it imported them
(2018); no record of Andreas Grabher's own agreement to that labelling was
found — the files themselves carry only the sentence "Written by Andreas
Grabher for Previous", and Previous as a whole is GPL-2.0-or-later. (2) That
SoftFloat Release 2a (unlike 2b, which has an indemnification clause) is
compatible with the GPL relies on the QEMU project's own position — QEMU's
`LICENSE` describes its specially-licensed parts as GPL-2.0-compatible, and its
2015 relicensing of softfloat to 2a carries the Acked-by of 17 rights holders —
not on a statement from the FSF itself, which we could not locate. If you are a
rights holder and see a problem here, please open an issue.
