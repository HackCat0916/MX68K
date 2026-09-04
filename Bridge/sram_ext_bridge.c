#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sram_ext_bridge.h"
#include "EmulatorBridge.h"
/* P494-②: C68K インスタンスと C68k_Set_Fetch()。m68000_bridge.c と同じ入手経路。 */
#include "../Core/c68k/c68k.h"

/* P493: 内蔵 SRAM 64KB 化 Stage 1。低位 16KB($ED0000-$ED3FFF)は Core 既存の
 * SRAM[] を無改変で使い続け、上位 48KB($ED4000-$EDFFFF)だけを Bridge 所有
 * バッファで提供する。MemReadTable/MemWriteTable の index 0x6A-0x6F を
 * 64KB 設定時のみ差し替える(sasi_bridge.c / scsi_ext_bridge.c と同型の
 * 確立済みパターン)。
 *
 * index の根拠: (0xED0000 >> 13) & 0xff == 0x68、(0xEDFFFF >> 13) & 0xff == 0x6f。
 * Core/px68k/x68k/mem_wrap.c:76(read)/:116(write)で SRAM_Read/SRAM_Write が
 * ちょうど 8 エントリ連続しており、先頭 2 つ(0x68/0x69)が Core 既存の
 * SRAM[0x4000] に対応する低位 16KB。ここには一切触れない。 */
#define SRAM_EXT_TABLE_IDX_FIRST 0x6a
#define SRAM_EXT_TABLE_IDX_LAST  0x6f
#define SRAM_EXT_BASE            0x4000u   /* $ED4000 の $ED0000 からのオフセット */
#define SRAM_EXT_SIZE            0xC000u   /* 48KB = $ED4000-$EDFFFF */
#define SRAM_EXT_FILENAME        "sram_ext.dat"
#define SRAM_LOW_SIZE            0x4000u   /* 16KB = $ED0000-$ED3FFF(Core 所有 SRAM[]) */
#define SRAM_FETCH_SHADOW_SIZE   0x10000u  /* 64KB = $ED0000-$EDFFFF(Fetch[] 1 エントリ分) */

static uint8_t g_sram_ext[SRAM_EXT_SIZE];
static bool    g_sram_64k_enabled = false;
static bool    g_sram_ext_loaded  = false;   /* プロセス内で一度だけ読む */

extern uint8_t (*MemReadTable[])(uint32_t);
extern void    (*MemWriteTable[])(uint32_t, uint8_t);

/* Core シンボル。sasi_bridge.c が既に使っている「ヘッダを引かず extern 再宣言」
 * パターンと同じ入手経路。FASTCALL は Core/px68k/common.h:16 の空マクロなので
 * Core/px68k/x68k/sram.h:20-21 / sysport.h:6 の宣言と型は厳密一致する。 */
extern uint8_t SRAM_Read(uint32_t adr);
extern void    SRAM_Write(uint32_t adr, uint8_t data);
extern uint8_t SysPort[];
/* Core 所有の低位 16KB 実体。Core/px68k/x68k/sram.h:6 の宣言と厳密一致。 */
extern uint8_t SRAM[0x4000];

/* P494-②: 命令フェッチ専用シャドウ。SRAM[](16KB)+ g_sram_ext[](48KB)を
 * 生バイトのまま連結したもの。^1 スワップは一切自前で解決しない
 * ——本ビルドは -DC68K_BYTE_SWAP_OPT 付きでコンパイルされており
 * (MX68K.xcodeproj/project.pbxproj)、c68kmac.inc:206-210 の
 * `#ifndef C68K_BIG_ENDIAN` → `#ifdef C68K_BYTE_SWAP_OPT` 分岐が有効になる。
 * この分岐は FETCH_LONG のみを再定義し、FETCH_BYTE/FETCH_WORD はトップレベル
 * 定義のまま(`*(u16*)PC` のネイティブ 16bit 読み)。ネイティブ u16 読みが
 * 正しい 68k ワード値になるためには、ホスト側の格納バイトがあらかじめ adr^1 で
 * スワップ済みである必要があり、それは SRAM[] / g_sram_ext[] が既に持っている
 * 格納規約そのもの(sram.c の SRAM_Read/SRAM_Write、および本ファイルの
 * sram_ext_read/sram_ext_write が実装している)。したがって生の memcpy 連結が
 * 唯一正しく、アクセサ関数経由で 1 バイトずつ詰め直すと ^1 を二重に解決して
 * しまう。 */
