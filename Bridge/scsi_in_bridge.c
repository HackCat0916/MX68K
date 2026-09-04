#include <stdint.h>
#include <stdbool.h>
#include "sasi.h"
#include "prop.h"        /* P447: Config.HDImage[] (診断プローブの副次的分母) */
#include "EmulatorBridge.h"

/* $E96020 番台(内蔵SASI/SCSI共有I/O領域)。MemReadTable/WriteTableインデックス
 * 算出: (0xE96020>>13)&0xff == 0x4b (sasi_bridge.cと同一領域)。
 * P247 Stage1: 機種がSCSI搭載(machine_type==4)の場合のみこのインデックスを
 * 差し替え、差替先は現行SASIハンドラへの完全パススルーとした。
 * P251 Stage2c: 三重ゲート(機種SCSI ∧ SCSIINROMロード済み ∧ 配線トグルON)を
 * すべて満たした場合のみ、offset 0x20-0x3f(絶対$E96020-3F)だけ実SCSI(SPC)へ
 * 委譲する分配ハンドラを差し込む。offset 0x00-0x1fは常にSASIパススルーのまま
 * 一切変えない。 */
#define SCSI_IN_MEM_TABLE_IDX 0x4b

extern uint8_t (*MemReadTable[])(uint32_t);
extern void    (*MemWriteTable[])(uint32_t, uint8_t);
extern void    safe_SASI_Write(uint32_t addr, uint8_t data);   /* sasi_bridge.c */

/* P273: 二重ゲートを構成するBridge側グローバル。
 * s_scsi_in_rom_loaded … EmulatorBridge.c(P247。SCSIINROM.DATロード成否)。 */
extern bool s_scsi_in_rom_loaded;

/* P251: 実SCSI(MB89352 SPC)ブリッジ(scsi_spc_bridge.cpp、extern "C")。
 * scsi_real_read/write は 0x00-0x1f 正規化済みoffsetを受け取る契約。 */
extern uint8_t scsi_real_read(uint32_t addr);
extern void    scsi_real_write(uint32_t addr, uint8_t data);
extern void    scsi_real_install_construct(void);
/* P510: scsi_real_set_machine_type() の呼び出しはここから削除した。後継の
 * scsi_real_set_scsi_mode() は mx68k_reset_hard() が本関数より**前**に無条件で
 * 1 回だけ呼ぶ(唯一の呼び出し箇所)。ここで呼んでいた旧実装は、SASI 機では
 * 早期 return して一度も呼ばれず前回リセット値が残る片道書換え(P447/D-31 型)
 * であり、かつ本関数の内部で走る scsi_real_install_construct() →
 * SCSI::Reset() が Memory::GetMemType() を読むため、順序としても手遅れだった。 */

/* P268: SCSI 機(machine==4)には内蔵 SASI が存在しないので offset 0x00-0x1f は
 * 不在(open-bus)化する。この関数群は scsi_in_bridge_install の machine==4 枝で
 * のみ install され(machine!=4 は早期 return)、SASI 機の経路には一切現れない。
 * 三重ゲート満了時は dispatch 経由で 0x00-0x1f のみここへ来る(0x20-0x3f は
 * scsi_real_* へ委譲・不変)。ゲート不成立時は 0x00-0x3f 全域がここへ来るが、
 * 従来の SASI_Read(SPC 未配線なのに SASI 値を返す)より open-bus の方が正しい。 */
static uint8_t insc_read(uint32_t addr)
{
    (void)addr;
    return 0xff;
}

static void insc_write(uint32_t addr, uint8_t data)
{
    (void)addr;
    (void)data;
}

/* P251 分配ハンドラ: 内蔵ウィンドウ内で offset 0x20-0x3f($E96020-3F)のみ
 * 実SCSIへ -0x20 正規化して委譲。offset 0x00-0x1f はSASI領域のまま不変。
 * XM6 sasi.cpp / XEiJ SPC_BASE_IN で確認済みの正規化。 */
static uint8_t scsi_in_dispatch_read(uint32_t addr)
{
    if ((addr & 0x3f) >= 0x20) {
        return scsi_real_read((addr & 0x3f) - 0x20);
    }
    return insc_read(addr);
}

static void scsi_in_dispatch_write(uint32_t addr, uint8_t data)
{
    if ((addr & 0x3f) >= 0x20) {
        scsi_real_write((addr & 0x3f) - 0x20, data);
        return;
    }
    insc_write(addr, data);
}

/* ★Code Review指摘・確定要件(Stage1): g_machine_type は EmulatorBridge.c 内で
 * static(内部リンケージ)のため extern 参照は不可。static を外さず、呼び出し
 * 側から値を引数で渡す設計を維持する。 */
