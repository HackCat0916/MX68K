#include <stdint.h>
#include "sasi.h"
#include "EmulatorBridge.h"

/* SASI I/Oポートのアドレスは 0xE96000 番台。
 * MemWriteTable のインデックス算出: (0xE96000 >> 13) & 0xFF == 0x4B
 * → MemWriteTable[0x4B] が SASI_Write スロット */
#define SASI_MEM_TABLE_IDX  0x4b

extern uint8_t (*MemReadTable[])(uint32_t);
extern void    (*MemWriteTable[])(uint32_t, uint8_t);

/* P450: メモリスイッチ書込に必要な Core シンボル。
 * scsi_spc_bridge.cpp の SRAM::SetMemSw が使っているのと同じ 2 本を、同じ
 * 入手経路(ヘッダを引かず extern 宣言)で取り込む。FASTCALL は
 * Core/px68k/common.h の空マクロなので sram.h の宣言と型は厳密一致する。 */
extern void SRAM_Write(uint32_t adr, uint8_t data);
extern void cpu_writemem24(uint32_t adr, uint32_t data);
/* P456: 書込み直後のリードバック用。SRAM_Write と同じ入手経路(ヘッダを引かず
 * extern 宣言)で取り込む。Core/px68k/x68k/sram.h:17 の宣言と型は厳密一致
 * (FASTCALL は Core/px68k/common.h の空マクロ)。
 * SRAM_Read(0x00ed005a) は sram.c:208-215 で addr&0xffff → 偶数 byte 枝
 * (= LE ワードの上位 = SRAM[0x5b])を返し、SRAM_Write(0x00ed005a) が
 * sram.c:246-249 の adr^=1 で書く SRAM[0x5b] と同じバイトを指す。 */
extern uint8_t SRAM_Read(uint32_t adr);

/* 非static: scsi_in_bridge.c から直接呼べるようにする(ロジック変更なし、
 * リンケージのみ変更。P247 Stage1 の内蔵SCSIパススルー用)。 */
void safe_SASI_Write(uint32_t addr, uint8_t data)
{
    /* アドレス下位3ビットが 7 以外 (SEL/デバイスセレクト以外) はそのまま通す */
    if ((addr & 0x07u) != 7u) {
        SASI_Write(addr, data);
        return;
    }
    /* data==0: デバイス未選択 → SASI_Init() で完全リセット (BusFree フェーズへ)
     * このままだと SASI_Device=0x7f のまま Config.HDImage[254] にOOBアクセスし
     * MALLOC guard page SIGBUS を引き起こす (P13 クラッシュ) */
    if (data == 0u) {
        debug_log("[P13-GUARD] SASI SEL data=0: no device, full reset\n");
        SASI_Init();
        return;
    }
    /* data != 0: uint8_t なのでビット位置は 0..7、SASI_Device は最大 7
     * → Config.HDImage[14..15] のみアクセス (in-bounds) */
    SASI_Write(addr, data);
}

/* P447 (A): read/write の対称化。
 *
 * P13 以来この関数は MemWriteTable[0x4b] だけを設定し、MemReadTable[0x4b] は
 * 「静的初期化子(Core/px68k/x68k/mem_wrap.c:66 の SASI_Read)のまま」であることに
 * 暗黙に依存していた。その前提は SCSI 機(machine_type==4)が導入されるまでは
 * 成立していたが、scsi_in_bridge_install() は SCSI 機で Read/Write の**両方**を
 * 差し替える一方、SASI 機では即 return する(復元処理を持たない)。
 * 結果として「SCSI 機で起動 → SASI 機へ切替 → ハードリセット」の順で、
 * 書き側だけが SASI へ戻り、読み側は SCSI 側ハンドラ(常時 0xFF)に取り残された。
 * $E96003(SASI ステータスポート)が恒久 0xFF となりフェーズ遷移が観測できず、
 * SASI HDD がプロセス再起動まで不可視になる。
 *
 * 対策はこの関数が read/write を同数だけ触ること — リセットのたびに読み側も
 * 明示的に SASI_Read へ戻す。SASI_Read は sasi.h:7 で宣言され、型は
 * MemReadTable[] の要素型 uint8_t (*)(uint32_t) と厳密一致する(FASTCALL は
 * Core/px68k/common.h:16 で空マクロ)。
 *
 * SCSI 機の挙動は不変: 呼び出し側(EmulatorBridge.c mx68k_reset_hard)は
 * sasi_bridge_install() の直後に scsi_in_bridge_install(g_machine_type) を呼び、
 * machine_type==4 の枝は Read/Write 両方を無条件に上書きする(両分岐とも)。 */
