# Third-Party Notices

MX68K is a macOS/iOS port of the open-source Sharp X68000 emulator px68k. In
addition to px68k, MX68K incorporates source code ported from other
open-source X68000 emulators, listed below with their original copyright
notices and license terms. For the complete list of all third-party
components (px68k, C68K, fmgen, win32api, mt32emu, Nuked-SC55, ZIPFoundation,
and the XM6 port below), see `ATTRIBUTION.md`; for the overall license structure, see
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