static uint8_t g_sram_fetch_shadow[SRAM_FETCH_SHADOW_SIZE];

/* バイト順は sram.c と同一規約(SRAM_Read/SRAM_Write が実質 SRAM[adr^1] を
 * 読み書きするのに合わせる)。sram_ext.dat の並びも sram.dat と揃う。 */
uint8_t sram_ext_read(uint32_t addr)
{
    uint32_t off;
    if (!g_sram_64k_enabled) return 0xFF;      /* 現状維持(px68k 準拠) */
    off = (addr & 0xFFFFu) - SRAM_EXT_BASE;
    if (off >= SRAM_EXT_SIZE) return 0xFF;
    return g_sram_ext[off ^ 1u];
}

static void sram_ext_write(uint32_t addr, uint8_t data)
{
    uint32_t off;
    if (!g_sram_64k_enabled) return;           /* 現状維持: 無視 */
    off = (addr & 0xFFFFu) - SRAM_EXT_BASE;
    if (off >= SRAM_EXT_SIZE) return;
    if (SysPort[5] != 0x31) return;            /* sram.c:228 と同じ書込許可ゲート */
    g_sram_ext[off ^ 1u] = data;
}

void sram_ext_install_table(bool enabled)
{
    int i;
    g_sram_64k_enabled = enabled;
    for (i = SRAM_EXT_TABLE_IDX_FIRST; i <= SRAM_EXT_TABLE_IDX_LAST; i++) {
        if (enabled) {
            MemReadTable[i]  = sram_ext_read;
            MemWriteTable[i] = sram_ext_write;
        } else {
            /* index 0x6A-0x6F の元の関数は Bridge 内の他コードから一切触れられて
             * おらず、常に既知の SRAM_Read/SRAM_Write。明示的に復元する。 */
            MemReadTable[i]  = SRAM_Read;
            MemWriteTable[i] = SRAM_Write;
        }
    }
    debug_log("[P493-SRAMEXT] enabled=%d index=0x6A-0x6F -> %s\n",
              (int)enabled, enabled ? "sram_ext_read/write" : "SRAM_Read/SRAM_Write");
}

bool sram_ext_is_enabled(void)
{
    return g_sram_64k_enabled;
}

void sram_ext_load(const char* dir_path)
{
    char path[1024];
    FILE* fp;
    size_t n;

    /* 16KB 設定時は sram_ext.dat に一切触れない(既存ユーザーへの影響ゼロ)。 */
    if (!g_sram_64k_enabled || !dir_path || !dir_path[0]) return;
    /* ハードリセットのたびに読み直すとゲストが書いた内容が巻き戻るため、
     * プロセス内で一度だけ読む(実機のバッテリバックアップと同じ挙動)。 */
    if (g_sram_ext_loaded) return;
    g_sram_ext_loaded = true;

    snprintf(path, sizeof(path), "%s/" SRAM_EXT_FILENAME, dir_path);
    fp = fopen(path, "rb");
    if (!fp) {
        debug_log("[P493-SRAMEXT] load: no %s -> zero-filled\n", SRAM_EXT_FILENAME);
        return;
    }
    n = fread(g_sram_ext, 1, SRAM_EXT_SIZE, fp);
    fclose(fp);
    if (n < SRAM_EXT_SIZE) memset(g_sram_ext + n, 0, SRAM_EXT_SIZE - n);
    debug_log("[P493-SRAMEXT] load: %s read=%zu/%u\n",
              SRAM_EXT_FILENAME, n, (unsigned)SRAM_EXT_SIZE);
}

void sram_ext_save(const char* dir_path)
{
    char path[1024];
    FILE* fp;

    if (!g_sram_64k_enabled || !dir_path || !dir_path[0]) return;
    snprintf(path, sizeof(path), "%s/" SRAM_EXT_FILENAME, dir_path);
    fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(g_sram_ext, 1, SRAM_EXT_SIZE, fp);
    fclose(fp);
    debug_log("[P493-SRAMEXT] save: %s written=%u\n",
              SRAM_EXT_FILENAME, (unsigned)SRAM_EXT_SIZE);
}

/* ---- P494-①: ゼロクリア --------------------------------------------------- */