void sasi_bridge_install(void)
{
    MemReadTable[SASI_MEM_TABLE_IDX]  = SASI_Read;
    MemWriteTable[SASI_MEM_TABLE_IDX] = safe_SASI_Write;
    debug_log("[P13/P447] sasi_bridge: MemReadTable[0x4b] -> SASI_Read, "
              "MemWriteTable[0x4b] -> safe_SASI_Write\n");
}

/* P450: XM6 SASI::Reset()(/Volumes/WD 8TB HDD/Develop/xm6/vm/sasi.cpp:172-193)
 * のメモリスイッチクリア部分に相当。SCSI が一つも存在しないとき、SRAM の
 * $ED006F/$ED0070/$ED0071 を「SCSI なし」の既定値へ戻す。
 *
 * ★Core の SRAM_SetSCSIMode(0) と書き込みバイトは完全に同一
 * (Core/px68k/x68k/sram.c:69-71)だが、あえて呼ばず Bridge 内で 3 回書く。
 * 理由: SRAM_SetSCSIMode の mode 1/2 は $ED000C-0F(ROM 起動ハンドル)まで
 * 書き換えるため、将来この呼び出しの引数を取り違えると D-18(起動崩壊)を
 * 再発させる。引数を持たない専用関数にしておけば構造上到達不能になる。
 * ($E8E00D の 0x31/0x55 解錠・施錠イディオムは Core sram.c:39-40 と同一。) */
static void sasi_memsw_write(uint32_t offset, uint8_t data)
{
    cpu_writemem24(0x00e8e00du, 0x31);           /* allow SRAM access */
    SRAM_Write(0x00ed0000u + offset, data);
    cpu_writemem24(0x00e8e00du, 0x55);           /* block SRAM access */
}

/* memsw_enabled: 「メモリスイッチ自動更新」設定値
 * scsi_present : 内蔵・外付を問わず SCSI I/F が装備されているか
 *                (XM6 の sasi.scsi_type != 0 に相当。判定は呼び出し側=
 *                 EmulatorBridge.c の mx68k_reset_hard が行う — この関数は
 *                 渡された bool に従うだけでロジックを持たない)。
 *
 * ログは毎ハードリセットで無条件に 1 行出す(特定値でフィルタしない)。
 * 判定結果だけでなく判定に使った生の入力値を同一行へ載せるため、
 * 「クリアしなかった」理由が機種なのか設定 OFF なのか外付検出なのかを、
 * ログ行単体で一意に判別できる(自己反証可能性の要件)。 */
void sasi_bridge_apply_memsw(int machine_type, bool memsw_enabled, bool scsi_present)
{
    bool will_clear = (memsw_enabled && !scsi_present);
    debug_log("[P450-MEMSW] machine=%d memsw=%d scsi_present=%d -> clear $ED006F/70/71 = %s\n",
              machine_type, memsw_enabled ? 1 : 0, scsi_present ? 1 : 0,
              will_clear ? "YES" : "no");
    if (!will_clear) return;
    sasi_memsw_write(0x6f, 0x00);    /* $ED006F: SCSI フラグ = なし */
    sasi_memsw_write(0x70, 0x07);    /* $ED0070: 内蔵(bit3=0)+ 本体 ID=7 */
    sasi_memsw_write(0x71, 0x00);    /* $ED0071: SASI エミュレーションフラグ全 0 */
}