void scsi_in_bridge_install(int machine_type)
{
    if (machine_type != 4) {
        /* SASI搭載機種: 差し替えない(sasi_bridge_installの状態のまま)。 */
        return;
    }

    /* P273: 二重ゲートの2条件値を無条件でログ出力(無言フォールバック禁止)。 */
    bool gate_machine = (machine_type == 4);
    bool gate_rom     = s_scsi_in_rom_loaded;
    debug_log("[P251] scsi_in wiring gate: machine=%d rom_loaded=%d\n",
              machine_type, gate_rom);

    if (gate_machine && gate_rom) {
        scsi_real_install_construct();
        MemReadTable[SCSI_IN_MEM_TABLE_IDX]  = scsi_in_dispatch_read;
        MemWriteTable[SCSI_IN_MEM_TABLE_IDX] = scsi_in_dispatch_write;
        debug_log("[P251] scsi_in_bridge: index0x4b -> scsi_in_dispatch_* (real SCSI wiring ON, sub-decode 0x20-0x3f)\n");
    } else {
        MemReadTable[SCSI_IN_MEM_TABLE_IDX]  = insc_read;
        MemWriteTable[SCSI_IN_MEM_TABLE_IDX] = insc_write;
        debug_log("[P251] scsi_in_bridge: index0x4b -> insc_* (passthrough, gate not satisfied)\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * P447 Phase 1 診断プローブ [P447-SASIWIRE]
 *
 * mx68k_reset_hard() の配線確定直後(sasi_bridge_install → scsi_in_bridge_install
 * の対が走った後、g_wired_machine_type を上書きする前)に**無条件で1回**呼ばれ、
 * MemReadTable/MemWriteTable[0x4b] の生の関数ポインタ値を、載りうる全候補
 * ハンドラのアドレスと同一行に並べて出力する。
 *
 * 自己反証可能性(CLAUDE.md「Self-Falsifiability Gate」):
 *  - フィルタ・条件付きトリガを一切持たないため、「0件」の解釈は
 *    「リセットが走っていない」の1通りに固定される。
 *  - 候補7シンボルを全て併記する = これが分母。read_slot がどの候補とも
 *    一致しない場合も「未知の第三者が載っている」と読み手が事後判定できる。
 *    期待シンボル1本だけを出す設計では「一致しなかった」と「誤ったシンボルと
 *    比較した」が区別できず、P378-P380 のベクタ定数誤りと同型の偽陰性を生む。
 *  - 派生値(「正しい/誤り」等の判定)は一切出力しない。生値のみを出し、
 *    判定は読み手が行う。取得時刻はリセット通し番号 n(単調増加)で識別する。
 *  - 標本自身が自分の素性を証明できるよう、判断の入力である machine
 *    (= g_machine_type, これから配線する機種)と wired_prev
 *    (= 上書き前の g_wired_machine_type, 直前セッションの機種)の両生値を併記。
 *  - SASI スロット在席の分母は hdimg_map(左から unit 0..7、Config.HDImage[unit*2]
 *    が非空なら '1')と n_hdimg=N/8。副因(C)「⌘R 経路で Config.HDImage[] が
 *    空のまま」を同じ標本で同時検証でき、「消えたのか / 元から入っていなかったのか」
 *    を標本自身で区別できる。非空スロットの生パスは同じ通し番号 n を付けた継続行で出す。
 *    ★P455: hdimg0= / hdimg2= は過去サイクルとの grep 継続性のために残した
 *    **部分集合**であり、これ単独を分母と読んではならない — 8 台化後は
 *    「unit 3 にイメージがあるのに hdimg0/hdimg2 が両方空」が正常に起こり得る。
 *
 * 本関数がこのファイルにあるのは、insc_read / insc_write /
 * scsi_in_dispatch_read / scsi_in_dispatch_write が static(内部リンケージ)で
 * あり、他の翻訳単位からアドレスを取得できないため。シンボルは全て実体参照し、
 * アドレス定数の直書きは行わない。
 * ═══════════════════════════════════════════════════════════════════════════ */
void scsi_in_bridge_log_slots(int machine_type, int wired_prev)
{
    static unsigned s_p447_reset_seq = 0;
    unsigned n = ++s_p447_reset_seq;

    /* P455: SASI スロット在席の分母(8 ユニット全て)。 */
    char hdimg_map[MX68K_SASI_UNIT_COUNT + 1];
    unsigned n_hdimg = 0;
    for (int u = 0; u < MX68K_SASI_UNIT_COUNT; u++) {
        int h = (Config.HDImage[u * 2][0] != '\0');
        hdimg_map[u] = h ? '1' : '0';
        n_hdimg += (unsigned)h;
    }
    hdimg_map[MX68K_SASI_UNIT_COUNT] = '\0';

    debug_log("[P447-SASIWIRE] n=%u phase=reset_hard machine=%d wired_prev=%d"
              " read_slot=%p write_slot=%p"
              " | cand: SASI_Read=%p safe_SASI_Write=%p SASI_Write=%p"
              " scsi_in_dispatch_read=%p scsi_in_dispatch_write=%p"
              " insc_read=%p insc_write=%p"
              " | hdimg_map=%s n_hdimg=%u/%d | hdimg0='%s' hdimg2='%s'\n",
              n, machine_type, wired_prev,
              (void *)(uintptr_t)MemReadTable[SCSI_IN_MEM_TABLE_IDX],
              (void *)(uintptr_t)MemWriteTable[SCSI_IN_MEM_TABLE_IDX],
              (void *)(uintptr_t)SASI_Read,
              (void *)(uintptr_t)safe_SASI_Write,
              (void *)(uintptr_t)SASI_Write,
              (void *)(uintptr_t)scsi_in_dispatch_read,
              (void *)(uintptr_t)scsi_in_dispatch_write,
              (void *)(uintptr_t)insc_read,
              (void *)(uintptr_t)insc_write,
              hdimg_map, n_hdimg, MX68K_SASI_UNIT_COUNT,
              Config.HDImage[0], Config.HDImage[2]);

    /* P455: 非空スロットの生パスを、同じ通し番号 n を付けた継続行で出す
     * (標本が自分の素性を証明できる形を維持。debug_log は固定バッファ非依存)。 */
    for (int u = 0; u < MX68K_SASI_UNIT_COUNT; u++) {
        if (Config.HDImage[u * 2][0] == '\0') continue;
        debug_log("[P447-SASIWIRE]   n=%u unit=%d idx=%d hdimg='%s'\n",
                  n, u, u * 2, Config.HDImage[u * 2]);
    }
}