void sram_ext_clear(void)
{
    memset(g_sram_ext, 0, SRAM_EXT_SIZE);
    /* Fetch シャドウも実体に追随させる(呼出し元 mx68k_sram_clear() は
     * memset(SRAM,...) を先に済ませているため、ここでの再構築で低位 16KB・
     * 上位 48KB の両方がクリア後の状態になる)。 */
    sram_ext_fetch_shadow_rebuild();
    debug_log("[P494-SRAMEXT] clear: upper 48KB zeroed (enabled=%d)\n",
              (int)g_sram_64k_enabled);
}

/* ---- P494-②: 命令フェッチ用シャドウ --------------------------------------- */

void sram_ext_fetch_shadow_rebuild(void)
{
    /* 単なるスナップショット(副作用なし)なので 64KB 設定の有効/無効で分岐しない。
     * 無効時は Fetch[] がシャドウを指していないため、書き換えても観測されない。
     * ここで早期 return すると「install_fetch(true) が呼ばれたのにラッチが
     * まだ false」という初期化順序の隙間でシャドウが 0 のまま Fetch[] に
     * 配線される、という事故を作り込むため、あえて無条件にする。 */
    memcpy(g_sram_fetch_shadow, SRAM, SRAM_LOW_SIZE);
    memcpy(g_sram_fetch_shadow + SRAM_LOW_SIZE, g_sram_ext, SRAM_EXT_SIZE);
}

void sram_ext_fetch_shadow_note(uint32_t addr_raw)
{
    if (!g_sram_64k_enabled) return;                       /* 16KB 設定時は完全に無関係 */
    if ((addr_raw & 0x00FF0000u) != 0x00ED0000u) return;   /* $ED0000-$EDFFFF 以外は無視 */
    /* 書込み許可ゲート(SysPort[5]==0x31)等の判定はここで重複実装しない。
     * 実体(SRAM_Write / sram_ext_write)が処理し終えた「結果」を丸ごと写すだけなので、
     * ゲート不通過の書込みは実体が変わっておらず、自動的に反映されない。
     * SRAM 領域への書込みはバッテリバックアップ用途で稀のため、毎回 64KB を
     * 再構築しても性能上の問題はない。 */
    sram_ext_fetch_shadow_rebuild();
}

void sram_ext_install_fetch(bool enabled)
{
    if (enabled) {
        sram_ext_fetch_shadow_rebuild();
        /* $ED0000-$EDFFFF の 64KB 全域(Fetch[] index 0xED の 1 エントリ)を
         * 連続シャドウへ向ける。C68k_Set_Fetch は
         * i=(low>>16)&0xff, j=(high>>16)&0xff で index 化するため、
         * low=0xed0000 / high=0xedffff は i=j=0xED(Core/c68k/c68k.c:220-224)。 */
        C68k_Set_Fetch(&C68K, 0xed0000, 0xedffff, (uintptr_t)g_sram_fetch_shadow);
    } else {
        /* P494 以前の呼出しと完全に同一(低位 16KB のみ・Core 所有 SRAM[] を直接参照)。 */
        C68k_Set_Fetch(&C68K, 0xed0000, 0xed3fff, (uintptr_t)SRAM);
    }
    debug_log("[P494-SRAMEXT] install_fetch: enabled=%d -> %s\n",
              (int)enabled, enabled ? "shadow(64KB)" : "SRAM[](16KB)");
}

/* ---- P495: セーブステート連携 --------------------------------------------- */

const uint8_t* sram_ext_snapshot64(void)
{
    /* 64KB 設定の有効/無効に関わらず常に最新化する(D-11/P472 の
     * 「常に物理最大を保存」パターンを SRAM へ適用)。書込み専用の別バッファは
     * 新設せず、既存の Fetch シャドウ(SRAM[] + g_sram_ext[] の生バイト連結)を
     * そのままスナップショットとして再利用する。 */
    sram_ext_fetch_shadow_rebuild();
    return g_sram_fetch_shadow;
}

void sram_ext_restore_upper48(const uint8_t* src48)
{
    if (!src48) return;
    memcpy(g_sram_ext, src48, SRAM_EXT_SIZE);
    /* 実体を書き換えたので Fetch シャドウを追随させる(呼出し元は低位 16KB の
     * memcpy(SRAM,...) を先に済ませているため、ここでの再構築で 64KB 全体が
     * 復元後の状態になる)。 */
    sram_ext_fetch_shadow_rebuild();
    debug_log("[P495-SRAMEXT] restore: upper 48KB from state (enabled=%d)\n",
              (int)g_sram_64k_enabled);
}