/* ======================================================================
 * P456 (D-33): SRAM $ED005A の自動同期。
 *
 * XM6 SASI::Reset()(/Volumes/WD 8TB HDD/Develop/xm6/vm/sasi.cpp:172-193)の
 *   if (sasi.memsw) {
 *       if (sasi.scsi_type < 2) SetMemSw(0x5a, sasi.sasi_drives);
 *       else                    SetMemSw(0x5a, 0x00);
 *   }
 * に相当する。
 *
 * ★$ED005A の意味は「台数」ではなく **LUN 込みドライブ index(0..15)に対する
 *   排他的上限値**である(ユーザーの実機 BIOS `IPLROM.DAT` 逆アセンブルで確定:
 *   f:0x10D34 `MOVE.B $ED005A,$0CB4` が唯一の参照点で、IOCS 側 `FF9A02` が
 *   `CMP.B $0CB4,D0` + `BCC` → `MOVEQ #-1,D0` で index >= 上限 を弾く。
 *   index = device*2 + LUN であることは `FF9A30 LSR.W #1,D0` → `BSET D0,D2`
 *   → `MOVE.B D2,$E96007` と Core/px68k/x68k/sasi.c:414-426 の対応から確定)。
 *   読まれるのはハードリセット直後の 1 回だけ(以後はワークエリア $0CB4 の複写)。
 *
 * ★sasi_bridge_apply_memsw() とは意図的に別関数にしてある —
 *   Docs/09 D-30 の「将来 Pxx のレビュー必須条件」(p450_scsi_present の
 *   OR 合成と memsw 適用ロジックへの不干渉)を構造的に守るため。
 *   述語も別: $ED005A は XM6 の scsi_type < 2(= SASI I/F 搭載か)であり、
 *   $ED006F/70/71 の scsi_type == 0 とは異なる。外付 CZ-6BS1 装着時
 *   (scsi_type==1)は $ED005A を**書かねばならない**ので、
 *   p450_scsi_present を流用してはならない(流用すると SASI 機 + 外付 SCSI で
 *   $ED005A=0 になり全 SASI が沈黙する)。
 *
 * 引数:
 *   machine_type    : 呼び出し側が渡す **配線確定機種** (g_wired_machine_type)。
 *                     ログには wired= として出す。
 *   memsw_enabled   : 「メモリスイッチ自動更新」設定値。
 *   sasi_if_present : SASI I/F 搭載か (呼び出し側が g_wired_machine_type != 4 で判定)。
 *                     この関数はロジックを持たず渡された bool に従うだけ。
 *   unit_mask       : SASI unit 0..7 の在席ビットマスク(bit u = unit u)。
 * ====================================================================== */
void sasi_bridge_apply_sasi_count(int machine_type, bool memsw_enabled,
                                  bool sasi_if_present, uint8_t unit_mask)
{
    /* msb = 最上位の在席 unit 番号。mask==0 のときは未定義なので -1 のままにし、
     * 下の式で count=0 に落とす(XM6 の「SASI I/F が無いので 0」と同値・同義)。 */
    int msb = -1;
    for (int u = 0; u < MX68K_SASI_UNIT_COUNT; u++) {
        if (unit_mask & (uint8_t)(1u << u)) msb = u;
    }

    /* ★算出式はこの 1 箇所に閉じ込める。
     *   案A(採用): count = (msb+1)*2   … margin = count - 最上位在席index = 2
     *   案B(対案): count = 2*msb + 1   … margin = 1(XM6 と無次元一致)
     * 案A を採る理由は **XM6 忠実性ではなく誤り時コストの非対称性**:
     *   上限が大きすぎた場合の害はほぼゼロ — IPL が存在しない LUN1 スロットを
     *   1 つ余分に叩き Core/px68k/x68k/sasi.c:254 が空パスエラーを返して終わる。
     *   上限が小さすぎた場合の害は D-33 そのもの — 該当ドライブが IOCS の
     *   BCC 判定で恒久的にアクセス不能になり、しかも症状が「起動デバイス無し」
     *   のような遠い形で出るため原因特定が極めて困難。
     * 案B へ切り替えるならこの 1 行を差し替えるだけでよい。
     * 上限: msb=7 → 16 (0x10)。XM6 vm/sasi.h:27 SASIMax=16 と一致し、
     * index 0..15 を全許可する正しい排他的上限。
     * ★Core の SRAM_SetSASIDrive() は使わない — drive>15 ガードでこの 16 が
     *   無言で捨てられ、かつ P450 が確立した Bridge ローカルの
     *   sasi_memsw_write() イディオムと二重系統になるため。 */
    int count = (msb < 0) ? 0 : (msb + 1) * 2;
    if (!sasi_if_present) count = 0;   /* XM6: scsi_type >= 2 → SetMemSw(0x5a, 0x00) */

    bool will_write = memsw_enabled;
    if (will_write) {
        sasi_memsw_write(0x5a, (uint8_t)count);
    }
    /* ★リードバックは書込みの有無によらず常に取る。
     * 書込んだ場合: readback != count なら実装欠陥(バイトスワップの向き誤り、
     *   または $E8E00D 解錠漏れ — SRAM_Write は SysPort[5]!=0x31 のとき
     *   無言で書込みを捨てるため、リードバック無しではこの失敗が観測不能)。
     * 書込まなかった場合: 現在の $ED005A の生値そのものが読める。 */
    uint8_t readback = SRAM_Read(0x00ed005au);

    /* 毎ハードリセットで無条件に 1 行。判定に使った生の入力値(wired/memsw/
     * sasi_if/mask)と中間値(msb)を派生値(count)と同一行に載せるため、
     * 「$ED005A が更新されなかった」理由が機種なのか設定 OFF なのか在席ゼロ
     * なのかを行単体で一意に判別できる(自己反証可能性の要件)。 */
    debug_log("[P456-SASICNT] machine=%d wired=%d memsw=%d sasi_if=%d mask=0x%02X "
              "msb=%d count=%d written=%s readback=0x%02X\n",
              mx68k_get_machine_type(), machine_type,
              memsw_enabled ? 1 : 0, sasi_if_present ? 1 : 0,
              (unsigned)unit_mask, msb, count,
              will_write ? "YES" : "no", (unsigned)readback);
}

