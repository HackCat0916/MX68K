#include <stdint.h>
#include <stdbool.h>
#include "scsi.h"
#include "EmulatorBridge.h"

/* 外付けSCSI(CZ-6BS1)I/Oは $EA0000 番台。
 * MemReadTable/WriteTable のインデックス: (0xEA0000 >> 13) & 0xFF == 0x50
 * → 既定では Core の SCSI_Read/SCSI_Write が直接配線されている(Bridge介入なし)。 */
#define SCSI_EXT_MEM_TABLE_IDX 0x50

/* P510: 窓内オフセットの分割点。$EA0000-$EA001F がSPCレジスタ、
 * $EA0020-$EA1FFF がIPL ROM(Inside X68000 印刷p.466 図7)。
 * 移植SPC実装(scsi_spc.cpp:346-374)も同じ 0x20 を境にレジスタ/ROMを分ける。 */
#define SCSI_EXT_WINDOW_MASK  0x1fffu
#define SCSI_EXT_REG_END      0x0020u
/* SSTS(ステータス)レジスタのオフセット。D-32 の永久ループが読み続けていた
 * 当のレジスタで、[P510-SCTL] の分母として読み出し総数を数える。
 * 一次情報源: Core/px68k/x68k/scsi.c:905 SetSSTS() の $EA000D。 */
#define SCSI_EXT_REG_SSTS     0x000du
/* DREG(データ)レジスタのオフセット。P269 の HDD ランプ busy パルス用。 */
#define SCSI_EXT_REG_DREG     0x0015u

extern uint8_t (*MemReadTable[])(uint32_t);
extern void    (*MemWriteTable[])(uint32_t, uint8_t);
extern void    StatBar_HDD(int32_t sw);

/* P510: 移植済みXM6 SPC実装(scsi_spc_bridge.cpp、extern "C")。
 * scsi_real_read/write は 0x00-0x1f 正規化済みoffsetを受け取る契約
 * (scsi_in_bridge.c と同一。内蔵は -0x20 が要るが外付けは窓の先頭が
 * そのままレジスタなので下位5bitをそのまま渡せる)。 */
extern uint8_t scsi_real_read(uint32_t addr);
extern void    scsi_real_write(uint32_t addr, uint8_t data);
extern void    scsi_real_install_construct(void);

/* P269: SCSI_Read/Writeの戻り値・副作用は一切変えない純粋な観測フック。
 * DREG($EA0015)アクセス時のみHDDランプのbusyパルスを追加発火。
 * P510 以降、ゲート成立時はROM域($EA0020-$EA1FFF)専用の経路となる。 */
static uint8_t scsi_ext_bridge_read(uint32_t addr)
{
    uint8_t v = SCSI_Read(addr);
    if (addr == 0xea0015) StatBar_HDD(1);
    return v;
}

static void scsi_ext_bridge_write(uint32_t addr, uint8_t data)
{
    SCSI_Write(addr, data);
    if (addr == 0xea0015) StatBar_HDD(1);
}

/* P510: $EA000D(SSTS)読み出し総数。[P510-SCTL] が「irq_count=0」を
 * 「そもそも外付けSPCに一度も喋っていない」と区別するための分母。
 * 累積(リセットしない)——ハング中に増え続けるかどうかを直接読むのが目的。 */
static uint32_t s_p510_ssts_rd_count = 0;

uint32_t p510_scsi_ext_ssts_read_count(void)
{
    return s_p510_ssts_rd_count;
}

/* P510 サブデコード分配器: scsi_in_bridge.c の scsi_in_dispatch_* と同型。
 *  offset 0x0000-0x001F → 移植済みSPC実装(scsi_real_read/write)。
 *  offset 0x0020-0x1FFF → 既存のCore SCSI_Read/SCSI_Write 経路を温存する。
 *    ★ROM を移植SPCの Memory::GetSCSI() 経路へ渡してはならない: MX の
 *      SCSIIPL[](Core/px68k/x68k/scsi.c)は LE16 スワップ格納で、Core 側の
 *      読み出しが `adr^1` で補正する前提になっている。移植実装はこの規約を
 *      共有しないため、渡すとバイト順が入れ替わったROMを返してしまう。 */
static uint8_t scsi_ext_dispatch_read(uint32_t addr)
{
    uint32_t off = addr & SCSI_EXT_WINDOW_MASK;
    if (off < SCSI_EXT_REG_END) {
        if (off == SCSI_EXT_REG_SSTS) s_p510_ssts_rd_count++;
        /* DREG の HDD ランプは scsi_real_read() 側が既に発火するため、
         * ここで重ねて呼ばない(二重パルス防止)。 */
        return scsi_real_read(off);
    }
    return scsi_ext_bridge_read(addr);
}

static void scsi_ext_dispatch_write(uint32_t addr, uint8_t data)
{
    uint32_t off = addr & SCSI_EXT_WINDOW_MASK;
    if (off < SCSI_EXT_REG_END) {
        scsi_real_write(off, data);
        return;
    }
    scsi_ext_bridge_write(addr, data);
}

/* P510: ゲート値は無条件にログ出力する(無言フォールバック禁止 —
 * scsi_in_bridge_install の [P251] ゲートログと同じ形)。
 * ゲート不成立時は従来どおり P269 観測フックのままで、この窓が非装着時に
 * バスエラーとなる P506(D-28)の合成とも直交する(あちらは
 * g_scsi_ext_board_wired==0 のときに m68000_bridge.c 側で成立する)。 */
void scsi_ext_bridge_install(int ext_wired, int ext_rom_loaded, int wired_machine_type)
{
    bool gate = (ext_wired != 0) && (ext_rom_loaded != 0);

    debug_log("[P510] scsi_ext wiring gate: ext_wired=%d ext_rom_loaded=%d wired_machine=%d\n",
              ext_wired, ext_rom_loaded, wired_machine_type);

    if (gate) {
        scsi_real_install_construct();
        MemReadTable[SCSI_EXT_MEM_TABLE_IDX]  = scsi_ext_dispatch_read;
        MemWriteTable[SCSI_EXT_MEM_TABLE_IDX] = scsi_ext_dispatch_write;
        debug_log("[P510] scsi_ext_bridge: index0x50 -> scsi_ext_dispatch_* "
                  "(real SPC wiring ON, sub-decode 0x00-0x1f; ROM 0x20-0x1fff stays on Core)\n");
    } else {
        MemReadTable[SCSI_EXT_MEM_TABLE_IDX]  = scsi_ext_bridge_read;
        MemWriteTable[SCSI_EXT_MEM_TABLE_IDX] = scsi_ext_bridge_write;
        debug_log("[P269] scsi_ext_bridge: index0x50 -> scsi_ext_bridge_* (Core passthrough, gate not satisfied)\n");
    }
}
