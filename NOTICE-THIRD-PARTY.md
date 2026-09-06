# Third-Party Notices

MX68K is a macOS/iOS port of the open-source Sharp X68000 emulator px68k. In
addition to px68k, MX68K incorporates source code ported from other
open-source X68000 emulators, listed below with their original copyright
notices and license terms. For the complete list of all third-party
components (px68k, C68K, fmgen, win32api, ZIPFoundation, and the XM6 port
below), see `ATTRIBUTION.md`; for the overall license structure, see
`LICENSE`.

---

## XM6 (version 2.06)

**Copyright (C) 2001-2006 ＰＩ．(ytanaka@ipc-tokai.or.jp)**

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