/* ======================================================================
 * P508 (D-47 症状①): SRAM $ED000C-0F(ROM 起動ハンドル)を配線構成に同期。
 *
 * IPL-ROM の ROM 起動プローブは $ED000C の 4 バイトを起動ハンドルとして
 * そのまま jsr する設計で、**値の検証を一切行わず、バスエラーだけを唯一の
 * 正常離脱条件**としている(5 世代の IPL-ROM 全てで $FF02D8 周辺のバイト列が
 * 同一、P508 spec_inv §2)。MX はこの 4 バイトを一度も書いておらず、常に
 * IPL 既定シード値 $00FC0000(内蔵 SCSI 用)のままだったため、内蔵 SCSI ROM が
 * 実際にはロードされていない構成でも $FC0000 のミラー(実体は $FE0000-$FFFFFF、
 * Inside X68000 p.466 §2·2)を起動コードとして実行してしまい、A3 に載った
 * 非コード値($52011A29)経由の奇数番地 jsr → アドレスエラー → TRAP#14 パニック
 * (「エラーが発生しました。リセットしてください。」)になっていた。
 *
 * 3 値の一次情報源(記号表):
 *   MX_ROMBOOT_INSCSI $00FC0000 … Core/px68k/x68k/sram.c:83-86 (SRAM_SetSCSIMode
 *     case 2) + XEiJ SPC.java:50 (SPC_HANDLE_IN) + SCSIINROM.DAT[0x00-03] 実測。
 *   MX_ROMBOOT_EXSCSI $00EA0020 … 同 sram.c:74-77 (case 1) + XEiJ SPC.java:51
 *     (SPC_HANDLE_EX、SPC_HANDLE_EX + (ID<<2) の ID0) + SCSIEXROM.DAT[0x20-23] 実測。
 *   MX_ROMBOOT_NONE   $00EF0000 … 起動デバイス皆無時の番兵値。P209 のバスエラー
 *     合成窓(Core/px68k/x68k/mem_wrap.c:326-334 rm_buserr + Bridge/m68000_bridge.c:24625、
 *     $EA0000-$EFFFFF)へ確実に当てて IPL-ROM の正常離脱経路 $FF0306 へ誘導する。
 *     ★実機の非 SCSI 機 ROM 既定値 $00BFFFFC は**採らない** — あれは「実装 RAM の
 *     すぐ外」という *関係* で成立する設計であり、MX の可変 RAM 容量設定(12MB 構成で
 *     $ED0008=$00C00000)では $BFFFFC が実 RAM 内に入りバスエラーしなくなる。
 *     $00EF0000 は RAM 容量に非依存な固定 I/O 窓なので、実機が達成したい不変条件
 *     (バスエラーの確実性)を MX の自由度に対しても保てる(D-30/D-46 と同型の判断)。
 *     $EE0000-$EEFFFF(P413 診断窓)・$EA0000-$EA1FFF(D-28/P506 窓)のどちらとも重複しない。
 *
 * ★SRAM_SetSCSIMode() は引き続き一切呼ばない — 引数取り違えで D-18 を再発させ得る
 *   ため、P450/P456 が確立した「引数の意味が構造上取り違え不能な専用関数」方針を維持する
 *   (この方針は D-18 解決後も維持する。scsi_compat_shim.h / scsi_spc_bridge.cpp の
 *   該当コメントも同旨へ P508 で改訂済み)。
 * ====================================================================== */
#define MX_ROMBOOT_INSCSI  0x00fc0000u   /* 内蔵SCSI IPL 起動ハンドル(ID0) */
#define MX_ROMBOOT_EXSCSI  0x00ea0020u   /* 外付 CZ-6BS1 IPL 起動ハンドル(ID0) */
#define MX_ROMBOOT_NONE    0x00ef0000u   /* ROM 起動デバイス皆無時の番兵(必ずバスエラー) */

/* $ED000C-0F の 4 バイトを 1 組の $E8E00D 解錠/施錠で書く専用ヘルパ。
 * バイト順は Core の SRAM_SetSCSIMode(sram.c:74-77 / 83-86)と同一のビッグ
 * エンディアン(offset 0x0C = 最上位バイト)。SRAM_Write 側の LE 入替
 * (sram.c:245-249 の adr^=1)は Core 内で吸収されるので、こちらは guest から
 * 見えるバイト順のまま渡せばよい。 */
static void sasi_romboot_write(uint32_t handle)
{
    cpu_writemem24(0x00e8e00du, 0x31);           /* allow SRAM access */
    SRAM_Write(0x00ed000cu, (uint8_t)((handle >> 24) & 0xffu));
    SRAM_Write(0x00ed000du, (uint8_t)((handle >> 16) & 0xffu));
    SRAM_Write(0x00ed000eu, (uint8_t)((handle >>  8) & 0xffu));
    SRAM_Write(0x00ed000fu, (uint8_t)( handle        & 0xffu));
    cpu_writemem24(0x00e8e00du, 0x55);           /* block SRAM access */
}

/* 引数:
 *   wired_machine_type : 配線確定機種 (g_wired_machine_type)。4 == 内蔵 SCSI 機。
 *   ext_wired          : 外付 CZ-6BS1 の**配線確定**装着状態 (g_scsi_ext_board_wired)。
 *   in_rom_loaded      : 内蔵 SCSI IPL ROM が SCSI[] に実ロード済みか。
 *   ext_rom_loaded     : 外付 SCSI IPL ROM が SCSIIPL[] に実ロード済みか。
 * この関数はロジックの判定材料を自分で集めず、渡された値に従うだけ
 * (P450/P456 と同じ方針)。
 *
 * ログは毎ハードリセットで無条件に 1 行。判定に使った 4 つの生値を派生値
 * (書いた/書くはずだったハンドル値)と同一行に載せるため、分岐ミスが起きても
 * 生値から事後に判別できる(自己反証可能性の要件)。memsw=/written= まで
 * 併記するので、「$ED000C が更新されなかった」理由が設定 OFF なのか
 * 分岐なのかも行単体で一意に判別できる。 */
void sasi_bridge_apply_rom_boot_handle(int wired_machine_type, bool ext_wired,
                                       bool in_rom_loaded, bool ext_rom_loaded)
{
    /* 判定順序は上から先勝ち。内蔵 SCSI 機は外付と排他(g_scsi_ext_board_wired
     * 自体が machine != 4 を含むが、ここでも順序で明示する)。 */
    uint32_t handle;
    if (wired_machine_type == 4 && in_rom_loaded) {
        handle = MX_ROMBOOT_INSCSI;
    } else if (ext_wired && ext_rom_loaded) {
        handle = MX_ROMBOOT_EXSCSI;
    } else {
        handle = MX_ROMBOOT_NONE;
    }

    /* 「メモリスイッチ自動更新」OFF なら SRAM には一切触れない
     * (sasi_bridge_apply_memsw / SRAM::SetMemSw と同一方針)。 */
    bool will_write = mx68k_get_memsw_auto_update();
    if (will_write) {
        sasi_romboot_write(handle);
    }

    debug_log("[P508-ROMBOOT] wired_machine=%d ext_wired=%d in_rom_loaded=%d "
              "ext_rom_loaded=%d -> ED000C=0x%08x memsw=%d written=%s\n",
              wired_machine_type, ext_wired ? 1 : 0, in_rom_loaded ? 1 : 0,
              ext_rom_loaded ? 1 : 0, (unsigned)handle,
              will_write ? 1 : 0, will_write ? "YES" : "no");
}
