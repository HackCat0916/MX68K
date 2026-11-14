/* ===========================================================================
 *  P949: FDC テーブル差替え配線の素通し診断プローブ [P949-FDCPT](既定 OFF)
 * ---------------------------------------------------------------------------
 *  インデックス 0x4A = ($E94000 >> 13) & 0xFF(FDC レジスタ窓)。
 *  差替え関数は元の FDC_Read/FDC_Write/FDC_IsDataReady を呼ぶだけで、
 *  戻り値・副作用は一切変えない(呼出し回数のカウントのみ)。
 *
 *  ★floppyu_regprobe_shim.h(P939)は include しないこと。あのヘッダは
 *    FDC_Read/FDC_Write を観測ラッパーへマクロ置換するため、ここでは素の
 *    シンボルを extern 宣言で直接参照する。
 *  ★既知の制約(コードでは防止しない):
 *    - 有効時は表 0x4A が本振り分け器を指すため、P939 観測(MX68K_FLOPPYU_PROBE=1)は
 *      有効中は止まる。OFF へ戻すと保存済みの観測ラッパーが復元されて再開する(P958)。
 *      振り分け器内部は素の FDC_Read を呼ぶ(内部の MSR ポーリングを観測に混入させないため、意図的)。
 *    - MX68K_FLOPPYU_DELAY_MS(P940)と同時有効化すると、DMA_SetReadyCB(0) の
 *      install 順の後勝ちで P940 の ReadyCB が無効化される(有効時は本振り分け器が勝つ)。
 *
 *  P955: ふろっぴU君割当てドライブ向けFDCコマンド振り分け器の骨格 [P955-ROUTER]。
 *    割当てドライブ宛てコマンドは常に「FDなし(Not Ready)」を返すスタブへ、
 *    残りのドライブは従来どおりCoreへ振り分ける。スタブはUSB実機通信を
 *    配線するまでの仮の受け口であり、実機とは一切通信しない。
 *
 *  P958: 有効/無効は config.json の設定値(mx68k_set_floppyu_config 経由)で決まる(既定 OFF)。
 *    環境変数 MX68K_FLOPPYU_FDC_PASSTHROUGH / MX68K_FLOPPYU_ASSIGNED_DRIVE は廃止。
 *    割当てはペア(ドライブ0/1 または 2/3)。2/3 は外付けFDDユニット配線時のみ有効。
 *    install は init/hardreset ごとに設定値から再計算し、配線行 [P958-FLOPPYU] を無条件に出す。
 *
 *  P1009: 振り分け器本体のUSB実機中継・第1段(読出しのみ)[P1009-ROUTER]。
 *    「中継可能」(振り分け器有効・USB通信層が起動済み[P1054: 環境変数 MX68K_FLOPPYU_USB が "1"=強制ON /
 *    "0"=強制OFF / 未設定=設定「Use FloppyU」に従う]・接続済み・P1004スライス未起動・
 *    当該接続世代で故障停止していない・ブリングアップ済み)のときだけ、割当てドライブ宛ての
 *    コマンドをコマンド単位で装置へ中継する。不成立なら従来のスタブ(Not Ready)のまま。
 *    中継するもの: SPECIFY(Coreへも送るミラー)・SENSE DEVICE STATUS・RECALIBRATE/SEEK・
 *    SENSE INTERRUPT STATUS・READ ID・単一セクタのREAD DATA・$E94005/$E94007。
 *    READ DATA のデータは、受信バッファを Core の DMA ch0 が ReadyCB 経由で1Bずつ引き取る
 *    (候補C1、Core標準FD経路と同じ順序で最後のデータB読出し時に割込み)。
 *    WRITE系は装置へ一切送らず書込み禁止(NW)エラーを合成する(MX独自方針)。
 *    (P1031: MX68K_FLOPPYU_WRITE=dryrun でデータ収集のみ。P1032: =wponly で、送信直前の非キャッシュ
 *     SDS が FT=0・WP=1・RY=1 を示した WRITE DATA 単一セクタに限り装置へ実送信する [P1032-WRSEND])
 *    マルチセクタ・MT=1・READ DELETED/DIAG・SCAN も装置へ送らず異常終了を合成する。
 *    port1 の同期往復はエミュレーションスレッド上で行い、port2 の唯一の消費者もこのスレッド。
 *    設計・定数の根拠は .mx68k_cycles/P1009_plan.md(記号表・変更内容 1-a〜1-k)。
 *
 *  P1054: 書込みモードは環境変数 MX68K_FLOPPYU_WRITE(優先)> 設定「Allow writing to FloppyU disks」
 *    (ON で normal、OFF で書込み全拒否)の順で install ごとに決める [P1054-CFG]。
 *    あわせて 'D'/'d' 後のゲスト自身の $E94005 読出しを読むだけで記録する受動観測 [P1054-E5P] を持つ
 *    (診断専用・挙動不変。e5p_* 関数群は振り分け器の状態に一切書き込まない)。
 * =========================================================================== */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "floppyu_fdc_passthrough_probe.h"
#include "floppyu_usb_transport.h"
#include "floppyu_vslice.h"   /* mx68k_floppyu_vslice_claimed */
#include "dmac.h"   /* Core:dmac.h、DMA[4]・dmac_ch 構造体・OCR/MTC フィールド定義(読取りのみ) */
#include "../Core/px68k/x68k/m68000.h"   /* M68K_PC/M68K_SR 列挙値(読取りのみ、Bridge:m68000_bridge.c:7 と同じ手法) */

extern void debug_log(const char* fmt, ...);   /* Bridge/EmulatorBridge.c で定義 */
extern uint32_t m68000_get_reg(int32_t regnum); /* Bridge:m68000_bridge.c で定義、ヘッダ未宣言 */
extern int  g_mx68k_frame_num;                 /* EmulatorBridge.c で定義 */
extern int  mx68k_turbo_audio_ratio_q16_get(void); /* P1064: EmulatorBridge.c で定義(読取り専用、ログ記録のみ) */

/* Core:x68k/mem_wrap.c:58/98 の表、Core:x68k/fdc.h:7/8/14、Core:x68k/dmac.h:48。
 * FASTCALL は Core:common.h:16 で空定義のため省略している。 */
extern uint8_t (*MemReadTable[])(uint32_t);
extern void    (*MemWriteTable[])(uint32_t, uint8_t);
extern uint8_t FDC_Read(uint32_t adr);
extern void    FDC_Write(uint32_t adr, uint8_t data);
extern int32_t FDC_IsDataReady(void);
extern void    DMA_SetReadyCB(int32_t ch, int32_t (*func)(void));
extern int32_t FDC_Int(uint8_t irq);                  /* Core:fdc.c:104、ヘッダ未宣言 */
extern int32_t FDD_Int(uint8_t irq);                  /* Core:fdd.h:20(P1052) */
extern void    IRQH_Int(uint8_t irq, void* handler);  /* Core:irqh.h:9 */
extern void    IRQH_IRQCallBack(uint8_t irq);         /* Core:irqh.h:8 */
extern uint8_t IRQH_IRQ[8];                           /* Core:irqh.c:18、ヘッダ未宣言(Bridge:fd_latency_experiment.c:49 と同じ) */
extern void   *IRQH_CallBack[8];                      /* Core:irqh.c:19、ヘッダ未宣言 */
extern int32_t VLINE;                                 /* Core:winx68k.h:22(P1100、読取りのみ。winx68k.h は include しない) */
extern uint8_t IOC_IntVect;                           /* Core:ioc.h:7 */
/* P939 観測ラッパー(Bridge:floppyu_regprobe.c)。シムは include しない方針のため直接宣言する */
extern uint8_t mx68k_floppyu_fdc_read_probe(uint32_t adr);
/* Core:x68k/x68kmemory.h:35。実体 Core:x68k/mem_wrap.c:386 は rm_main(addr) を呼ぶだけで、
 * 0x000000-0xBFFFFF(RAM)では MEM の16bit語から該当バイトを取り出すのみ(副作用なし、
 * C68K_BYTE_SWAP_OPT 相当の並びもここで吸収される)。RAM 以外は MemReadTable 経由で
 * 副作用があり得るため、P1009 の照合は RAM 域に限って呼ぶ。 */
extern uint8_t dma_readmem24(uint32_t addr);

#define P949_FDC_TABLE_IDX  0x4A
#define P949_SUM_PERIOD     300

/* P954: Core:fdc.c:15 CMD_TABLE の複写(一次情報源)。別翻訳単位のため
 * 直接参照できない — 既存の Bridge:m68000_bridge.c P509_CMD_TABLE と
 * 同じ複写パターン。*/
static const uint8_t P954_CMD_TABLE[32] = {0, 0, 8, 2, 1, 8, 8, 1, 0, 8, 1, 0, 8, 5, 0, 2,
                                           0, 8, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 8, 0, 0};

static int      s_p949_enabled     = 0;   /* P958: 直近の init/hardreset で確定した配線状態 */
static int      s_p949_last_sum_f  = -1;
static uint32_t s_p949_read_count  = 0;
static uint32_t s_p949_write_count = 0;
static uint32_t s_p949_ready_count = 0;

extern uint8_t IOC_IntStat;   /* Core:ioc.h、既存公開 */

/* P958: 初回差替え時に表 0x4A から実際に読み取った元の関数ポインタ(OFF 時の復元用) */
static uint8_t (*s_p958_orig_read)(uint32_t)           = NULL;
static void    (*s_p958_orig_write)(uint32_t, uint8_t) = NULL;

/* P951: ReadyCB 呼出し時の DMA[0].OCR & 3 別回数(R3)と、DMA_Exec(0) 直後の
 * 割込み/MTC 残量関係(R1)。分母と分子を両方出し「0件」と「未計測」を区別する。 */
static uint32_t s_p951_ready_ocr_count[4]     = {0, 0, 0, 0};
static uint32_t s_p951_dma0_exec_count        = 0;
static uint32_t s_p951_irq_before_drain_count = 0;

/* P952: ReadyCB 呼出し時の DMA[0].OCR bit7(転送方向)別回数。
 * [0]=Memory->Device(WRITE、bit7=0)、[1]=Device->Memory(READ、bit7=1)。 */
static uint32_t s_p952_ready_dir_count[2] = {0, 0};

/* P954: US(コマンド第2バイト下位2bit)と $E94007 由来 drvsel(Core実体の
 * fdc.drv 相当)の帰属キー一致/不一致確認。分母を全て併記する(自己反証可能性)。 */
static int32_t  s_p954_wrnum      = 0;   /* Core:fdc.c fdc.wrnum の複写(残りパラメータバイト数) */
static int      s_p954_us_pending = 0;   /* 次のバイトがUS対応コマンドの最初のパラメータか */
static int32_t  s_p954_drvsel     = 0;   /* $E94007 書込みのミラー(-1=非選択)。初期値0はCore:fdc.c
                                            FDC_Init() の memset(&fdc,0,...) に合わせる(P686同様) */
static uint32_t s_p954_cmd_total              = 0;
static uint32_t s_p954_us_match_count         = 0;
static uint32_t s_p954_us_mismatch_count      = 0;
static uint32_t s_p954_specify_count          = 0;
static uint32_t s_p954_sense_int_status_count = 0;
static uint32_t s_p954_other_no_param_count   = 0;
/* cmd=5/9/13/17/25/29 は $E94003 への書込みデータフェーズ(fdc.bufnum管理、
 * Bridgeから見えない)を持つため、この区間はミラーがデータバイトを新規コマンド
 * 開始と誤認識し us_match/mismatch へ偽データが混入しうる。汚染の可視化用カウンタ。 */
static uint32_t s_p954_write_phase_cmd_count  = 0;

/* P955: 振り分け器の状態。P958 でペア割当て(mask=0x3/base=0・mask=0xC/base=2)へ移行済み。
 * P1009: ペア内の切替は、ホストがユニット番号をそのまま渡し装置側FDCがSELECTで切り替える
 *   (P1009_spec_inv_followup (e))ため、MXは常に装置ユニット0/1へ番号変換して送るだけでよい。 */
#define P955_FIFO_DEPTH   8u
/* P1009: RELAY は割込みFIFOには積まない(FIFOに積むのは CORE/STUB の2値だけ、§1-b) */
enum { P955_OWNER_CORE = 0, P955_OWNER_STUB = 1, P1009_OWNER_RELAY = 2 };

static int      s_p955_enabled = 0;
static uint8_t  s_p955_mask = 0;          /* 割当てMXドライブのビット集合 */
static uint8_t  s_p955_base = 0;          /* 番号変換の基底(0 or 2)。ふろっぴU君はユニット0/1で応答する想定・未確定 */
static int      s_p955_owner = P955_OWNER_CORE;   /* 直近のコマンド開始を受けた側(読出し・ReadyCBの経路) */
static int      s_p955_pending = 0;       /* US対応コマンドの第1バイトを保留中 */
static uint32_t s_p955_pending_adr = 0;
static uint8_t  s_p955_pending_byte = 0;
static int32_t  s_p955_drvsel_mx = 0;     /* $E94007ミラー(-1=非選択)。Core:fdc.c FDC_Init の memset に合わせ 0 */
static uint8_t  s_p955_core_ctrl = 0;     /* Coreへ渡した(と見なす)$E94005値。install時に初期化 */
static uint8_t  s_p955_dev_ctrl = 0;      /* 装置側の$E94005値(変換後)。P1009: 中継可能時に rl_e5_write が送信 */
static uint8_t  s_p955_dev_drvsel = 0;    /* 装置側の$E94007値(変換後、ミラーのみ)。送信は rl_e7_write */
static uint8_t  s_p955_int_fifo[P955_FIFO_DEPTH];  /* Seek/Recalibrate完了割込みの発行側の到着順 */
static uint32_t s_p955_int_head = 0, s_p955_int_count = 0;

/* スタブ(装置ユニット番号空間で動く。MX番号への逆変換は結果生成直後に1回だけ行う) */
static uint8_t  s_p955_stub_cmd = 0;
static uint8_t  s_p955_stub_prm[10];
static uint32_t s_p955_stub_wrptr = 0, s_p955_stub_wrnum = 0;
static uint8_t  s_p955_stub_rsp[10];
static uint32_t s_p955_stub_rdptr = 0, s_p955_stub_rdnum = 0;
static uint8_t  s_p955_stub_int_st0 = 0x80;   /* Core:fdc.c:295 と同じ「保留割込み無し」初期値 */

/* 集計(分母と分子を併記する、自己反証可能性) */
static uint32_t s_p955_cmd_start, s_p955_core_cmd, s_p955_stub_cmd_count, s_p955_core_noparam;
static uint32_t s_p955_sense_int_core, s_p955_sense_int_stub, s_p955_key_disagree;
static uint32_t s_p955_fifo_overflow, s_p955_stub_int_raised, s_p955_us_hist[4];
static uint8_t  s_p955_last_stub_cmd, s_p955_last_us_mx, s_p955_last_us_dev, s_p955_last_st0_dev, s_p955_last_st0_mx;
static int      s_p955_last_stub_f = -1;

/* ---- 番号変換ヘルパ(MX番号↔装置ユニット番号) ----
 * 前提: mask 内の全ビット d について base <= d <= base+1。P958 のペア割当ては
 * mask∈{0x3,0xC}・base∈{0,2} の対で設定する——対応がずれると
 * 1u << (d - s_p955_base) が負シフト(未定義動作)になる。install時に検査する。
 * base=0(割当て0または1)では恒等変換になる。 */

/* MX側の第2バイト(US/HD込み)→装置側。呼出し側で us∈mask を保証済み */
static uint8_t p955_us_mx_to_dev(uint8_t b) { return (uint8_t)((b & ~3u) | (((b & 3u) - s_p955_base) & 3u)); }
/* 装置側ST0/ST3→MX側。IC=10(0x80系、無効コマンド応答)はユニット欄を持たないので変換しない */
static uint8_t p955_st_dev_to_mx(uint8_t st) {
    if ((st & 0xC0u) == 0x80u) return st;
    return (uint8_t)((st & ~3u) | (((st & 3u) + s_p955_base) & 3u));
}
/* $E94005: 割当てMXドライブdのビットを装置ビット(d-base)へ移す。上位ビット(0x20/0x40/0x80)はそのまま */
static uint8_t p955_ctrl_mx_to_dev(uint8_t data) {
    uint8_t dev = data & 0xF0u;
    for (uint8_t d = 0; d < 4u; d++)
        if (((s_p955_mask >> d) & 1u) && ((data >> d) & 1u)) dev |= (uint8_t)(1u << (d - s_p955_base));
    return dev;
}

/* P1028: 'C'由来のFDC割込みの遅延発行。
 * live     = 直近にFDC割込み線を立てたのが中継の'C'で、まだ取り下げ/他要因の発行で上書きされていない
 * deferred = 新規コマンド処理中のため取り下げ(または発行を見送り)、コマンド完了時に再発行待ち */
static int      s_rl_c_irq_live = 0;
static int      s_rl_c_irq_deferred = 0;

/* ---- スタブ(Not Ready固定、装置ユニット空間) ---- */
static void p955_stub_raise_int(void) {
    /* Core:fdc.c:137-141 FDC_SetInt の複写(static のため直接呼べない) */
    s_rl_c_irq_live = 0;
    IOC_IntStat |= 0x80;
    if (IOC_IntStat & 4) IRQH_Int(1, &FDC_Int);
    s_p955_stub_int_raised++;
}

/* 中継不可時(未接続・USB無効・故障停止等)のフォールバック。中継可能時は P1009 の rl_* が担う。
 * 既知の残留リスク: C/H/R/N はパラメータのエコー。Core の FDC_EPhaseEnd
 *   (FDD_GetCurrentID)とは異なり、空ドライブ時のCore値と一致する保証は無い。 */
static void p955_stub_exec(void) {
    uint8_t cmd = s_p955_stub_cmd;
    uint8_t unit = (uint8_t)(s_p955_stub_prm[1] & 7u);   /* cmd8 では使わない */
    int raise = 0;
    memset(s_p955_stub_rsp, 0, sizeof(s_p955_stub_rsp));
    s_p955_stub_rdptr = 0;
    s_p955_stub_rdnum = 0;
    switch (cmd) {
    case 2: case 5: case 6: case 9: case 12: case 17: case 25: case 29:
        s_p955_stub_rsp[0] = (uint8_t)(0x48u | unit);
        s_p955_stub_rsp[1] = 0;
        s_p955_stub_rsp[2] = (cmd == 17u || cmd == 25u || cmd == 29u) ? 0x08u : 0u;   /* Core:fdc.c:532 */
        s_p955_stub_rsp[3] = s_p955_stub_prm[2];
        s_p955_stub_rsp[4] = s_p955_stub_prm[3];
        s_p955_stub_rsp[5] = s_p955_stub_prm[4];
        s_p955_stub_rsp[6] = s_p955_stub_prm[5];
        s_p955_stub_rdnum = 7;
        raise = 1;
        break;
    case 10: case 13:
        s_p955_stub_rsp[0] = (uint8_t)(0x48u | unit);
        s_p955_stub_rdnum = 7;
        raise = 1;
        break;
    case 4:   /* SenseDeviceStatus: 割込み無し(Core:fdc.c:217-223) */
        s_p955_stub_rsp[0] = (uint8_t)(unit | 0x10u);
        s_p955_stub_rdnum = 1;
        break;
    case 7: case 15:   /* Recalibrate/Seek: 結果フェーズ無し、cyl は常に0 */
        s_p955_stub_int_st0 = (uint8_t)(0x48u | unit);
        raise = 1;
        break;
    case 8:   /* SenseIntStatus: P1009 で Core(P982後、Core:fdc.c:294-306)と同形に。
               * 保留ありは ST0+PCN の2バイト(スタブはPCNを更新しないので常に0)、無しは0x80の1バイト */
        if (s_p955_stub_int_st0 != 0x80u) {
            s_p955_stub_rsp[0] = s_p955_stub_int_st0;
            s_p955_stub_rsp[1] = 0;
            s_p955_stub_rdnum = 2;
        } else {
            s_p955_stub_rsp[0] = 0x80u;
            s_p955_stub_rdnum = 1;
        }
        s_p955_stub_int_st0 = 0x80;
        break;
    default:
        break;
    }
    if (s_p955_stub_rdnum > 0u) {
        s_p955_last_st0_dev = s_p955_stub_rsp[0];
        s_p955_stub_rsp[0] = p955_st_dev_to_mx(s_p955_stub_rsp[0]);
        s_p955_last_st0_mx = s_p955_stub_rsp[0];
    }
    s_p955_last_stub_f = g_mx68k_frame_num;
    if (raise) p955_stub_raise_int();
}

static void p955_stub_start(uint8_t byte) {
    s_p955_stub_cmd = byte & 0x1Fu;
    s_p955_stub_wrptr = 0; s_p955_stub_rdptr = 0; s_p955_stub_rdnum = 0;
    s_p955_stub_prm[s_p955_stub_wrptr++] = byte;
    s_p955_stub_wrnum = P954_CMD_TABLE[s_p955_stub_cmd];
    if (s_p955_stub_wrnum == 0) p955_stub_exec();
}
static void p955_stub_param(uint8_t byte) {
    if (s_p955_stub_wrptr < sizeof(s_p955_stub_prm)) s_p955_stub_prm[s_p955_stub_wrptr++] = byte;
    if (--s_p955_stub_wrnum == 0) p955_stub_exec();
}

/* P1009: スタブと中継で共通の「Core式」MSR(Core:fdc.c:459-464 の式、wexec は常に0)。
 * 旧スタブにあった SENSE INT 1バイト時の 0xaf マスクは、P982 後の Core に存在しないため削除した。 */
static uint8_t p1009_core_msr(uint32_t wrnum, uint32_t rdnum) {
    uint8_t ret = 0x80u;
    ret |= rdnum ? 0x40u : 0u;
    ret |= (wrnum || rdnum) ? 0x10u : 0u;
    return ret;
}
static uint8_t p955_stub_msr(void) {
    return p1009_core_msr(s_p955_stub_wrnum, s_p955_stub_rdnum);
}
/* スタブのリザルト読出し(残り無しは Core:fdc.c:454 の初期値 0xff) */
static uint8_t p955_stub_read_data(void) {
    if (s_p955_stub_rdnum > 0u && s_p955_stub_rdptr < sizeof(s_p955_stub_rsp)) {
        s_p955_stub_rdnum--;
        return s_p955_stub_rsp[s_p955_stub_rdptr++];
    }
    return 0xffu;
}

/* ---- 割込みFIFO(Seek/Recal完了割込みの発行側を到着順に保持) ---- */
static void p955_int_fifo_push(int side) {
    if (s_p955_int_count == P955_FIFO_DEPTH) {   /* 満杯: 最古を捨てる */
        s_p955_int_head = (s_p955_int_head + 1u) % P955_FIFO_DEPTH;
        s_p955_int_count--;
        s_p955_fifo_overflow++;
    }
    s_p955_int_fifo[(s_p955_int_head + s_p955_int_count) % P955_FIFO_DEPTH] = (uint8_t)side;
    s_p955_int_count++;
}
static int p955_int_fifo_pop(void) {
    if (s_p955_int_count == 0u) return P955_OWNER_CORE;   /* 空ならCore(既定経路と同一挙動) */
    int side = s_p955_int_fifo[s_p955_int_head];
    s_p955_int_head = (s_p955_int_head + 1u) % P955_FIFO_DEPTH;
    s_p955_int_count--;
    return side;
}

/* ===========================================================================
 *  P1009: 中継状態機械(エミュレーションスレッドのみで動く)
 * =========================================================================== */
#define P1009_TAG             "[P1009-ROUTER]"
#define FU_WR                 0x80u     /* 書込み指示ビット(XM6t fdc_bridger.h:666-667) */
#define FU_REG_MSR            0x01u     /* レジスタ番号 = $E9400x & 0x0F(XM6t vm/fdc.cpp:456/569) */
#define FU_REG_DATA           0x03u
#define FU_REG_E5             0x05u
#define FU_REG_E7             0x07u
#define FU_FRAME_CAP          16391u    /* 'R'本体受理上限 = 1024×16+7(XM6t m_recvData[20000]) */
#define FU_RESULT_LEN         7u        /* Core:fdc.c:17 DAT_TABLE[6]=7 */
#define FU_READ_DEADLINE_MS   3000      /* READ送信から'R'検出までの上限(ホスト時間) */
#define FU_READ_STALL_MS      1000      /* 'R'検出後の無着信上限(XM6t fdc_bridger.h:880-887) */
#define FU_RID_DEADLINE_MS    3000
#define FU_RID_POLL_MIN_US    2000      /* READ ID待ち中の装置MSR問合せ最小間隔(XM6t送信キュー2ms周期) */
#define FU_P1_TIMEOUT_MS      100       /* 同期往復1回の上限(XM6t実測最大2.6msの約40倍) */
#define FU_SDS_CACHE_MS       500       /* XM6t fdc_bridger.h:588-616 */
#define FU_E5_CACHE_MS        500       /* XM6t fdc_bridger.h:365-397 */
#define FU_SEEK_C_TIMEOUT_MS  3000
#define FU_DNB_TIMEOUT_MS     3000      /* P1010: 合成DnBの強制解除上限(FU_SEEK_C_TIMEOUT_MSと同値) */
#define P1010_TAG             "[P1010-DNB]"
#define FU_SENSE_DRAIN_MAX    8         /* 範囲外ST0の読み捨て上限(範囲外最大2件×余裕4倍) */
#define FU_P1008_REF_FNV      0x206549b3u
#define FU_LOG_LIMIT          32u       /* c_event/refused 行の上限 */
#define FU_SDS_LOG_LIMIT      16u
#define FU_EJECT_GUARD_FRAMES 30u        /* P1058: 合成エラー直後の排出指示を抑止する猶予フレーム数(MX独自値、記号表参照) */
#define P1058_EJGUARD_TAG "[P1058-EJGUARD]"
#define MSR_RQM               0x80u     /* µPD765 MSR bit7(Core:fdc.c:471-474) */
#define MSR_DIO               0x40u     /* µPD765 MSR bit6、1=FDC→ホスト(Core:fdc.c:473) */
#define BRINGUP_POLL_INTERVAL_MS 10     /* P1026: 初期化後の装置MSRポーリング間隔 */
#define BRINGUP_POLL_TIMEOUT_MS  2500   /* P1026: 1回の bringup 試行あたりのポーリング上限 */
#define BRINGUP_FAIL_LIMIT       3      /* P1026: 同一接続世代でポーリング不成立がこの回数連続したら故障停止 */
#define BRINGUP_HIST_MAX         32u

static const uint8_t FU_INIT[] = { 0x80, 0xFE, 0x40 };          /* XM6t fdc_bridger.h:92-97、P1004 S1 */
static const uint8_t FU_IOC[]  = { 0x93, 0x60, 0x91, 0x06 };    /* INT.VECTOR=0x60/INT.MASK=0x06、P1004 S2 */

/* P1032: RL_WR_WAIT_W=書込みコマンド送信済・'W'/'w'/'C'待ち、RL_WR_WAIT_DONE=データ送信済・'w'/'C'待ち、
 * RL_WR_RESULT=装置リザルト取得待ち(wponly モードのみ到達する) */
enum { RL_IDLE = 0, RL_RD_WAIT, RL_RD_SUPPLY, RL_RID_WAIT, RL_RESULT, RL_WR_COLLECT,
       RL_WR_WAIT_W, RL_WR_WAIT_DONE, RL_WR_RESULT, RL_NSTATE };
static const char* const RL_STATE_NAME[RL_NSTATE] = { "idle", "rd_wait", "rd_supply", "rid_wait", "result", "wr_collect",
                                                      "wr_wait_w", "wr_wait_done", "wr_result" };
enum { RE_TIMEOUT = 0, RE_STALL, RE_DISCONNECT, RE_SHORT, RE_OVERFLOW, RE_ABORTED, RE_T_EVENT, RE_FAULT, RE_MEDIA, RE_N };
static const char* const RE_NAME[RE_N] = { "timeout", "stall", "disconnect", "short", "overflow",
                                           "aborted_by_cmd", "t_event", "fault", "media_change" };
/* port2 フレーム本体の行先: TARGET=進行中READ / STRAY=READ以外で届いた / DISCARD=中断済みREADの残り */
enum { RL_P2_NONE = 0, RL_P2_TARGET, RL_P2_STRAY, RL_P2_DISCARD };

static int      s_rl_usb_on = 0;           /* P1054: install 時点で USB通信層が起動済みか(mx68k_floppyu_usb_is_enabled) */
static int      s_rl_state = RL_IDLE;
static uint8_t  s_rl_cmd[10];              /* [1] は装置番号へ変換済み */
static uint32_t s_rl_cmd_n = 0, s_rl_wrnum = 0;
static uint8_t  s_rl_us_mx = 0;            /* 変換前の第2バイト & 7(合成エラーのST0に使う) */
static uint8_t  s_rl_rsp[8];
static uint32_t s_rl_rdptr = 0, s_rl_rdnum = 0;
static uint32_t s_rl_op_gen = 0;
static int64_t  s_rl_op_t0_us = 0, s_rl_last_rx_us = 0;
static int      s_rl_op_f0 = 0;
static uint32_t s_rl_msr_wait = 0, s_rl_other_bytes = 0;
/* READ DATA 受信・供給 */
static uint8_t  s_rl_frame[FU_FRAME_CAP];
static int      s_rl_seen_r = 0;
static uint32_t s_rl_frame_len = 0;
static uint32_t s_rl_supply_n = 0, s_rl_supply_pos = 0;
static uint8_t  s_rl_dev_result[FU_RESULT_LEN];
/* port2 パーサ(常時、構造的に解釈) */
static int      s_rl_p2_phase = 0;         /* 0=イベント 1=長さ下位 2=長さ上位 3=本体 */
static int      s_rl_p2_kind = RL_P2_NONE;
static uint8_t  s_rl_p2_len_lo = 0;
static uint32_t s_rl_p2_len = 0, s_rl_p2_got = 0;
static int      s_rl_in_poll = 0;
/* 接続・ブリングアップ・故障停止・モード表示 */
static int      s_rl_bringup_valid = 0;
static uint32_t s_rl_bringup_gen = 0;
/* P1087(D): 書込み系の異常(説明のつかないリザルト・書込み待ち打切り・送信途中失敗)で bringup を無効化した直後、
 * 次 tick を待たずゲストの次コマンド受付時に1回だけ inline で再 bringup する予約。install/reset 経路では立てない */
static int         s_rl_rebringup_pending = 0;
static uint32_t    s_rl_rebringup_gen = 0;
static const char* s_rl_rebringup_trigger = "-";
static uint32_t    s_rl_rebringup_ok = 0, s_rl_rebringup_fail = 0, s_rl_rebringup_skip = 0;
static int      s_rl_fault_valid = 0;
static uint32_t s_rl_fault_gen = 0;
static int      s_rl_gen_seen_valid = 0;
static uint32_t s_rl_gen_seen = 0;
static int      s_rl_last_mode = -1;
static const char* s_rl_last_reason = NULL;
static const char* s_rl_reason = "router_off";
/* P1026: bringup ポーリング不成立の連続回数(接続世代ごと) */
static uint32_t s_rl_bringup_poll_fail_n = 0;
static uint32_t s_rl_bringup_poll_fail_gen = 0;
/* SPECIFY ミラー */
static int      s_rl_spec_capturing = 0, s_rl_spec_n = 0, s_rl_spec_valid = 0;
static uint8_t  s_rl_spec[2];
/* READ DATA 完了直後のSENSE INTを0x80合成する(§1-b②) */
static int      s_rl_synth_after_read = 0;
/* SEEK/RECAL 保留(装置ユニット別、送信時刻。0=保留なし) */
static int64_t  s_rl_seek_t_us[4];
static uint32_t s_rl_seek_seq[4];
/* P1010: 中継RECAL/SEEK後のMSRドライブビジー(D0B〜D3B)合成。添字・ビットはゲスト側(変換前)番号。
 * 実機µPD765と同じく、SENSE INTでSE付きST0を返すまで保持する(IPLROM $FF937C が MSR&$1F==0 を待つ) */
static uint8_t  s_rl_dnb_mx = 0;
static int64_t  s_rl_dnb_t_us[4];          /* 立てた時刻(ホスト時間)。0=未設定 */
static uint32_t s_rl_dnb_gen[4];           /* 立てたときのUSB接続世代 */
static uint8_t  s_rl_dnb_udev[4];          /* 対応する装置側ユニット番号(ログ用) */
static uint32_t s_rl_dnb_busy_relay[4], s_rl_dnb_busy_core[4];
static uint32_t s_rl_dnb_set, s_rl_dnb_clr_sense, s_rl_dnb_clr_timeout, s_rl_dnb_clr_disc,
                s_rl_dnb_clr_fault, s_rl_dnb_clr_other, s_rl_dnb_busy_reads, s_rl_dnb_sense_no_se;
static uint32_t s_rl_dnb_set_lines, s_rl_dnb_clr_lines;
/* キャッシュ・重複抑止 */
static int      s_rl_sds_valid[8];         /* キー = 装置側 US|HD(下位3bit) */
static uint8_t  s_rl_sds_val[8];
static int64_t  s_rl_sds_t[8];
static int      s_rl_e5c_valid = 0;
static uint8_t  s_rl_e5c_key = 0, s_rl_e5c_val = 0;
static int64_t  s_rl_e5c_t = 0;
static int      s_rl_e5_sent_valid = 0, s_rl_e7_sent_valid = 0;
static uint8_t  s_rl_e5_sent = 0, s_rl_e7_sent = 0;
/* READ ID */
static int64_t  s_rl_rid_last_poll_us = 0;
static uint8_t  s_rl_rid_msr[16];
static uint32_t s_rl_rid_msr_n = 0, s_rl_rid_polls = 0;
/* 照合 */
static int      s_rl_verify_pending = 0, s_rl_mar0_valid = 0;
static uint32_t s_rl_mar0 = 0, s_rl_verify_n = 0, s_rl_frame_fnv = 0;
static uint8_t  s_rl_ocr0 = 0;
static int      s_rl_first_read_dumped = 0;
static uint8_t  s_rl_prev_chr[3];
static int      s_rl_prev_chr_valid = 0;
/* フレーム毎ブロック時間 */
static int64_t  s_rl_blk_frame_us = 0;
static int      s_rl_frame_active = 0;
/* P1092(D-127): ステータスバーのFD表示用(表示専用、送信判断・安全柵には使わない)。
 * 添字はMX側ドライブ番号(0-3)。装置ユニット番号から書き込む箇所は fu_status_dev_to_mx() で変換する
 * (割当てがドライブ2/3のとき base=2 で両者が異なるため)。初期値は安全側(未挿入)。
 * P1095: 書込み元は p955_route_read() case 0x05 の $E94005 読出しbit7のみ(リセットは rl_install_reset) */
static int      s_floppyu_present[4] = { 0, 0, 0, 0 };
static int      s_rl_last_io_frame[4] = { -100000, -100000, -100000, -100000 };
static inline uint8_t fu_status_dev_to_mx(uint8_t udev) { return (uint8_t)((udev + s_p955_base) & 3u); }
/* 集計(分母と分子を併記する) */
static uint32_t s_rl_relay_hist[32], s_rl_stub_hist[32], s_rl_refused_hist[32];
static uint32_t s_rl_read_begin, s_rl_read_ok, s_rl_read_err[RE_N], s_rl_rid_begin, s_rl_rid_ok, s_rl_rid_err;
static uint32_t s_rl_sds_usb, s_rl_sds_cache, s_rl_e5_usb, s_rl_e5_cache, s_rl_e5_sent_n, s_rl_e7_sent_n;
static uint32_t s_rl_c_events[RL_NSTATE], s_rl_c_unsolicited, s_rl_c_seq, s_rl_lc_events;
static uint32_t s_rl_d_events, s_rl_t_events, s_rl_w_events, s_rl_stray_frame, s_rl_p2_unknown, s_rl_frame_overflow;
static uint32_t s_rl_sense_relay, s_rl_sense_relay_80, s_rl_sense_core_fifo, s_rl_sense_synth_after_read;
static uint32_t s_rl_st0_out_of_range, s_rl_sense_drain_limit, s_rl_fifo_stale_stub, s_rl_seek_c_timeout, s_rl_ecnr_both;
static uint32_t s_rl_p1_calls, s_rl_p1_timeouts, s_rl_p1_errs, s_rl_int_raised, s_rl_bringup_n, s_rl_fault_n;
static uint32_t s_rl_blk_hist[4], s_rl_relay_frames;
static int64_t  s_rl_blk_us_max = 0;
static uint32_t s_rl_rp_e5_rd, s_rl_rp_e5_wr, s_rl_rp_e7_wr, s_rl_rp_cmd_wr, s_rl_rp_msr_rd, s_rl_rp_total;
static uint32_t s_rl_retry_same_chr, s_rl_refused_lines, s_rl_c_lines, s_rl_sds_lines, s_rl_aborted_by_cmd;
static uint32_t s_rl_result_discarded;   /* P1018: 新規コマンドで未読 RL_RESULT を破棄した回数(分母は s_p955_cmd_start) */

/* P1031: WRITE DATA dryrun(MX68K_FLOPPYU_WRITE=dryrun)。装置へは一切送らず、ゲストの書込みデータを
 * 収集・記録してから従来どおり NW エラーを返す。分母 begin = done + aborted + 進行中 */
#define P1031_TAG             "[P1031-WRDRY]"
#define WR_BUF_CAP            1024u     /* 128<<3。N>3 は事前条件で拒否する */
/* P1032/P1035: MX68K_FLOPPYU_WRITE の値。"dryrun"→DRYRUN、"wponly"→WPONLY、"1"→WRITE(段階1、柵つき)、それ以外→OFF
 * P1037: "armed"→ARMED(段階2a、武装マーカー付き使い捨てディスク限定の連続書込み)
 * P1053: "normal"→NORMAL(段階2b-1、武装なしの通常利用。交換後READ柵+同一キー連続書込み検知+読み戻し照合)
 * P1064: "fmt1"→FMT1(単一トラック限定の実FORMAT送信。WRITE DATA は装置へ送らない) */
enum { WRM_OFF = 0, WRM_DRYRUN = 1, WRM_WPONLY = 2, WRM_WRITE = 3, WRM_ARMED = 4, WRM_NORMAL = 5, WRM_FMT1 = 6 };
static int      s_rl_write_mode = WRM_OFF;
/* P1054: 書込みモードの由来と設定値。s_rl_write_cfg は install 直前に EmulatorBridge.c から渡される */
enum { RL_MSRC_OFF = 0, RL_MSRC_ENV = 1, RL_MSRC_CONFIG = 2 };
static const char* const RL_MSRC_NAME[3] = { "off", "env", "config" };
static int      s_rl_write_cfg = 0;
static int      s_rl_mode_src = RL_MSRC_OFF;
static int      s_rl_cfg_ignored = 0;
static uint32_t s_rl_cfg_lines = 0;
#define P1054_CFG_TAG         "[P1054-CFG]"
static uint8_t  s_rl_wrcollect_buf[WR_BUF_CAP];
static uint32_t s_rl_wrcollect_pos = 0, s_rl_wrcollect_n = 0;
static int      s_rl_wrcollect_f0 = 0;
static int      s_rl_wrcollect_mar0_valid = 0;
static uint32_t s_rl_wrcollect_mar0 = 0;
static uint16_t s_rl_wrcollect_mtc0 = 0;
static uint8_t  s_rl_wrcollect_ocr0 = 0, s_rl_wrcollect_csr0 = 0;
static uint32_t s_rl_wrcollect_begin, s_rl_wrcollect_done, s_rl_wrcollect_aborted, s_rl_wrcollect_refused;
static uint32_t s_rl_wrcollect_lines;
/* P1100: WR_COLLECT 段階の切り分け診断(挙動変更なし、読取り専用)。DMA_Exec(0) 1回あたりの MTC 減少量を数え、
 * 1回の呼出しで1Bしか転送されない(候補b)か、無転送の呼出しが多い(候補a/c)かを区別する */
#define P1100_TAG                 "[P1100-WRCOLL]"
static int      s_p1100_active = 0;      /* 収集開始〜集計行出力まで1 */
static int      s_p1100_is_fmt = 0;      /* 1=FORMAT 経由の収集(rl_fmt_collect_begin) */
static int      s_p1100_f0 = 0, s_p1100_vl0 = 0;     /* 収集開始時の frame/VLINE */
static uint8_t  s_p1100_ccr0 = 0, s_p1100_csr0 = 0;  /* 収集開始時の DMA[0].CCR/CSR */
static uint16_t s_p1100_mtc0 = 0;                    /* 収集開始時の DMA[0].MTC */
static int      s_p1100_f1 = 0, s_p1100_vl1 = 0;     /* 1バイト目(rl_collect_byte pos==0) */
static int      s_p1100_fn = 0, s_p1100_vln = 0;     /* 最後の1バイト(rl_collect_done 冒頭) */
static int      s_p1100_calls = 0, s_p1100_calls_moved = 0, s_p1100_calls_zero = 0;
static uint16_t s_p1100_max_delta = 0;
static uint16_t s_p1100_last_mtc = 0;
static uint32_t s_p1100_n_total = 0;     /* 集計行を出した総収集件数(分母) */
static unsigned long long s_p1100_zero_total = 0;   /* 完了した収集の無転送呼出しの累計 */
static int      s_p1100_lines = 0;       /* 出力済み集計行の件数(最初の32件まで出力) */
static void p1100_begin(int is_fmt)
{
    s_p1100_active = 1;
    s_p1100_is_fmt = is_fmt;
    s_p1100_f0 = g_mx68k_frame_num;
    s_p1100_vl0 = VLINE;
    s_p1100_ccr0 = DMA[0].CCR;
    s_p1100_csr0 = DMA[0].CSR;
    s_p1100_mtc0 = DMA[0].MTC;
    s_p1100_last_mtc = DMA[0].MTC;
    s_p1100_calls = 0; s_p1100_calls_moved = 0; s_p1100_calls_zero = 0; s_p1100_max_delta = 0;
}
/* P1063: FORMAT TRACK(WRITE ID, cmd 13)の dryrun 限定観測。ID表(SC×4B)を上の収集機構で受け取り、
 * 装置へは一切送らず成功リザルトを合成する。dryrun 以外のモードでは従来どおり無条件拒否 */
#define P1063_TAG             "[P1063-FMT]"
static int      s_rl_wr_is_format = 0;    /* 収集中の内容が FORMAT の ID 表か(rl_start で毎回クリア) */
static uint32_t s_rl_fmt_dryrun_n = 0;    /* dryrun FORMAT の受付回数(自己反証可能性の分母、sum 行へ出す) */
static uint32_t s_rl_fmt_lines = 0;       /* [P1063-FMT] 行の出力数(FU_LOG_LIMIT で打切り) */

/* P1064: MX68K_FLOPPYU_WRITE=fmt1。許可したトラック(C=0 限定)だけ WRITE ID を装置へ送る。
 * 送信前に装置へ触れない照合(Phase A)→非キャッシュ SDS(WP/RY/T0)と同期 READ ID(Phase B)→再検査(C0)→送信。
 * ヘッドを動かすコマンドは一切挿入しない(rl_wr_sync_seek は呼ばない)。記号・定数の根拠は .mx68k_cycles/P1064_plan.md。
 * P1066: MX68K_FLOPPYU_FORMAT_CYL=0-0 なら C0 の2面(C0H0→C0H1)を、前トラックの検証 ok を条件に連続送信する
 * (複数トラックモード。根拠は .mx68k_cycles/P1066_plan.md) */
#define P1064_TAG                 "[P1064-FMT1]"
#define FU_FMT1_SEND_MAX          1u        /* 単一トラックモードの1プロセスあたり実送信上限(s_rl_fmt1_spent と二重化) */
#define FU_FMT1_SEND_HARD_MAX     2u        /* P1066: 複数トラックモードを含む送信回数の絶対上限(C0 の面数) */
#define FU_FMT_RID_DEADLINE_MS    500       /* 同期 READ ID の結果フェーズ待ち上限(≒3回転、ファームの2秒ウォッチドッグ比0.25) */
#define FU_FMT_RID_POLL_SLEEP_US  FU_SYNC_POLL_SLEEP_US   /* READ ID 待ち中の装置MSR問合せ間隔 */
#define FU_FMT_DRAIN_MS           150       /* READ ID 後の'C'/'c'吸収窓(Fable5 C-1) */
#define FU_FMT1_W_WAIT_MS         50        /* 送信直後の同期'W'待ちの上限(Fable5 C-2) */
#define FU_FMT1_VERIFY_N          8u        /* 送信成功後の検証 READ ID 回数(SC=8) */
#define ST3_T0                    0x10u     /* Inside X68000 p.418 図34: Track 0 信号 */
/* 柵(環境変数、install ごとにまず全拒否側へ戻し、fmt1 のときだけ解析する) */
static int      s_rl_fm_env_ok = 0;
static int      s_rl_fm_unit = -1;         /* 許可する装置側ユニット(0..3) */
static uint8_t  s_rl_fm_c = 0;
static uint8_t  s_rl_fm_hmask = 0;         /* P1066: 許可する面のマスク(単一: 1<<h、複数: 0x3) */
static int      s_rl_fm_multi = 0;         /* P1066: 複数トラックモード(MX68K_FLOPPYU_FORMAT_CYL=0-0) */
/* ★s_rl_fmt1_spent は「1回でも送信を試みた」。ここの static 初期化子でのみ 0 にする(プロセス起動時のみ)。送信関数を呼ぶ前に
 *   1を立て、以後どの関数でも 0 に戻さない。P1066: 面ごとの送信済みは s_rl_fmt1_spent_hmask が持つ(同じく0に戻さない) */
static int      s_rl_fmt1_spent = 0;
static int      s_rl_fmt1_inflight = 0;    /* 送信した FORMAT のリザルト待ち中(リザルト判定・打切りで0) */
/* P1066: 以下は static 初期化子でのみ 0 にする(rl_install_reset でもリセットしない、プロセス寿命) */
static uint8_t     s_rl_fmt1_spent_hmask = 0;     /* 送信を試みた面のマスク(送信関数を呼ぶ前に立てる) */
static int         s_rl_fmt1_halt = 0;            /* 永久停止ラッチ(解除経路なし) */
static const char* s_rl_fmt1_halt_why = "-";      /* 最初の停止理由を保持する */
static int         s_rl_fmt1_chain_ok = 0;        /* 直前に送信したトラックの検証が ok だったか(送信直前に0、検証 ok で1) */
static uint32_t    s_rl_fmt1_chain_d0 = 0, s_rl_fmt1_chain_gen0 = 0;   /* 最初の送信時の s_rl_wr_d_begin / s_rl_wr_gen_begin */
static int         s_rl_fmt1_chain_set = 0;
static uint8_t     s_rl_fmt1_sent_h = 0;          /* 直前に送信した面 */
static uint8_t     s_rl_fmt1_v_h = 0;             /* 検証対象の面(ok_formatted 時に s_rl_fmt1_sent_h を写す) */
/* 拒否理由。P1066 の追加分は末尾(FM_REF_N の直前)に置き、既存の値を動かさない。列挙と FM_REF_NAME は同じ順で追記する */
enum { FM_REF_LATCHED = 0, FM_REF_ENV, FM_REF_UNIT, FM_REF_HEAD, FM_REF_PARAM, FM_REF_IDS, FM_REF_SEND_MAX, FM_REF_COLLECT,
       FM_REF_LINK, FM_REF_REENTRY, FM_REF_SDS_FAIL, FM_REF_NO_MEDIA, FM_REF_WP, FM_REF_T0, FM_REF_ST3_OTHER,
       FM_REF_RID_TIMEOUT, FM_REF_RID_IC, FM_REF_RID_C_WITHOUT_CC, FM_REF_RID_NO_C, FM_REF_RID_CHS, FM_REF_RECHECK,
       FM_REF_SPENT, FM_REF_HALT, FM_REF_CHAIN, FM_REF_CHAIN_EPOCH, FM_REF_ORDER, FM_REF_N };
static const char* const FM_REF_NAME[] = {
    "refused_latched", "refused_fence_env", "refused_fence_unit", "refused_fence_head", "refused_fence_param",
    "refused_fence_ids", "refused_send_max", "refused_collect", "refused_link", "refused_reentry", "refused_sds_fail",
    "refused_no_media", "refused_wp", "refused_t0", "refused_st3_other", "refused_rid_timeout", "refused_rid_ic",
    "refused_rid_c_without_cc", "refused_rid_no_c", "refused_rid_chs", "refused_recheck", "refused_spent",
    "refused_halt", "refused_chain", "refused_chain_epoch", "refused_order" };
_Static_assert(sizeof(FM_REF_NAME) / sizeof(FM_REF_NAME[0]) == FM_REF_N, "FM_REF_NAME と FM_REF_* の要素数が一致しない");
static uint32_t s_rl_fmt1_ref[FM_REF_N];
static uint32_t s_rl_fmt1_seen, s_rl_fmt1_ok, s_rl_fmt1_nw, s_rl_fmt1_n_unexp, s_rl_fmt1_invalid;
/* 送信成功後の検証 READ ID(送信時の世代・'D'/'d'受信数・SEEK数が不変のときだけ1回) */
static uint32_t s_rl_fmt1_seek_n = 0;      /* rl_seek() で全ユニット共通に+1 */
static uint32_t s_rl_fmt1_send_seek_n = 0; /* 送信時点の s_rl_fmt1_seek_n */
static int      s_rl_fmt1_verify_pending = 0;
static uint32_t s_rl_fmt1_v_gen = 0, s_rl_fmt1_v_d = 0, s_rl_fmt1_v_seek = 0;
static uint32_t s_rl_fmt1_verify_runs, s_rl_fmt1_verify_ok, s_rl_fmt1_verify_bad, s_rl_fmt1_verify_skipped;
/* 拒否後の迷子'C'の計数(Fable5 F-8、挙動は変えない) */
static int      s_rl_fmt1_stray_pending = 0;
static uint32_t s_rl_fmt1_stray_c_fwd = 0;
/* P1065: 検証を実行した起動点別の回数(SEEK/RECAL 中継直前 / Phase 0)、吸収窓の後も'C'が不足した回数、検証後に中継不可だった回数 */
static uint32_t s_rl_fmt1_verify_at_seek = 0, s_rl_fmt1_verify_at_fmt = 0;
static uint32_t s_rl_fmt1_c_deficit_n = 0, s_rl_fmt1_relay_lost_n = 0;
static uint32_t s_rl_fmt1_pre_lines = 0, s_rl_fmt1_lines = 0;   /* precheck 行 / それ以外の行(各 FU_AR_LOG_LIMIT) */
#ifdef MX68K_FLOPPYU_USB_SELFTEST
static int      s_st_fmt1_force_ret = 0;   /* 単体テスト専用: 0 以外なら次の1回の送信を装置へ送らずこの戻り値とする */
#endif

/* P1067: normal モード(設定 UI 由来)の FORMAT(WRITE ID)。設定「Allow FORMAT on FloppyU disks」が ON のときだけ、
 * 柵が開いたユニットの物理フォーマット済み標準2HD(1024B×8)ディスクへ送る。fmt1 の状態変数・関数本体には手を入れず、
 * 状態は s_nmf_* として別に持つ。記号・定数の根拠は .mx68k_cycles/P1067_plan.md 第3版。
 * 位置の根拠: C≥1 で物理位置を示す証拠は READ ID(媒体に記録された C/H)だけ。PCN==ID表C はゲストが直前に SEEK した
 * 意図との整合検査(PCN は装置 FDC のソフトウェアカウンタでヘッドの物理位置を測らない)、T0 は C=0 以外では矛盾検出のみ */
#define P1067_TAG                 "[P1067-NMF]"
#define FU_NMF_RUNS_PER_EPOCH     2u        /* 同一ユニット・同一エポックで開始してよい走行数(走行開始 C0H0 時にのみ判定) */
#define FU_NMF_MAX_CYL            76u       /* 2HD のシリンダ 0..76(FU_AR_NCYL-1) */
#define FU_NMF_LAST_POS           153u      /* 走行の最終位置 k=2C+H(C76H1) */
enum { NMF_SRC_OFF = 0, NMF_SRC_CONFIG = 1, NMF_SRC_ENV = 2, NMF_SRC_IGNORED = 3 };
static const char* const NMF_SRC_NAME[4] = { "off", "config", "env", "ignored" };
enum { NMF_VSRC_SEEK = 0, NMF_VSRC_FMT, NMF_VSRC_WRITE };
static const char* const NMF_VSRC_NAME[3] = { "seek", "fmt", "write" };
/* 拒否理由(全理由を sum 行へ 0 件も含めて出す)。列挙と NMF_REF_NAME は同じ順 */
enum { NMF_REF_MODE = 0, NMF_REF_OFF, NMF_REF_SC_ZERO, NMF_REF_HALT, NMF_REF_CHAIN, NMF_REF_LATCHED, NMF_REF_UNIT,
       NMF_REF_FENCE, NMF_REF_PARAM, NMF_REF_IDS, NMF_REF_CYL, NMF_REF_RUN_OTHER_UNIT, NMF_REF_RUN_START, NMF_REF_RUN_MAX,
       NMF_REF_ORDER, NMF_REF_COLLECT, NMF_REF_LINK, NMF_REF_NO_PCN, NMF_REF_PCN, NMF_REF_REENTRY, NMF_REF_SDS_FAIL,
       NMF_REF_NO_MEDIA, NMF_REF_WP, NMF_REF_T0, NMF_REF_ST3_OTHER, NMF_REF_RID_TIMEOUT, NMF_REF_RID_IC,
       NMF_REF_RID_C_WITHOUT_CC, NMF_REF_RID_NO_C, NMF_REF_RID_CHS, NMF_REF_RID_N, NMF_REF_RECHECK, NMF_REF_N };
static const char* const NMF_REF_NAME[] = {
    "refused_mode", "refused_off", "refused_sc_zero", "refused_halt", "refused_chain", "refused_latched",
    "refused_fence_unit", "refused_fence_closed", "refused_param", "refused_ids", "refused_cyl", "refused_run_other_unit",
    "refused_run_start", "refused_run_max", "refused_order", "refused_collect", "refused_link", "refused_no_pcn",
    "refused_pcn", "refused_reentry", "refused_sds_fail", "refused_no_media", "refused_wp", "refused_t0",
    "refused_st3_other", "refused_rid_timeout", "refused_rid_ic", "refused_rid_c_without_cc", "refused_rid_no_c",
    "refused_rid_chs", "refused_rid_n", "refused_recheck" };
_Static_assert(sizeof(NMF_REF_NAME) / sizeof(NMF_REF_NAME[0]) == NMF_REF_N, "NMF_REF_NAME と NMF_REF_* の要素数が一致しない");
static int      s_rl_format_cfg = 0;       /* 設定値(install 直前に EmulatorBridge.c から渡される) */
static int      s_nmf_on = 0;              /* install で確定: normal かつ(設定由来+設定 ON、または env 経路) */
static int      s_nmf_src = NMF_SRC_OFF;
/* ★以下の走行・停止・検証・PCN の状態は static 初期化子でのみ 0 にする(rl_install_reset でも戻さない、プロセス寿命)。
 *   例外: s_nmf_inflight は送信後処理・打切りで 0、PCN 記録は無効化契機(§V-3b)で valid=0 */
static int         s_nmf_inflight = 0;     /* 送信した normal FORMAT のリザルト待ち中(モード非依存の判別に使う、§V-1b) */
static int         s_nmf_halt = 0;         /* 停止ラッチ(解除経路なし) */
static const char* s_nmf_halt_why = "-";
static int         s_nmf_partial = 0;      /* 走行中に停止した(ディスクが半端に初期化された可能性) */
static int         s_nmf_run_active = 0;   /* 走行中(C0H0 送信で開始、C76H1 検証 ok で完走) */
static uint8_t     s_nmf_run_us = 0;       /* 走行中の装置ユニット */
static uint32_t    s_nmf_run_pos = 0;      /* 次に期待する位置 k=2C+H */
static uint32_t    s_nmf_run_ue = 0;       /* 走行開始時の ue[run_us] */
static uint32_t    s_nmf_run_n[4];         /* エポック内で開始した走行数(ユニット別) */
static int         s_nmf_ep_valid[4];
static uint32_t    s_nmf_ep_ue[4];         /* run_n を数えているエポック(ue[us]) */
static int         s_nmf_chain_ok = 0;     /* 直前に送信したトラックの検証が ok(c_deficit_final==0 込み)だったか */
static uint8_t     s_nmf_sent_us = 0, s_nmf_sent_c = 0, s_nmf_sent_h = 0;
static uint32_t    s_nmf_sent_k = 0, s_nmf_sent_ue = 0, s_nmf_sent_gen = 0;
static int         s_nmf_verify_pending = 0;
static uint8_t     s_nmf_v_us = 0, s_nmf_v_c = 0, s_nmf_v_h = 0;
static uint32_t    s_nmf_v_k = 0, s_nmf_v_ue = 0, s_nmf_v_gen = 0;
/* PCN 記録(装置ユニット別)。装置経由のゲスト SENSE INT で IC=00・SE=1 のときだけ記録する */
static int         s_nmf_pcn_valid[4];
static uint8_t     s_nmf_pcn[4], s_nmf_pcn_st0[4];
static uint32_t    s_nmf_pcn_ue[4], s_nmf_pcn_gen[4];
static int         s_nmf_pcn_f[4];
/* 集計(分母と分子を併記) */
static uint32_t    s_nmf_seen, s_nmf_ref[NMF_REF_N], s_nmf_sends, s_nmf_ok, s_nmf_nw, s_nmf_n_unexp, s_nmf_invalid,
                   s_nmf_tracks_ok, s_nmf_verify_runs, s_nmf_verify_ok, s_nmf_verify_bad, s_nmf_verify_skipped,
                   s_nmf_verify_at[3], s_nmf_c_deficit_n, s_nmf_relay_lost_n, s_nmf_runs_started, s_nmf_runs_completed,
                   s_nmf_runs_aborted, s_nmf_pcn_rec, s_nmf_pcn_inval_seek, s_nmf_pcn_inval_sense, s_nmf_pcn_inval_bringup,
                   s_nmf_pcn_inval_sync, s_nmf_pcn_inval_all, s_nmf_map_inval, s_nmf_wr_refused;
static uint32_t    s_nmf_wr_mt = 0, s_nmf_wr_multi = 0, s_nmf_wr_nbig = 0;   /* WRITE DATA の受付前拒否の内訳(R-g2 観測用) */
/* 行種別ごとの予算(各 FU_AR_LOG_LIMIT)。1走行=154トラックで precheck/result/verify 各154行、sent+wsync 308行 */
static uint32_t    s_nmf_pre_lines = 0, s_nmf_lines = 0, s_nmf_res_lines = 0, s_nmf_v_lines = 0, s_nmf_pcn_lines = 0;
/* P1070: FORMAT.X は走行(C0H0..C76H1)の後に C77H0 を1回余分に要求することがある(形式テーブル上限154 → 155回目、
 * ふろっぴU君経由の実測は 154回[P1063]と 155回[P1067/P1069/P1070 H-0]の両方があり差の原因は未解明)。
 * XEiJ(FDC.java:3365)・XM6本家(fdi.cpp:3739)と同じく、装置へ送らず正常終了で返す(吸収)。MX は「直前に完走した走行の
 * 直後の1回の C77H0」だけに限る。記号・条件の根拠は .mx68k_cycles/P1070_plan.md 第3版 */
#define FU_NMF_ABSORB_CYL         77u       /* = FU_NMF_MAX_CYL+1(形式テーブル上限154 → C=154>>1=77, H=154&1=0) */
#define FU_NMF_C77_LOG_LIMIT      64u
#define P1070_TAG                 "[P1070-NMF]"
_Static_assert(FU_NMF_ABSORB_CYL == FU_NMF_MAX_CYL + 1u, "FU_NMF_ABSORB_CYL は FU_NMF_MAX_CYL+1");
/* 直前に完走した走行の記録(完走直後の FORMAT 1回だけ有効。停止・normal WRITE DATA・install・同ユニットの目標≠77 の
 * SEEK/RECAL・次の FORMAT で失効、§V-3 X1〜X5) */
static int         s_nmf_ld_valid = 0;
static uint8_t     s_nmf_ld_us = 0;
static uint32_t    s_nmf_ld_ue = 0, s_nmf_ld_gen = 0, s_nmf_ld_runs = 0;
static int         s_nmf_ld_f = 0;
static uint32_t    s_nmf_c77_seen = 0, s_nmf_c77_cand = 0, s_nmf_absorbed = 0, s_nmf_c77_lines = 0;
/* 失効点別の計数(有効な記録を実際に消した回数だけ数える) */
static uint32_t    s_nmf_ld_exp_write = 0, s_nmf_ld_exp_seek = 0, s_nmf_ld_exp_install = 0, s_nmf_ld_exp_halt = 0,
                   s_nmf_ld_exp_fmt = 0;
static void rl_nmf_halt(const char* why);
#ifdef MX68K_FLOPPYU_USB_SELFTEST
static int      s_st_nmf_force_ret = 0;    /* 単体テスト専用: 次の1回の normal FORMAT 送信を装置へ送らずこの戻り値とする */
#endif

/* P1032: WRITE DATA の wponly 限定実送信(MX68K_FLOPPYU_WRITE=wponly)。送信直前の非キャッシュ SDS で
 * FT=0・WP=1・RY=1・US/HD一致を確認できたときだけ装置へ書込みコマンドを送る。それ以外は dryrun と同じ NW。
 * 記号・定数の根拠は .mx68k_cycles/P1032_plan.md の記号表 */
#define P1032_TAG                 "[P1032-WRSEND]"
/* P1033: $E94005 送信値の限定ログ(排出 bit5 がゲスト起点かの可視化) */
#define P1033_E5_TAG              "[P1033-E5W]"
#define FU_REG_MTC_LO             0x30u     /* 装置側MTCレジスタ(XM6t fdc_bridger.h:808-812、XEiJ FUK.java:790-791) */
#define FU_REG_MTC_HI             0x31u
#define FU_P2_DATA_START          0x4Du     /* 'W'応答の転送開始識別子(XM6t fdc_bridger.h:955、XEiJ FUK.java:981) */
#define FU_WR_EVENT_DEADLINE_MS   3000      /* 送信後、'W'/'w'/'C'のいずれも来ない上限(MX独自値) */
#define FU_WR_DONE_DEADLINE_MS    3000      /* データ送信後、'w'/'C'が来ない上限(MX独自値) */
#define FU_WR_RESULT_DEADLINE_MS  3000      /* 'C'後('w'無し)、装置MSR=RQM|DIOにならない上限(MX独自値)。'w'経路はMSRを待たない */
#define FU_P2_WRITE_TIMEOUT_MS    500       /* port2 の 1+n バイト同期送信の上限(要実測) */
#define FU_WR_SEND_MAX            8u        /* 1プロセスあたりの実送信回数上限(MX独自の安全上限) */
#define FU_WR_SDS_RQM_POLL_MAX    16u       /* 非キャッシュSDSで結果フェーズになるまでの装置MSR問合せ上限 */
#define FU_WR_EV_MAX              16u       /* 書込み待ち中のイベント時系列の記録上限 */
#define FU_WR_LATE_C_WINDOW_MS    500       /* 'w'で結果確定後、遅着'C'として数える時間窓(MX独自値) */
#define ST3_FT                    0x80u     /* µPD765 仕様 [training knowledge] */
#define ST3_WP                    0x40u     /* Core:x68k/fdc.c:223 */
#define ST3_RY                    0x20u     /* Core:x68k/fdc.c:222 */
#define ST1_OR                    0x10u     /* 送信後異常の合成値(ゲストに対してはエラーであれば足りる) */
/* ★s_rl_wr_sends と s_rl_wr_latched_off はここの static 初期化子でのみ 0 にする(プロセス起動時のみ)。
 *   rl_install_reset() を含め他のどの関数でも 0 に戻さないこと——ソフトリセット・機種切替のハードリセットで
 *   送信回数上限やラッチが解除されないようにするため(Fable5 M3)。
 *   P1037: armed の状態(s_rl_ar_armed・s_rl_ar_arm_used・s_rl_ar_arm_gen・s_rl_ar_arm_f・s_rl_ar_disarm・
 *   s_rl_ar_map と集計値)も同じ規律で、プロセス起動時以外に初期化し直さない(例外は環境変数の解析結果のみ) */
static uint32_t s_rl_wr_sends = 0;
static int      s_rl_wr_latched_off = 0;
static uint16_t s_rl_wr_mtc_begin = 0;     /* 収集開始時の DMAC MTC */
static uint32_t s_rl_wr_gen_begin = 0;     /* 収集開始時の USB 接続世代 */
static uint32_t s_rl_wr_d_begin = 0;       /* 収集開始時の 'D'/'d' 受信数 */
static uint32_t s_rl_rd_d_begin = 0;       /* P1038: READ 発行時の 'D'/'d' 受信数(武装判定の基準点) */
static uint32_t s_rl_rd_gen_begin = 0;     /* P1053: READ 発行時の USB 接続世代(normal の柵解除の基準点) */
static int      s_rl_wr_data_sent = 0;     /* 当該コマンドで 'W' 応答を送ったか(戻り値に関わらず1回限り) */
static uint32_t s_rl_wr_n = 0;             /* 当該コマンドの送信データ長 */
static int64_t  s_rl_wr_state_t_us = 0;    /* 現在の書込み待ち状態へ入った時刻(タイムアウト判定用) */
static int64_t  s_rl_wr_send_t_us = 0;     /* 書込みコマンド送信時刻(イベント時系列の基準) */
static uint32_t s_rl_wr_c_seen = 0;        /* 当該コマンドの書込み待ち中に受けた 'C' 数 */
static int64_t  s_rl_wr_result_t_us = 0;   /* 'C'無しで結果確定した時刻(遅着'C'の計数用、0=対象外) */
static int      s_rl_wr_op_f0 = 0;         /* P1099(診断専用): 書込み/FORMAT専用のコマンド受信フレーム番号
                                            * (共有変数s_rl_op_f0はREAD DATA/READ IDからも書換えられるため別に持つ) */
static int      s_rl_wr_data_sent_f = 0;   /* P1099(診断専用): データ送信時のフレーム番号 */
static int      s_rl_wr_result_f = 0;      /* P1099(診断専用): 結果確定時のフレーム番号 */
static char     s_rl_wr_ev_c[FU_WR_EV_MAX];
static int64_t  s_rl_wr_ev_us[FU_WR_EV_MAX];
static uint32_t s_rl_wr_ev_n = 0;
static uint8_t  s_rl_wr_res_msr[16];
static uint32_t s_rl_wr_res_msr_n = 0, s_rl_wr_res_polls = 0;
static int64_t  s_rl_wr_res_last_poll_us = 0;
static uint8_t  s_rl_wr_p2buf[1u + WR_BUF_CAP];
/* 集計(分母と分子を併記する) */
static uint32_t s_rl_wr_checks, s_rl_wr_ref_writable, s_rl_wr_ref_no_media, s_rl_wr_ref_unknown, s_rl_wr_ref_latched;
static uint32_t s_rl_wr_late_c, s_rl_wr_ev_W, s_rl_wr_ev_w, s_rl_wr_ev_C, s_rl_wr_ev_t, s_rl_wr_ev_D;
static uint32_t s_rl_wr_data_sent_n, s_rl_wr_late_w, s_rl_wr_results, s_rl_wr_timeouts;
static uint32_t s_rl_wr_invalid;   /* P1034: 装置リザルト7Bを受領したが妥当性検査で不採用にした件数 */
static uint32_t s_rl_wr_supp_e5r, s_rl_wr_supp_e5w, s_rl_wr_supp_e7w;
static uint32_t s_rl_wr_pre_lines, s_rl_wr_lines;
/* P1033: E5W ログ専用の行数カウンタ(書込み診断行 s_rl_wr_lines の予算を消費しないよう分離) */
static uint32_t s_rl_e5w_lines = 0;

/* P1035: MX68K_FLOPPYU_WRITE=1(mode1)段階1。WP=0 の書込み可能ディスクへも送るが、WPセンサーと独立した柵
 * (送信上限1回/プロセス・許可ユニット必須・許可セクタ必須・読み戻しベリファイ)を全て満たした場合のみ。
 * 記号・定数の根拠は .mx68k_cycles/P1035_plan.md の記号表 */
#define P1035_TAG                 "[P1035-WRMODE1]"
#define P1035_VERIFY_TAG          "[P1035-WRVERIFY]"
#define FU_WR_SEND_MAX_WRITE      1u        /* mode1 段階1の実送信上限(1プロセス1回、MX独自)。拡大は段階2で */
/* mode1 の柵(環境変数、install ごとに解析。未設定・不正は全拒否) */
static int      s_rl_wf_env_ok = 0;        /* UNIT・CHR とも妥当に解析できたか */
static int      s_rl_wf_unit = -1;         /* 許可する装置側ユニット(0..3) */
static uint8_t  s_rl_wf_chrn[4];           /* 許可する C,H,R,N */
static int      s_rl_wr_wp_at_send = 1;    /* 送信直前SDSで観測したWP(1=WP)。classify の期待値選択用。既定1=NW期待側 */
/* 読み戻しベリファイ(ログ機械照合) */
static int      s_rl_wv_pending = 0;       /* mode1・WP=0 で ok_* 採用後、同一ユニット・CHRN の READ を待つ */
static uint8_t  s_rl_wv_us, s_rl_wv_chrn[4];
static uint32_t s_rl_wv_fnv = 0;           /* 送信データFNV(= [P1032-WRSEND] data の data_fnv) */
static const char* s_rl_wv_outcome = "-";
static int      s_rl_wv_pre_valid = 0;     /* 書込み前に許可セクタを読んだFNVを記録済みか */
static uint32_t s_rl_wv_pre_fnv = 0;
/* 読出し開始時のCHRN/ユニット(read end ログ用。後続コマンドで s_rl_cmd が上書きされても正しい値を出すため) */
static uint8_t  s_rl_rd_chrn[4], s_rl_rd_us = 0;
/* 集計(分母と分子を併記) */
static uint32_t s_rl_m1_ref_env, s_rl_m1_ref_unit, s_rl_m1_ref_chr, s_rl_m1_ref_max, s_rl_m1_pass;
static uint32_t s_rl_m1_ok_written, s_rl_m1_ok_chrn_unexp, s_rl_m1_fence_reads_ok;
static uint32_t s_rl_wv_match, s_rl_wv_mismatch, s_rl_m1_lines, s_rl_wv_lines;

/* P1051: mode1/armed の書込み全同期化。装置ファーム(app.c v0.81)は SEEK/RECAL/READ 系の後の最初の WRITE だけを
 * 実FDCで同期実行し、同じコマンドバイトの2回目以降は RAM キャッシュ+合成リザルトで即答する(.mx68k_cycles/P1050_code_inv.md)。
 * そこで送信直前に同一シリンダの SEEK+SENSE INT を挟み、毎回同期経路に乗せる。記号・定数の根拠は .mx68k_cycles/P1051_plan.md */
#define P1051_TAG                 "[P1051-SYNC]"
#define FU_SYNC_C_WAIT_MS         50        /* SEEK 後の SIS 発火待ち(port2 の'C'、無ければこの上限で SIS へ)。MX独自値 */
#define FU_SYNC_SIS_MAX           4         /* Ready 変化 ST0(IC=11)を読み捨てる分を含む SIS の最大回数。MX独自値 */
#define FU_SYNC_DRAIN_MS          30        /* SIS 後の'c'吸収窓。MX独自値 */
#define FU_SYNC_POLL_SLEEP_US     200       /* 同期手順内で port2 を読む間隔 */
#define FU_SYNC_ST0_LIST_MAX      8u
static int      s_rl_sync_active = 0;       /* 同期手順中のみ1。rl_on_c は'C'を吸収し、'c'を数える */
static uint32_t s_rl_sync_c = 0, s_rl_sync_cc = 0;
/* would_cache 判別用の反事実モデル(装置が P1051 の SEEK 挿入無しでキャッシュ経路を選んだか、app.c@1503)。
 * cf_first: MX が装置へ実際に送った RECAL/SEEK/READ DATA で1(P1051 が挿入する SEEK は除外)、
 * 同期書込みの7B採用で0。cf_cmd: 最後に採用した書込みのコマンドバイト */
static int      s_rl_cf_first = 1;
static uint8_t  s_rl_cf_cmd = 0;
static uint32_t s_rl_sy_ok, s_rl_sy_refused, s_rl_sy_would_cache_n;
static uint32_t s_rl_sy_form_a, s_rl_sy_form_c, s_rl_sy_form4, s_rl_sy_form_other;
static uint32_t s_rl_sy_lines;

/* P1037: MX68K_FLOPPYU_WRITE=armed(段階2a)。使い捨てディスクの最終セクタ C76H1R8 に段階1で書いた武装マーカーを
 * ゲストが読み、全1024B一致した後だけ、許可ユニット・許可シリンダ範囲の単一セクタ書込みを最大256回/プロセス送る。
 * ディスク交換の疑い('D'/'d'・排出ビット・RY=0・USB世代変化・読み戻し不一致・想定外'W'・リセット)は全て永久停止
 * (再武装は MX の再起動のみ)。記号・定数の根拠は .mx68k_cycles/P1037_plan.md の記号表 */
#define P1037_ARM_TAG             "[P1037-WRARM]"
#define P1037_TAG                 "[P1037-WRARMED]"
#define P1037_VERIFY_TAG          "[P1037-WRVERIFY]"
#define P1037_MAP_TAG             "[P1037-WRMAP]"
#define FU_WR_SEND_MAX_ARMED      256u      /* 段階2a: 1プロセスあたりの実送信上限(暴走ブレーキ。P1036 実測194回+余裕) */
#define FU_AR_NCYL                77u       /* 2HD 形状(77: [training knowledge]、XDF 1,261,568B と整合) */
#define FU_AR_NHEAD               2u
#define FU_AR_NSEC                8u        /* 実IPLROM N別表 $FF9468(P1036 Spec Inv §1) */
#define FU_AR_SECLEN              1024u
#define FU_AR_NENT                (FU_AR_NCYL * FU_AR_NHEAD * FU_AR_NSEC)   /* 1232 */
#define FU_AR_LOG_LIMIT           1024u     /* w・r・verify・arm 行それぞれ別カウンタでこの上限(Fable5 R3) */
static const uint8_t FU_AR_MARK_CHRN[4] = { 76u, 1u, 8u, 3u };   /* 2HD 最終セクタ(P1035 H-3 と同じセクタ) */
static const char FU_AR_MARK_HEAD[] = "MX68K-FLOPPYU-WRITE-ARMED-DISK-1";   /* 32文字(マーカー先頭32B) */
/* 柵(環境変数、install ごとに解析。未設定・不正は全拒否) */
static int      s_rl_ar_env_ok = 0, s_rl_ar_unit = -1;
static uint8_t  s_rl_ar_cmin = 0, s_rl_ar_cmax = 0;
/* 武装状態(プロセス寿命) */
static int      s_rl_ar_armed = 0;          /* 1=武装中 */
static int      s_rl_ar_arm_used = 0;       /* 1=このプロセスで武装を一度行った(再武装不可) */
static uint32_t s_rl_ar_arm_gen = 0;
static int      s_rl_ar_arm_f = -1;
static const char* s_rl_ar_disarm = "-";    /* 永久ラッチの理由(最初の1件) */
static uint8_t  s_rl_ar_marker[FU_AR_SECLEN];
static uint32_t s_rl_ar_marker_fnv = 0, s_rl_ar_fill_fnv = 0;
static int      s_rl_ar_marker_built = 0;
/* セクタ別読み戻し照合表(ユニット1台分) */
typedef struct {
    uint32_t last_fnv, pre_fnv;
    uint16_t writes, vmatch, vmismatch;
    uint8_t  written, pending, pre_valid, verified, uncertain;
} rl_ar_sec_t;
static rl_ar_sec_t s_rl_ar_map[FU_AR_NENT];
static uint32_t s_rl_ar_map_key[FU_AR_NENT];   /* WRMAP 行を最後に出した時点の writes+vmatch+vmismatch+uncertain */
/* 送信した書込みの照合表添字と w 行の seq(r 行・結果不明時の記録用) */
static int      s_rl_ar_cur_valid = 0;
static uint32_t s_rl_ar_cur_idx = 0, s_rl_ar_cur_seq = 0;
/* 集計(分母と分子) */
enum { AR_REF_LATCHED = 0, AR_REF_ARM_USED, AR_REF_D_DURING, AR_REF_LEN, AR_REF_MISMATCH, AR_REF_N };
static const char* const AR_REF_NAME[AR_REF_N] = { "latched", "arm_used", "d_during_read", "len", "marker_mismatch" };
static uint32_t s_rl_ar_marker_reads, s_rl_ar_arm_ref[AR_REF_N], s_rl_ar_pass, s_rl_ar_ref_env, s_rl_ar_ref_unit,
                s_rl_ar_ref_range, s_rl_ar_ref_mark, s_rl_ar_ref_notarmed, s_rl_ar_ref_max, s_rl_ar_ref_wp,
                s_rl_ar_ok_written, s_rl_ar_ok_unexp, s_rl_ar_vmatch, s_rl_ar_vmismatch, s_rl_ar_ry0_sds,
                s_rl_ar_w_lines, s_rl_ar_r_lines, s_rl_ar_v_lines, s_rl_ar_arm_lines, s_rl_ar_w_seq, s_rl_ar_disarm_calls,
                s_rl_ar_read_err_skip, s_rl_ar_ref_drain;

/* P1053: MX68K_FLOPPYU_WRITE=normal(段階2b-1)。武装なしの通常利用の書込み中継。柵は (1) 交換後READ柵(ユニット別、
 * 基準点スナップショット比較)、(2) 同一キー連続書込み検知(時間軸つき)、(3) 読み戻し照合ラッチ。プロセス通算の送信上限は持たない。
 * 記号・定数の根拠は .mx68k_cycles/P1053_plan.md の記号表。いずれも参照実装(XEiJ/XM6t)には無い MX 独自の安全側上乗せ */
#define P1053_TAG                 "[P1053-NM]"
#define P1053_FENCE_TAG           "[P1053-FENCE]"
#define FU_WR_LOOP_MAX            32u       /* 同一キー(us,C,H,R,N)への連続送信の上限。33回目を拒否しラッチ(P1036 実測最大2) */
#ifndef FU_WR_LOOP_RESET_MS
#define FU_WR_LOOP_RESET_MS       3000      /* 同一キーの直前送信からこれを超えて空いたら数え直す。セルフテストのみ -D で短縮 */
#endif
/* P1087(D-126a): normal で装置の「整った異常終了」(ND 等)をラッチせずゲストへ素通しする第2段判定の定数。
 * FU_NM_ERR_ST1_OK = DE(0x20)|OR(0x10)|ND(0x04)|MA(0x01)。EN(0x80)・NW(0x02)・未定義ビット(0x40/0x08)は含めない
 * (根拠: XEiJ FDC.java:184-189、Core:fdc.c:205/403。.mx68k_cycles/P1087_plan.md 記号表)。
 * FU_NM_ERR_MAX は1プロセスで素通しする総数の上限(一次情報源なし、MX 独自の安全側上乗せ)。65件目は本物の7Bを返してラッチ */
#define FU_NM_ERR_ST1_OK          0x35u
#define FU_NM_ERR_MAX             64u
static int      s_nm_unit_state = 0;        /* MX68K_FLOPPYU_WRITE_UNIT: 0=未設定(全ユニット許可) 1=有効値 -1=不正値(全拒否) */
static int      s_nm_unit = -1;
/* 交換後READ柵(装置側ユニット別)。P1055: 開 ⇔ 判別保留中でない && fo_valid && fo_ue==ue[u] && fo_gen==USB世代。
 * ue[u] はユニット別エポック(増加のみ)。'D'/'d' は装置共通の s_rl_d_events を進めるが、柵はそれと比較しない(U-9 解消) */
static int      s_nm_fo_valid[4];
static uint32_t s_nm_fo_ue[4], s_nm_fo_gen[4];
static uint32_t s_nm_ue[4];
static uint32_t s_rl_rd_ue_begin[4];        /* READ 発行時の ue[] スナップショット(rl_read_begin) */
static uint32_t s_rl_wr_ue_begin[4] = {0};   /* P1058: G1/G6用、書込み収集開始時のユニット別エポック */
static int      s_nm_startup_logged = 0;
enum { NM_CL_STARTUP = 0, NM_CL_INSTALL, NM_CL_EV_D, NM_CL_EV_d, NM_CL_EJECT, NM_CL_RY0, NM_CL_GEN,
       NM_CL_ATTR_UNIT, NM_CL_ATTR_ALL, NM_CL_OUTSIDE, NM_CL_N };
static const char* const NM_CL_NAME[NM_CL_N] = { "startup", "install_reset", "ev_D", "ev_d", "eject_bit5", "ry0_at_write", "gen_changed",
                                                 "attr_unit", "attr_all", "outside_change" };
static uint32_t s_nm_close_ev[NM_CL_N];
/* P1055: 'D'/'d' の組単位の判別保留(状態機械は normal のときだけ動く)。状態遷移表・判定手順は .mx68k_cycles/P1055_plan.md。
 *  I=IDLE / H1=保留・走査未完了 / H2=保留・走査完了で 'd' 待ち。保留中は全ユニットの柵を閉扱いにする */
#define P1055_TAG                 "[P1055-ATTR]"
#define P1095_TAG                 "[P1095-E5P]"
#define P1099_TAG                 "[P1099-WRSTAGE]"
#define FU_NM_HOLD_FRAMES         30        /* 保留の打切り。起点は FDD 割込みを実際に発行したフレーム(発行待ち中は更新) */
#define FU_NM_HOLD_MAX_FRAMES     600       /* 割込み持ち越しが続く場合の 'D' 起点の上限(約11秒) */
#define FU_NM_OC_FRESH_FRAMES     1         /* 保留外の変化(oc_mask)を判定へ加える鮮度窓: 保留開始フレームとの差がこれ以下 */
#define FU_NM_ATTR_LINE_MAX       64u      /* hold/resolve/outside_change/timeout/abort 行の合計上限 */
enum { NM_ST_I = 0, NM_ST_H1, NM_ST_H2 };
static const char* const NM_ST_NAME[3] = { "I", "H1", "H2" };
/* 全閉の理由(判定手順3の不成立条件+打切り)。gen/install は既存の閉鎖記録(gen_changed/install_reset)で出す */
enum { NM_AR_NOREAD = 0, NM_AR_MULTI_D, NM_AR_ND, NM_AR_AMB, NM_AR_MULTIBIT, NM_AR_COALESCED, NM_AR_NO_BASE,
       NM_AR_ZERO, NM_AR_MULTI_CH, NM_AR_TIMEOUT, NM_AR_TIMEOUT_MAX, NM_AR_UNPAIRED_D, NM_AR_GEN, NM_AR_INSTALL, NM_AR_N };
static const char* const NM_AR_NAME[NM_AR_N] = { "noread", "multi_D", "nd", "amb", "multibit", "coalesced", "no_base",
                                                 "zero_changed", "multi_changed", "timeout", "timeout_max", "unpaired_d",
                                                 "gen", "install" };
static int      s_nm_st = NM_ST_I;
/* P1059: c3('W'送信判定)とG4('W'受理条件)で同一の述語を使う(乖離の再発を構造的に防ぐ、P1059 Code
 * Investigation案A)。normalモード限定、収集開始時点から見て当該ユニットが変化していないか。
 * 書込み専用(c3/G4専用)で、読出し側の rl_nm_fence_open() とは別機構 */
static int rl_nm_unit_unchanged(uint8_t us)
{
    return s_nm_ue[us] == s_rl_wr_ue_begin[us] && s_nm_st == NM_ST_I;
}
/* 保留文脈(全閉・判定で破棄) */
static int      s_nm_hold_f = 0, s_nm_hold_rf = 0;
static uint32_t s_nm_hold_nD = 0, s_nm_hold_nd = 0, s_nm_hold_co0 = 0;
static uint8_t  s_nm_hold_read = 0, s_nm_hold_val[4];
static int      s_nm_hold_amb = 0, s_nm_hold_multibit = 0;
static uint32_t s_nm_hold_pc = 0;
/* 基準値(src=usb・単一ビット・割当てキーの読出しだけで更新)と、保留外で基準値から変化を見たユニット集合(次の判定まで保持) */
static int      s_nm_base_ok[4];
static uint8_t  s_nm_base_val[4];
static uint8_t  s_nm_oc_mask = 0;
static int      s_nm_oc_frame[4];           /* oc_mask の各ビットが立ったフレーム(判定での鮮度確認用) */
/* 集計 */
static uint32_t s_nm_at_holds, s_nm_at_res_unit, s_nm_at_res_all, s_nm_at_ar[NM_AR_N], s_nm_at_outside,
                s_nm_at_dtf[4], s_nm_at_end_multi_D, s_nm_at_end_other, s_nm_at_lines, s_nm_at_dropped;
static uint32_t s_nm_ref_attr_hold;
/* P1062: 真のG3('D'/'d' による WAIT_W 打切り=rl_abort_op(RE_MEDIA))の計測専用。挙動には一切関与しない。
 * G3 発生時に「次の late_W 1回」「次の bringup 結果1回」へ紐付ける保留フラグを立て、紐付け行には発生フレーム
 * (s_rl_g3_pending_f)を生値で併記する。保留中の再発生は overwrite、発生時点で既に bringup 無効だった回は ambiguous に計上 */
#define P1062_TAG                 "[P1062-G3]"
static uint32_t s_rl_g3_abort = 0;
static int      s_rl_g3_pending_f = 0;
static int      s_rl_g3_w_pending = 0, s_rl_g3_bu_pending = 0, s_rl_g3_bu_ambiguous_pending = 0;
static uint32_t s_rl_g3_w_delay_n = 0, s_rl_g3_w_delay_sum = 0, s_rl_g3_w_delay_max = 0;
static uint32_t s_rl_g3_bringup_poll_ok = 0, s_rl_g3_bringup_poll_fail = 0, s_rl_g3_bringup_ambiguous = 0;
static uint32_t s_rl_g3_pending_overwrite = 0;
enum { NM_OR_ST = 0, NM_OR_D, NM_OR_GEN, NM_OR_N };
static const char* const NM_OR_NAME[NM_OR_N] = { "st_nonzero", "d_during", "gen_changed" };
static uint32_t s_nm_open_n, s_nm_open_ref[NM_OR_N];
/* 同一キー連続書込み検知。更新は装置へ書込みコマンドを実際に送った時点だけ(rl_send_write_cmd の戻り値 != -1) */
static int      s_nm_loop_valid = 0;
static uint8_t  s_nm_loop_key[5];
static uint32_t s_nm_loop_cnt = 0, s_nm_loop_cnt_max = 0, s_nm_loop_trip = 0, s_nm_loop_time_reset = 0;
static int64_t  s_nm_loop_t_us = 0;
/* 読み戻し照合表(装置側ユニット×2HD形状) */
typedef struct {
    uint32_t last_fnv, epoch_ue, epoch_gen, len;
    uint16_t writes, vmatch, vmismatch;
    uint8_t  written, pending, uncertain;
    uint8_t  cached;   /* P1089: 1=高速モードで装置の合成応答(ok_cached)により受理した(物理書込みは未確認) */
} rl_nm_sec_t;
static rl_nm_sec_t s_nm_map[4][FU_AR_NENT];
static int      s_nm_cur_valid = 0, s_nm_cur_mapped = 0;
static uint8_t  s_nm_cur_us = 0;
static uint32_t s_nm_cur_idx = 0, s_nm_cur_ue = 0, s_nm_cur_gen = 0;
static const char* s_nm_latch_why = "-";    /* normal で再起動までラッチした最初の理由 */
static int      s_nm_sy_fail_pending = 0;   /* 同期手順失敗後、次のゲストFDCコマンド開始まで1 */
static uint32_t s_nm_pass, s_nm_ref_need_read, s_nm_ref_loop, s_nm_ref_latched, s_nm_ref_wp, s_nm_ref_no_media,
                s_nm_ref_drain, s_nm_ref_unit, s_nm_ref_env, s_nm_ref_unknown, s_nm_ok_written, s_nm_ok_unexp,
                s_nm_v_match, s_nm_v_mismatch, s_nm_v_skip_epoch, s_nm_v_unmapped, s_nm_v_skip_err, s_nm_v_skip_hold,
                s_nm_sy_fence_closed, s_nm_sy_fail_c_fwd, s_nm_latch_calls,
                s_nm_w_lines, s_nm_fence_lines, s_nm_v_lines, s_nm_w_seq;
/* P1087: 装置の整った異常終了の素通し総数(上限 FU_NM_ERR_MAX)・上限到達件数・第2段で落ちた数・ST1 ビット別内訳 */
static uint32_t s_nm_err_n = 0, s_nm_err_limit = 0, s_nm_err_rej = 0;
static uint32_t s_nm_err_nd = 0, s_nm_err_de = 0, s_nm_err_or = 0, s_nm_err_ma = 0;

/* P1089(D-126-b): normal の書込みモード「高速」(XEiJ+t 方式)。設定 UI 由来の normal で設定「高速」のときだけ有効
 * (環境変数 MX68K_FLOPPYU_WRITE 設定時は無視)。書込み前の同期 SEEK と(キャッシュ経路の)非キャッシュ SDS を省き、
 * 装置ファームの write-behind(合成正常応答 形C・第4形)を ok_cached として受理する。装置キャッシュが空でない
 * 可能性がある間(dirty)は port1 の往復上限を延長し、'D'・世代変化・MX 起点リセットでの未 flush データを検出する。
 * 記号・定数の根拠は .mx68k_cycles/P1089_plan.md 記号表 */
#define P1089_TAG                 "[P1089-FAST]"
#define FU_P1_FLUSH_TIMEOUT_MS    2500      /* dirty 中の port1 往復上限(MX独自値、最悪 flush 1831ms+余裕) */
#define FU_WC_IDLE_CONFIRM_MS     200       /* dirty 中に書込みが途絶えてから能動 flush 確認 SDS を送るまで(MX独自値) */
#define FU_WC_PEND_MAX            32u       /* 未確定 cached 一覧の容量(MX独自値、装置容量10の3倍強) */
#define FU_WC_LOG_LIMIT           256u      /* [P1089-FAST] の idle_confirm/pre_reset/gen 行の上限(discard_D 行は予算対象外) */
_Static_assert(FU_P1_FLUSH_TIMEOUT_MS < FU_WR_EVENT_DEADLINE_MS, "FU_P1_FLUSH_TIMEOUT_MS は FU_WR_EVENT_DEADLINE_MS 未満");
_Static_assert(FU_WC_IDLE_CONFIRM_MS < 500, "FU_WC_IDLE_CONFIRM_MS は装置のアイドル自発 flush(500ms)より短く");
/* 設定の由来(状態取得 src と同じ値): 0=off 1=設定 3=設定は高速だが環境変数 MX68K_FLOPPYU_WRITE 優先で無視 */
enum { NMFAST_SRC_OFF = 0, NMFAST_SRC_CONFIG = 1, NMFAST_SRC_IGNORED = 3 };
static int      s_rl_write_fast_cfg = 0;    /* 設定値(install 直前に EmulatorBridge.c から渡される) */
static int      s_nm_fast = 0;              /* install で確定: normal かつ設定由来かつ設定「高速」 */
static int      s_nm_fast_src = NMFAST_SRC_OFF;
/* flush 契機の種別(flush_req を立てた直近の送信)。D_after_eject_flush の判定・クリア理由の決定に使う */
enum { WCF_NONE = 0, WCF_CMD, WCF_SDS, WCF_E5, WCF_E5_EJECT, WCF_E7, WCF_IDLE, WCF_PRE_RESET, WCF_N };
static const char* const WCF_NAME[WCF_N] = { "none", "cmd", "sds", "e5", "e5_eject", "e7", "idle", "pre_reset" };
/* dirty のクリア理由(sum 行 wc_dirty_clear= の内訳) */
enum { WCC_RX_AFTER_FLUSH = 0, WCC_FORMA, WCC_IDLE_CONFIRM, WCC_PRE_RESET, WCC_D_EJECT, WCC_DISCARD, WCC_N };
static const char* const WCC_NAME[WCC_N] = { "rx_after_flush", "forma", "idle_confirm", "pre_reset", "D_after_eject_flush",
                                             "discard" };
/* 装置キャッシュの推定状態。dirty=1: 装置キャッシュが空でない可能性がある / flush_req=1: flush 契機を送ったが完了を未確認 */
static int      s_fu_wc_dirty = 0, s_fu_wc_flush_req = 0, s_fu_wc_flush_src = WCF_NONE;
static uint32_t s_fu_wc_est = 0;            /* 推定件数(ログ用。アイドル自発 flush・満杯 flush は観測しない) */
static uint8_t  s_fu_wc_last_us = 0;        /* 直近の ok_cached の装置側 US/HD(能動 flush 確認 SDS に渡す) */
static int      s_fu_wc_dirty_f = -1, s_fu_wc_last_cached_f = -1;   /* dirty になったフレーム / 直近 ok_cached のフレーム(生値) */
static int      s_fu_wc_last_clear = -1;    /* 直近のクリア理由(WCC_*、-1=なし) */
typedef struct { uint8_t us; uint16_t idx; } rl_wc_pend_t;
static rl_wc_pend_t s_fu_wc_pend[FU_WC_PEND_MAX];   /* 直近の flush 完了確認以降に ok_cached で受理したセクタ */
static uint32_t s_fu_wc_pend_n = 0;
static int      s_fu_wc_pend_ovf = 0;       /* 一覧が溢れた(このときは cached=1 かつ未照合の全セクタを対象にする) */
static uint32_t s_fu_wc_send_est = 0;       /* 書込みコマンド送信時点の s_fu_wc_est('W' 待ち時間の est≥10 判定用) */
/* 集計(分母と分子) */
static uint32_t s_nm_ok_cached = 0, s_nm_fast_nosync = 0, s_nm_sds_skip_fast = 0, s_nm_sds_cf_first = 0;
static uint32_t s_fu_wc_dirty_set = 0, s_fu_wc_flush_req_n = 0, s_fu_wc_clr[WCC_N];
static uint32_t s_fu_wc_discard_D = 0, s_fu_wc_discard_sectors = 0, s_fu_wc_gen_dirty = 0, s_fu_wc_pre_reset_fail = 0;
static uint32_t s_fu_wc_pend_ovf_n = 0, s_fu_wc_lines = 0;
static int      s_fu_wc_shutdown_logonly = 0;
static uint32_t s_rl_p1_flush_ext = 0;
static int64_t  s_rl_p1_flush_ext_max_us = 0;
static int64_t  s_fu_wr_cmd_to_W_max_us = 0, s_fu_wr_cmd_to_W_max_us_est10 = 0;
static uint32_t s_nm_v_match_cached = 0, s_nm_v_mismatch_cached = 0;
static uint32_t s_nm_cached_unverified = 0;   /* rl_tick で周期的に数え直す(設定画面の状態行用、原子的に読む) */

/* 判別保留を除いた柵の状態(ログ・拒否理由の分類用)。送信判断には rl_nm_fence_open を使う */
static int rl_nm_fence_open_nohold(uint8_t us)
{
    us &= 3u;
    return s_nm_fo_valid[us] && s_nm_fo_ue[us] == s_nm_ue[us] && s_nm_fo_gen[us] == mx68k_floppyu_usb_generation();
}
static int rl_nm_fence_open(uint8_t us)
{
    return s_nm_st == NM_ST_I && rl_nm_fence_open_nohold(us);
}
static uint8_t rl_nm_open_mask(void)
{
    uint8_t m = 0;
    for (uint8_t u = 0; u < 4u; u++) if (rl_nm_fence_open(u)) m |= (uint8_t)(1u << u);
    return m;
}
static uint8_t rl_nm_open_mask_nohold(void)
{
    uint8_t m = 0;
    for (uint8_t u = 0; u < 4u; u++) if (rl_nm_fence_open_nohold(u)) m |= (uint8_t)(1u << u);
    return m;
}
/* 柵の閉鎖記録。unit<0 は全ユニット。bump=1 ならエポック ue を進める(unit<0 は全4ユニット、それ以外は当該ユニット)。
 * clear=1 なら fo_valid を明示的に落とす。'D'/'d' の記録は bump=0・clear=0(記録のみ、閉じるかは判別保留の判定が決める)。
 * was_open は閉鎖前に開いていたユニットのビット列(呼出し側で事象の直前に取る)。reason は attr_all の理由(他は NULL) */
static void rl_nm_fence_close(int unit, int why, int bump, int clear, uint8_t was_open, const char* reason)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    if (bump) {
        if (unit < 0) { for (int u = 0; u < 4; u++) s_nm_ue[u]++; }
        else          s_nm_ue[unit & 3]++;
    }
    if (clear) {
        if (unit < 0) memset(s_nm_fo_valid, 0, sizeof(s_nm_fo_valid));
        else          s_nm_fo_valid[unit & 3] = 0;
    }
    s_nm_close_ev[why]++;
    if (s_nm_fence_lines >= FU_AR_LOG_LIMIT) return;
    s_nm_fence_lines++;
    char us[8];
    if (unit < 0) snprintf(us, sizeof(us), "all");
    else          snprintf(us, sizeof(us), "%d", unit & 3);
    debug_log(P1053_FENCE_TAG " close f=%d why=%s unit=%s was_open=0x%x open_after=0x%x d_events=%u gen=%u n=%u"
              " bump=%d ue=%u,%u,%u,%u st=%s reason=%s\n",
              g_mx68k_frame_num, NM_CL_NAME[why], us, (unsigned)was_open, (unsigned)rl_nm_open_mask(),
              (unsigned)s_rl_d_events, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_nm_close_ev[why],
              bump, (unsigned)s_nm_ue[0], (unsigned)s_nm_ue[1], (unsigned)s_nm_ue[2], (unsigned)s_nm_ue[3],
              NM_ST_NAME[s_nm_st], reason ? reason : "-");
}
/* normal の再起動までのラッチ。最初の理由を保持し、ログは最初の1回だけ */
static void rl_nm_latch(const char* why)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    s_rl_wr_latched_off = 1;
    s_nm_latch_calls++;
    if (s_nm_latch_why[0] != '-') return;
    s_nm_latch_why = why;
    debug_log(P1053_TAG " latch f=%d why=%s d_events=%u gen=%u\n",
              g_mx68k_frame_num, why, (unsigned)s_rl_d_events, (unsigned)mx68k_floppyu_usb_generation());
}
/* 送信後に結果不明・不採用で既存ラッチへ入る経路(rl_ar_after_unknown から呼ぶ)。送信した書込みのセクタは照合しない */
static void rl_nm_after_unknown(const char* why)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    if (s_nm_cur_valid && s_nm_cur_mapped) s_nm_map[s_nm_cur_us][s_nm_cur_idx].uncertain = 1;
    s_nm_cur_valid = 0;
    rl_nm_latch(why);
}

/* P1089: 装置キャッシュの dirty を落とす(why=WCC_*)。未確定一覧も空にする */
static void rl_wc_clear(int why)
{
    s_fu_wc_clr[why]++;
    s_fu_wc_last_clear = why;
    s_fu_wc_dirty = 0;
    s_fu_wc_flush_req = 0;
    s_fu_wc_flush_src = WCF_NONE;
    s_fu_wc_est = 0;
    s_fu_wc_pend_n = 0;
    s_fu_wc_pend_ovf = 0;
}

/* P1089: dirty 中に flush 契機を送る(送った)ときに呼ぶ。dirty は保持し、この後の受信つき往復の成功で落とす(M1)。
 * 契機の種別は直近のもので上書きする(装置は受信順に処理するので、'D' より前の最後の契機が排出なら flush 済み) */
static void rl_wc_note_trigger(int src)
{
    if (!s_fu_wc_dirty) return;
    if (!s_fu_wc_flush_req) s_fu_wc_flush_req_n++;
    s_fu_wc_flush_req = 1;
    s_fu_wc_flush_src = src;
}

/* P1089: ok_cached を受理した。dirty を立て、照合表に対応するセクタなら未確定一覧へ加える */
static void rl_wc_on_cached(uint8_t us_hd, int mapped, uint8_t us, uint32_t idx)
{
    if (!s_fu_wc_dirty) { s_fu_wc_dirty_set++; s_fu_wc_dirty_f = g_mx68k_frame_num; }
    s_fu_wc_dirty = 1;
    s_fu_wc_est++;
    s_fu_wc_last_us = us_hd & 7u;
    s_fu_wc_last_cached_f = g_mx68k_frame_num;
    if (!mapped) return;
    if (s_fu_wc_pend_n < FU_WC_PEND_MAX) {
        s_fu_wc_pend[s_fu_wc_pend_n].us = us & 3u;
        s_fu_wc_pend[s_fu_wc_pend_n].idx = (uint16_t)idx;
        s_fu_wc_pend_n++;
    } else if (!s_fu_wc_pend_ovf) {
        s_fu_wc_pend_ovf = 1;
        s_fu_wc_pend_ovf_n++;
    }
}

/* P1089: 未確定一覧(溢れていれば cached=1 かつ未照合の全セクタ)を uncertain にし、その件数を返す */
static uint32_t rl_wc_mark_uncertain(void)
{
    uint32_t n = 0;
    if (s_fu_wc_pend_ovf) {
        for (uint32_t u = 0; u < 4u; u++)
            for (uint32_t i = 0; i < FU_AR_NENT; i++) {
                rl_nm_sec_t* e = &s_nm_map[u][i];
                if (e->written && e->cached && e->pending && !e->uncertain) { e->uncertain = 1; n++; }
            }
        return n;
    }
    for (uint32_t k = 0; k < s_fu_wc_pend_n; k++) {
        rl_nm_sec_t* e = &s_nm_map[s_fu_wc_pend[k].us][s_fu_wc_pend[k].idx];
        if (!e->uncertain) { e->uncertain = 1; n++; }
    }
    return n;
}

/* P1089: 未確定一覧を「unit:C/H/R」の列にする(ログ用) */
static void rl_wc_pend_str(char* out, size_t sz)
{
    size_t o = 0;
    out[0] = '\0';
    for (uint32_t k = 0; k < s_fu_wc_pend_n && o + 24 < sz; k++) {
        uint32_t i = s_fu_wc_pend[k].idx;
        o += (size_t)snprintf(out + o, sz - o, "%s%u:%u/%u/%u", k ? "," : "", (unsigned)s_fu_wc_pend[k].us,
                              (unsigned)(i / (FU_AR_NHEAD * FU_AR_NSEC)), (unsigned)((i / FU_AR_NSEC) % FU_AR_NHEAD),
                              (unsigned)(i % FU_AR_NSEC + 1u));
    }
    if (o == 0) snprintf(out, sz, "-");
}

/* P1089: 装置が未 flush データを失った(失った可能性がある)。未確定セクタを uncertain にしてラッチし、dirty を落とす。
 * why: 'D' 受信=wcache_discard_D / USB 世代変化=gen_changed_dirty / bringup 前の flush 確認失敗=pre_reset_flush_fail。
 * 行は予算対象外('D' の破棄は稀かつ診断上重要。世代変化・確認失敗も同様に稀) */
static void rl_wc_discard(const char* why)
{
    char ps[FU_WC_PEND_MAX * 16 + 8];
    rl_wc_pend_str(ps, sizeof(ps));
    uint32_t est = s_fu_wc_est, pn = s_fu_wc_pend_n;
    int ovf = s_fu_wc_pend_ovf, fr = s_fu_wc_flush_req, src = s_fu_wc_flush_src;
    uint32_t n = rl_wc_mark_uncertain();
    s_fu_wc_discard_sectors += n;
    rl_nm_latch(why);
    debug_log(P1089_TAG " discard f=%d why=%s est=%u pend_n=%u ovf=%d uncertain_n=%u pend=%s flush_req=%d flush_src=%s"
              " dirty_f=%d last_cached_f=%d gen=%u d_events=%u latched=%d\n",
              g_mx68k_frame_num, why, (unsigned)est, (unsigned)pn, ovf, (unsigned)n, ps, fr, WCF_NAME[src],
              s_fu_wc_dirty_f, s_fu_wc_last_cached_f, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_rl_d_events,
              s_rl_wr_latched_off);
    rl_wc_clear(WCC_DISCARD);
}

/* P1089: port1 往復の上限。装置キャッシュが空でない可能性がある間(dirty)だけ延長する。
 * dirty は高速モードの ok_cached でしか立たない(安全優先では常に FU_P1_TIMEOUT_MS)。高速→安全優先へ切り替えた
 * ハードリセット直後に残った dirty でも、bringup 前の flush 確認(B-11)が延長上限で動くよう s_nm_fast は条件に含めない */
static int rl_p1_timeout_ms(void)
{
    return s_fu_wc_dirty ? FU_P1_FLUSH_TIMEOUT_MS : FU_P1_TIMEOUT_MS;
}

/* P1089: port1 往復の後処理。延長上限で実行した往復を数え、flush 契機の後の受信つき往復が成功したら dirty を落とす(M1) */
static void rl_p1_after(int ext, int64_t dt, size_t rxlen, int r)
{
    if (ext) {
        s_rl_p1_flush_ext++;
        if (dt > s_rl_p1_flush_ext_max_us) s_rl_p1_flush_ext_max_us = dt;
    }
    if (r >= 1 && rxlen > 0 && s_fu_wc_dirty && s_fu_wc_flush_req) {
        int why = s_fu_wc_flush_src == WCF_IDLE ? WCC_IDLE_CONFIRM
                : s_fu_wc_flush_src == WCF_PRE_RESET ? WCC_PRE_RESET : WCC_RX_AFTER_FLUSH;
        rl_wc_clear(why);
    }
}

/* P1067: normal FORMAT の唯一の停止入口。最初の理由を保持し、走行中なら半端状態(partial)を立て、同プロセスの
 * normal WRITE DATA も止める(rl_nm_latch)。書込みラッチが先に別経路で立った場合も、呼出し側(打切り4経路)から必ず呼ぶ */
static void rl_nmf_halt(const char* why)
{
    if (s_nmf_ld_valid) { s_nmf_ld_valid = 0; s_nmf_ld_exp_halt++; }   /* P1070 X1: 停止で完走記録を失効(早期 return より前) */
    if (s_nmf_run_active) s_nmf_partial = 1;
    if (s_nmf_halt) return;
    s_nmf_halt = 1;
    s_nmf_halt_why = why;
    if (s_nmf_run_active) s_nmf_runs_aborted++;
    rl_nm_latch("format_halt");
    debug_log(P1067_TAG " halt f=%d why=%s unit=%u C=%u H=%u k=%u tracks_sent=%u tracks_ok=%u partial=%d run_active=%d"
              " run_us=%u run_pos=%u inflight=%d verify_pending=%d latched=%d\n",
              g_mx68k_frame_num, why, (unsigned)s_nmf_sent_us, (unsigned)s_nmf_sent_c, (unsigned)s_nmf_sent_h,
              (unsigned)s_nmf_sent_k, (unsigned)s_nmf_sends, (unsigned)s_nmf_tracks_ok, s_nmf_partial, s_nmf_run_active,
              (unsigned)s_nmf_run_us, (unsigned)s_nmf_run_pos, s_nmf_inflight, s_nmf_verify_pending, s_rl_wr_latched_off);
}

/* P1067: PCN 記録の無効化(unit<0 は全ユニット)。cnt は契機別の計数先 */
static void rl_nmf_pcn_invalidate(int unit, uint32_t* cnt)
{
    if (unit < 0) memset(s_nmf_pcn_valid, 0, sizeof(s_nmf_pcn_valid));
    else          s_nmf_pcn_valid[unit & 3] = 0;
    if (cnt) (*cnt)++;
}

/* P1067: 装置経由のゲスト SENSE INT の結果(装置側 ST0 生値・PCN)から PCN 記録を更新する。
 * IC=00 かつ SE=1 なら記録、それ以外の有効な ST0(IC=01 異常終了・IC=11 Ready 変化・SE 無し)は当該ユニットを無効化、
 * 0x80(保留割込み無し)は何もしない */
static void rl_nmf_pcn_note(uint8_t st0_dev, int have_pcn, uint8_t pcn)
{
    if ((st0_dev & 0xC0u) == 0x80u) return;
    uint8_t us = st0_dev & 3u;
    int rec = (st0_dev & 0xC0u) == 0x00u && (st0_dev & 0x20u) && have_pcn;
    if (rec) {
        s_nmf_pcn[us] = pcn;
        s_nmf_pcn_valid[us] = 1;
        s_nmf_pcn_ue[us] = s_nm_ue[us];
        s_nmf_pcn_gen[us] = mx68k_floppyu_usb_generation();
        s_nmf_pcn_f[us] = g_mx68k_frame_num;
        s_nmf_pcn_st0[us] = st0_dev;
        s_nmf_pcn_rec++;
    } else {
        rl_nmf_pcn_invalidate(us, &s_nmf_pcn_inval_sense);
    }
    if (s_nmf_on && s_nmf_pcn_lines < FU_AR_LOG_LIMIT) {
        s_nmf_pcn_lines++;
        debug_log(P1067_TAG " pcn f=%d unit=%u st0_dev=0x%02x have_pcn=%d pcn=%u action=%s ue=%u gen=%u\n",
                  g_mx68k_frame_num, (unsigned)us, (unsigned)st0_dev, have_pcn, (unsigned)pcn, rec ? "record" : "invalidate",
                  (unsigned)s_nm_ue[us], (unsigned)mx68k_floppyu_usb_generation());
    }
}

/* P1035: 送信上限はモード別(wponly 8 / mode1 1)。上限を参照する全箇所はこの関数を通す。P1037: armed 256。
 * P1053: normal は上限なし(UINT32_MAX を返すが判定には使わない。ログ表示は rl_wr_max_str で "-") */
static uint32_t rl_wr_send_max(void)
{
    if (s_rl_write_mode == WRM_FMT1) {   /* P1064: 既定値 FU_WR_SEND_MAX(8)へ落とさない */
        if (s_rl_fm_env_ok && s_rl_fm_multi) {   /* P1066: min(許可面数, 絶対上限) */
            uint32_t pc = (uint32_t)__builtin_popcount((unsigned)s_rl_fm_hmask);
            return pc < FU_FMT1_SEND_HARD_MAX ? pc : FU_FMT1_SEND_HARD_MAX;
        }
        return FU_FMT1_SEND_MAX;
    }
    if (s_rl_write_mode == WRM_NORMAL) return UINT32_MAX;
    return s_rl_write_mode == WRM_ARMED ? FU_WR_SEND_MAX_ARMED
         : (s_rl_write_mode == WRM_WRITE ? FU_WR_SEND_MAX_WRITE : FU_WR_SEND_MAX);
}
static const char* rl_wr_max_str(char* buf, size_t sz)
{
    if (s_rl_write_mode == WRM_NORMAL) snprintf(buf, sz, "-");
    else                               snprintf(buf, sz, "%u", (unsigned)rl_wr_send_max());
    return buf;
}

/* P1028: 遅延発行の集計(分母 s_rl_c_seq / s_p955_cmd_start と併記する) */
#define P1028_DEFER_TAG       "[P1028-CDEFER]"
#define P1028_RING_TAG        "[P1028-CEVENT-RING]"
#define P1028_RING_N          64u
#define P1028_NEXT_CMD_NONE   0xFFFFu
static uint32_t s_p1028_withdrawn, s_p1028_deferred_at_c, s_p1028_merged, s_p1028_lines;
static uint32_t s_p1028_reraise_result, s_p1028_reraise_seek, s_p1028_reraise_nonrelay, s_p1028_reraise_tick;
static int      s_p1028_defer_f = -1;
static int      s_p1028_vec_dumped = 0;
typedef struct {
    uint32_t seq;            /* 0=未使用 */
    int      f;
    uint32_t pc;
    uint16_t sr;
    uint8_t  state;
    uint8_t  irq_after;      /* rl_on_c 処理直後の IRQH_IRQ[1] */
    const char* act;
    uint16_t next_cmd;       /* 直後の新規コマンド第1バイト(P1028_NEXT_CMD_NONE=未着) */
    uint8_t  withdrawn;      /* その新規コマンド開始で取り下げたか */
    uint8_t  deferred;       /* 受信時点で発行を見送ったか */
} p1028_cevent_t;
static p1028_cevent_t s_p1028_ring[P1028_RING_N];
static uint32_t s_p1028_ring_dumped_seq = 0;

static int64_t rl_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void rl_hex(char* out, size_t outsz, const uint8_t* p, size_t n, size_t max)
{
    size_t show = n < max ? n : max, o = 0;
    out[0] = '\0';
    for (size_t i = 0; i < show && o + 3 < outsz; i++) o += (size_t)snprintf(out + o, outsz - o, "%02X", p[i]);
    if (show < n && o + 4 < outsz) snprintf(out + o, outsz - o, "...");
}

static uint32_t rl_fnv(const uint8_t* p, uint32_t n)
{
    uint32_t h = 2166136261u;   /* FNV-1a 32bit(P1004 と同じ) */
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* P1037: 武装マーカー(先頭32B=ASCII、以降 (i*37+0x5B)&0xFF)と E5埋めの FNV を1回だけ生成する。
 * 期待 FNV は marker=0x9ca6c9a1・fill=0x0e9d71c5(P1037 記号表、定数は埋め込まず実行時に計算してログへ出す) */
static void rl_ar_marker_build(void)
{
    if (s_rl_ar_marker_built) return;
    uint8_t fill[FU_AR_SECLEN];
    memcpy(s_rl_ar_marker, FU_AR_MARK_HEAD, 32);
    for (uint32_t i = 32; i < FU_AR_SECLEN; i++) s_rl_ar_marker[i] = (uint8_t)((i * 37u + 0x5Bu) & 0xFFu);
    memset(fill, 0xE5, sizeof(fill));
    s_rl_ar_marker_fnv = rl_fnv(s_rl_ar_marker, FU_AR_SECLEN);
    s_rl_ar_fill_fnv = rl_fnv(fill, FU_AR_SECLEN);
    s_rl_ar_marker_built = 1;
}

/* P1037: 永久ラッチ。armed の全ての停止経路はこれを通す(解除経路なし、再武装は MX 再起動のみ)。
 * ログは武装中→停止の遷移1回だけ(武装は1プロセス1回なので1プロセス最大1行)。2回目以降の呼出しは件数のみ */
static void rl_ar_disarm(const char* why)
{
    if (s_rl_write_mode != WRM_ARMED) return;
    if (s_rl_ar_disarm[0] == '-') s_rl_ar_disarm = why;   /* 最初の理由を保持 */
    int was = s_rl_ar_armed;
    s_rl_ar_armed = 0;
    s_rl_wr_latched_off = 1;        /* 既存ラッチを流用(解除経路なし) */
    s_rl_ar_disarm_calls++;
    if (!was) return;
    debug_log(P1037_ARM_TAG " disarm f=%d why=%s was_armed=%d arm_f=%d sends=%u/%u d_events=%u gen=%u arm_gen=%u\n",
              g_mx68k_frame_num, why, was, s_rl_ar_arm_f, (unsigned)s_rl_wr_sends, (unsigned)FU_WR_SEND_MAX_ARMED,
              (unsigned)s_rl_d_events, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_rl_ar_arm_gen);
}

/* P1037: セクタ番号→照合表の添字。前提: c<77, h<2, 1<=r<=8(呼出し元で範囲検査済み) */
static uint32_t rl_ar_idx(uint8_t c, uint8_t h, uint8_t r)
{
    return ((uint32_t)c * FU_AR_NHEAD + h) * FU_AR_NSEC + (uint32_t)(r - 1u);   /* 0..1231 */
}

/* P1037: MX68K_FLOPPYU_WRITE_CYL の解析。受理は「数字1〜2桁 '-' 数字1〜2桁 で終端」かつ a<=b<=76 のみ。
 * 符号・空白・空・3桁以上・余分な文字は不正(rl_wf_parse_chrn と同じく strtoul を使わず1文字ずつ手で解析する) */
static int rl_ar_parse_cyl(const char* s, uint8_t* a, uint8_t* b)
{
    unsigned v[2];
    if (s == NULL) return 0;
    for (int k = 0; k < 2; k++) {
        unsigned x = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 2u) return 0;      /* 3桁以上 */
            x = x * 10u + (unsigned)(*s - '0');
            s++;
        }
        if (digits == 0u) return 0;           /* 空・符号・空白で始まる */
        v[k] = x;
        if (k == 0) { if (*s != '-') return 0; s++; }
    }
    if (*s != '\0') return 0;                 /* 余分な文字(2個目の '-' 等) */
    if (v[0] > v[1] || v[1] > 76u) return 0;
    *a = (uint8_t)v[0]; *b = (uint8_t)v[1];
    return 1;
}

/* P1010: DnBを立てる。同じユニットが既に立っていれば時刻等だけ更新(再SEEKはやり直し扱い、件数は数えない) */
static void rl_dnb_set(uint8_t umx, uint8_t udev, const char* name)
{
    umx &= 3u;
    int again = (s_rl_dnb_mx >> umx) & 1u;
    int64_t now = rl_now_us();
    uint32_t gen = mx68k_floppyu_usb_generation();
    s_rl_dnb_mx |= (uint8_t)(1u << umx);
    s_rl_dnb_t_us[umx] = now ? now : 1;
    s_rl_dnb_gen[umx] = gen;
    s_rl_dnb_udev[umx] = udev & 3u;
    if (!again) {
        s_rl_dnb_set++;
        s_rl_dnb_busy_relay[umx] = 0;
        s_rl_dnb_busy_core[umx] = 0;
    }
    if (s_rl_dnb_set_lines < FU_LOG_LIMIT) {
        s_rl_dnb_set_lines++;
        debug_log(P1010_TAG " dnb set f=%d gen=%u cmd=%s unit_mx=%u unit_dev=%u again=%d mask_after=0x%02x\n",
                  g_mx68k_frame_num, (unsigned)gen, name, (unsigned)umx, (unsigned)(udev & 3u), again,
                  (unsigned)s_rl_dnb_mx);
    }
}

/* P1010: DnBを落とす。立っていなければ何もしない。st0_mx は reason=sense のときのみ実値(それ以外0xff) */
static void rl_dnb_clear(uint8_t umx, const char* reason, uint8_t st0_mx)
{
    umx &= 3u;
    if (!((s_rl_dnb_mx >> umx) & 1u)) return;
    s_rl_dnb_mx &= (uint8_t)~(1u << umx);
    if      (strcmp(reason, "sense") == 0)      s_rl_dnb_clr_sense++;
    else if (strcmp(reason, "timeout") == 0)    s_rl_dnb_clr_timeout++;
    else if (strcmp(reason, "disconnect") == 0) s_rl_dnb_clr_disc++;
    else if (strcmp(reason, "fault") == 0)      s_rl_dnb_clr_fault++;
    else                                        s_rl_dnb_clr_other++;
    int64_t held = rl_now_us() - s_rl_dnb_t_us[umx];
    s_rl_dnb_t_us[umx] = 0;
    if (s_rl_dnb_clr_lines < FU_LOG_LIMIT) {
        s_rl_dnb_clr_lines++;
        uint32_t br = s_rl_dnb_busy_relay[umx], bc = s_rl_dnb_busy_core[umx];
        debug_log(P1010_TAG " dnb clear f=%d gen=%u set_gen=%u unit_mx=%u unit_dev=%u reason=%s st0_mx=0x%02x held_us=%lld"
                  " busy_reads=%u busy_reads_relay=%u busy_reads_core=%u mask_after=0x%02x\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_rl_dnb_gen[umx],
                  (unsigned)umx, (unsigned)s_rl_dnb_udev[umx], reason, (unsigned)st0_mx, (long long)held,
                  (unsigned)(br + bc), (unsigned)br, (unsigned)bc, (unsigned)s_rl_dnb_mx);
    }
}

static void rl_dnb_clear_all(const char* reason)
{
    for (uint8_t u = 0; u < 4u; u++) rl_dnb_clear(u, reason, 0xffu);
}

/* P1010: USB未接続、または立てたときと接続世代が違うユニットのDnBを解除する */
static void rl_dnb_check_link(int conn, uint32_t gen)
{
    for (uint8_t u = 0; u < 4u; u++)
        if (((s_rl_dnb_mx >> u) & 1u) && (!conn || s_rl_dnb_gen[u] != gen)) rl_dnb_clear(u, "disconnect", 0xffu);
}

/* P1032: 書込みコマンドを装置へ送った後の待ち状態(いずれも装置FDCが実行/結果フェーズにある) */
static int rl_in_write_wait(void)
{
    return s_rl_state == RL_WR_WAIT_W || s_rl_state == RL_WR_WAIT_DONE || s_rl_state == RL_WR_RESULT;
}
static int rl_op_needs_device(void)
{
    return s_rl_state == RL_RD_WAIT || s_rl_state == RL_RID_WAIT || rl_in_write_wait();
}
static int rl_in_read_phase(void)
{
    return s_rl_state == RL_RD_WAIT || s_rl_state == RL_RD_SUPPLY || s_rl_state == RL_RID_WAIT;
}

/* P1052(D-116): 装置port2の'D'(FDD割込みオン)を、ゲストのFDD割込み(vec IntVect+1)へ転送する。
 * 'D'受信ではpendingを立てるだけで、発行は rl_tick の安全点(rl_fdd_int_service)で行う。
 * 計数の恒等式(崩れていれば計数か判定のバグ):
 *   raised = acked + requeued + requeued_ext + inflight + reset_drop_inflight
 *   pend_set + requeued + requeued_ext = raised + masked + pending + reset_drop_pend
 * 不変条件: pending と inflight は同時に1にならない('D'が発行済み・未受理の要求に重なった場合は coalesced) */
#define P1052_TAG             "[P1052-FDDINT]"
static uint32_t s_rl_D_events, s_rl_dl_events;   /* 'D'(0x44)/'d'(0x64)の個別受信数(合算の s_rl_d_events とは別) */
static int      s_rl_fdd_int_pending = 0;        /* 発行待ちのFDD割込みあり */
static int      s_rl_fi_inflight = 0;            /* 発行済み・ゲスト未受理 */
static uint32_t s_rl_fi_pend_set, s_rl_fi_coalesced, s_rl_fi_raised, s_rl_fi_masked;
static uint32_t s_rl_fi_deferred_irq1, s_rl_fi_deferred_cdef, s_rl_fi_deferred_wr;
static uint32_t s_rl_fi_streak, s_rl_fi_streak_max;
static uint32_t s_rl_fi_requeued, s_rl_fi_requeued_ext, s_rl_fi_acked, s_rl_fi_ack_after_reset;
static uint32_t s_rl_fi_reset_drop_pend, s_rl_fi_reset_drop_inflight;
static uint32_t s_rl_fi_lines;

/* ===========================================================================
 *  P1054: 'D'/'d' 後のユニット別変化の受動観測 [P1054-E5P](診断専用・挙動不変)
 * ---------------------------------------------------------------------------
 *  ゲスト自身の $E94005 読出し(p955_route_read の case 0x05)を、戻り値確定後に「読むだけ」で記録する。
 *  ★e5p_* 関数は s_rl_* / s_nm_* / s_p955_* へ一切代入しない。観測用の状態は全て s_e5p_* に閉じる。
 *  窓: 'D'/'d' 受信で開き(開いていれば追記)、最後のイベントから E5P_CLOSE_FRAMES フレーム経過で閉じる。
 *      install(init/ハードリセット)・USB接続世代の変化・shutdown では強制終端する(why=install|gen|shutdown)。
 *  キー: 装置側 $E94005 の選択ビット(dev_ctrl & 0x0F)。割当てユニットの単一ビットキーは k1(ユニット0)・k2(ユニット1)。
 *  値: 装置寄与(dev_part)。キー別の最終値は src が usb/cache/wrwait の読出しだけで更新する
 *      (nobit/norelay/usbfail は装置の値ではないため。件数は by_src で数える)。
 *  窓外の読出しは行を出さず、キー別の最終値と件数だけを更新する。
 * =========================================================================== */
#define P1054_TAG             "[P1054-E5P]"
#define E5P_CLOSE_FRAMES      120     /* 最後のイベントから約2秒(P1054 記号表) */
#define E5P_WIN_DETAIL_MAX    64u     /* open/ev/rd 行を出す窓の数 */
#define E5P_RD_PER_WIN_MAX    48u     /* 1窓あたりの rd 行上限 */
#define E5P_WINSUM_MAX        256u
enum { E5P_SRC_USB = 0, E5P_SRC_CACHE, E5P_SRC_WRWAIT, E5P_SRC_NOBIT, E5P_SRC_NORELAY, E5P_SRC_USBFAIL, E5P_SRC_N };
static const char* const E5P_SRC_NAME[E5P_SRC_N] = { "usb", "cache", "wrwait", "nobit", "norelay", "usbfail" };
static int      s_e5p_seen = 0;               /* 振り分け器が一度でも有効になったか(shutdown 時の sum 行の要否) */
static uint8_t  s_e5p_last_val[16];
static int      s_e5p_last_f[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int      s_e5p_open = 0, s_e5p_detail = 0;
static uint32_t s_e5p_win = 0;                /* 窓の連番(1始まり) */
static int      s_e5p_f_open = 0, s_e5p_last_ev_f = 0;
static uint32_t s_e5p_n_ev, s_e5p_nD, s_e5p_nd, s_e5p_co0, s_e5p_ack0;
static uint32_t s_e5p_reads, s_e5p_reads_assigned, s_e5p_by_src[E5P_SRC_N], s_e5p_multibit, s_e5p_seq;
static uint32_t s_e5p_rd_lines, s_e5p_lines_dropped, s_e5p_outside_at_open;
static uint8_t  s_e5p_unit_mask, s_e5p_changed_within;
static uint8_t  s_e5p_pre_val[16], s_e5p_first_val[16], s_e5p_wlast_val[16];
static int      s_e5p_pre_f[16];
static int      s_e5p_first_ok[16];
/* 累積 */
static uint32_t s_e5p_wins_logged, s_e5p_reads_total, s_e5p_by_src_total[E5P_SRC_N], s_e5p_rd_lines_total;
static uint32_t s_e5p_rd_dropped_total, s_e5p_winsum_lines, s_e5p_winsum_dropped, s_e5p_outside_reads;

static int e5p_single_bit(uint8_t k) { return k != 0u && (k & (uint8_t)(k - 1u)) == 0u; }

/* "k1:xx,k2:xx"(未観測は "--")。with_f=1 なら "@f" を付ける */
static void e5p_kv(char* out, size_t sz, const uint8_t* val, const int* ok_or_f, int with_f)
{
    size_t o = 0;
    out[0] = '\0';
    for (int k = 1; k <= 2; k++) {
        int ok = with_f ? (ok_or_f[k] >= 0) : ok_or_f[k];
        if (ok) o += (size_t)snprintf(out + o, sz - o, "%sk%d:%02x", k > 1 ? "," : "", k, (unsigned)val[k]);
        else    o += (size_t)snprintf(out + o, sz - o, "%sk%d:--", k > 1 ? "," : "", k);
        if (with_f && o < sz) o += (size_t)snprintf(out + o, sz - o, "@%d", ok_or_f[k]);
        if (o >= sz) { out[sz - 1] = '\0'; return; }
    }
}

static void e5p_close(const char* why)
{
    if (!s_e5p_open) return;
    s_e5p_open = 0;
    int pre_ok[16], last_ok[16];
    uint8_t changed = 0;
    for (int k = 0; k < 16; k++) {
        pre_ok[k] = s_e5p_pre_f[k] >= 0;
        last_ok[k] = s_e5p_first_ok[k];
        if (pre_ok[k] && last_ok[k] && s_e5p_pre_val[k] != s_e5p_wlast_val[k]) changed |= (uint8_t)k;
    }
    if (s_e5p_winsum_lines >= E5P_WINSUM_MAX) { s_e5p_winsum_dropped++; return; }
    s_e5p_winsum_lines++;
    char fs[48], ls[48], ps[48];
    e5p_kv(fs, sizeof(fs), s_e5p_first_val, s_e5p_first_ok, 0);
    e5p_kv(ls, sizeof(ls), s_e5p_wlast_val, last_ok, 0);
    e5p_kv(ps, sizeof(ps), s_e5p_pre_val, pre_ok, 0);
    debug_log(P1054_TAG " win_sum win=%u why=%s f_open=%d f_close=%d n_ev=%u nD=%u nd=%u coalesced_delta=%u acked_delta=%u"
              " reads=%u reads_assigned=%u by_src=usb:%u,cache:%u,wrwait:%u,nobit:%u,norelay:%u,usbfail:%u multibit=%u"
              " unit_read_mask=0x%x first=%s last=%s pre=%s changed_mask=0x%x changed_within=0x%x"
              " outside_reads_since_prev=%u lines_dropped=%u\n",
              (unsigned)s_e5p_win, why, s_e5p_f_open, g_mx68k_frame_num, (unsigned)s_e5p_n_ev,
              (unsigned)s_e5p_nD, (unsigned)s_e5p_nd, (unsigned)(s_rl_fi_coalesced - s_e5p_co0),
              (unsigned)(s_rl_fi_acked - s_e5p_ack0), (unsigned)s_e5p_reads, (unsigned)s_e5p_reads_assigned,
              (unsigned)s_e5p_by_src[E5P_SRC_USB], (unsigned)s_e5p_by_src[E5P_SRC_CACHE],
              (unsigned)s_e5p_by_src[E5P_SRC_WRWAIT], (unsigned)s_e5p_by_src[E5P_SRC_NOBIT],
              (unsigned)s_e5p_by_src[E5P_SRC_NORELAY], (unsigned)s_e5p_by_src[E5P_SRC_USBFAIL],
              (unsigned)s_e5p_multibit, (unsigned)s_e5p_unit_mask, fs, ls, ps, (unsigned)changed,
              (unsigned)s_e5p_changed_within, (unsigned)s_e5p_outside_at_open, (unsigned)s_e5p_lines_dropped);
}

/* 'D'/'d' 受信時(既存処理の後)に呼ぶ */
static void e5p_on_event(char ev)
{
    if (!s_e5p_open) {
        s_e5p_open = 1;
        s_e5p_win++;
        s_e5p_detail = s_e5p_win <= E5P_WIN_DETAIL_MAX;
        if (s_e5p_detail) s_e5p_wins_logged++;
        s_e5p_f_open = g_mx68k_frame_num;
        s_e5p_n_ev = 0; s_e5p_nD = 0; s_e5p_nd = 0;
        s_e5p_co0 = s_rl_fi_coalesced;
        s_e5p_ack0 = s_rl_fi_acked;
        s_e5p_reads = 0; s_e5p_reads_assigned = 0; s_e5p_multibit = 0; s_e5p_seq = 0;
        memset(s_e5p_by_src, 0, sizeof(s_e5p_by_src));
        s_e5p_rd_lines = 0; s_e5p_lines_dropped = 0;
        s_e5p_unit_mask = 0; s_e5p_changed_within = 0;
        s_e5p_outside_at_open = s_e5p_outside_reads;
        s_e5p_outside_reads = 0;
        memset(s_e5p_first_ok, 0, sizeof(s_e5p_first_ok));
        for (int k = 0; k < 16; k++) { s_e5p_pre_val[k] = s_e5p_last_val[k]; s_e5p_pre_f[k] = s_e5p_last_f[k]; }
        if (s_e5p_detail) {
            char ps[64];
            e5p_kv(ps, sizeof(ps), s_e5p_pre_val, s_e5p_pre_f, 1);
            debug_log(P1054_TAG " open win=%u f=%d ev=%c D=%u d=%u coalesced=%u fi_pending=%d fi_inflight=%d acked=%u"
                      " e7_sent=0x%02x e7_valid=%d gen=%u pre=%s\n",
                      (unsigned)s_e5p_win, g_mx68k_frame_num, ev, (unsigned)s_rl_D_events, (unsigned)s_rl_dl_events,
                      (unsigned)s_rl_fi_coalesced, s_rl_fdd_int_pending, s_rl_fi_inflight, (unsigned)s_rl_fi_acked,
                      (unsigned)s_rl_e7_sent, s_rl_e7_sent_valid, (unsigned)mx68k_floppyu_usb_generation(), ps);
        }
    } else if (s_e5p_detail) {
        debug_log(P1054_TAG " ev win=%u f=%d ev=%c D=%u d=%u coalesced=%u acked=%u\n",
                  (unsigned)s_e5p_win, g_mx68k_frame_num, ev, (unsigned)s_rl_D_events, (unsigned)s_rl_dl_events,
                  (unsigned)s_rl_fi_coalesced, (unsigned)s_rl_fi_acked);
    }
    s_e5p_n_ev++;
    if (ev == 'D') s_e5p_nD++; else s_e5p_nd++;
    s_e5p_last_ev_f = g_mx68k_frame_num;
}

/* ゲストの $E94005 読出しの戻り値確定後に呼ぶ(読むだけ) */
static void e5p_on_read(uint8_t core_part, uint8_t dev_part, int src)
{
    uint8_t key = s_p955_dev_ctrl & 0x0Fu;
    int data_ok = (src == E5P_SRC_USB || src == E5P_SRC_CACHE || src == E5P_SRC_WRWAIT);
    s_e5p_reads_total++;
    s_e5p_by_src_total[src]++;
    if (s_e5p_open) {
        s_e5p_reads++;
        s_e5p_by_src[src]++;
        if (src != E5P_SRC_NOBIT) s_e5p_reads_assigned++;
        if (key != 0u && !e5p_single_bit(key)) s_e5p_multibit++;
        if (e5p_single_bit(key) && src != E5P_SRC_NOBIT) s_e5p_unit_mask |= key;
        if (e5p_single_bit(key) && data_ok) {
            if (s_e5p_first_ok[key] && s_e5p_wlast_val[key] != dev_part) s_e5p_changed_within |= key;
            if (!s_e5p_first_ok[key]) { s_e5p_first_ok[key] = 1; s_e5p_first_val[key] = dev_part; }
            s_e5p_wlast_val[key] = dev_part;
        }
        uint32_t seq = ++s_e5p_seq;
        if (s_e5p_detail && s_e5p_rd_lines < E5P_RD_PER_WIN_MAX) {
            s_e5p_rd_lines++;
            s_e5p_rd_lines_total++;
            debug_log(P1054_TAG " rd win=%u seq=%u f=%d dt_f=%d core_ctrl=0x%02x dev_ctrl=0x%02x key=0x%x core_part=0x%02x"
                      " dev_part=0x%02x ret=0x%02x src=%s acked=%u e7_sent=0x%02x state=%s PC=0x%06x\n",
                      (unsigned)s_e5p_win, (unsigned)seq, g_mx68k_frame_num, g_mx68k_frame_num - s_e5p_f_open,
                      (unsigned)s_p955_core_ctrl, (unsigned)s_p955_dev_ctrl, (unsigned)key, (unsigned)core_part,
                      (unsigned)dev_part, (unsigned)(core_part | dev_part), E5P_SRC_NAME[src], (unsigned)s_rl_fi_acked,
                      (unsigned)s_rl_e7_sent, RL_STATE_NAME[s_rl_state], (unsigned)(m68000_get_reg(M68K_PC) & 0xFFFFFFu));
        } else {
            s_e5p_lines_dropped++;
            s_e5p_rd_dropped_total++;
        }
    } else {
        s_e5p_outside_reads++;
    }
    if (e5p_single_bit(key) && data_ok) {
        s_e5p_last_val[key] = dev_part;
        s_e5p_last_f[key] = g_mx68k_frame_num;
    }
}

/* rl_tick の末尾から毎フレーム呼ぶ */
static void e5p_tick(void)
{
    if (s_e5p_open && g_mx68k_frame_num - s_e5p_last_ev_f >= E5P_CLOSE_FRAMES) e5p_close("timeout");
}

static void e5p_log_sum(const char* why)
{
    debug_log(P1054_TAG " sum f=%d why=%s wins=%u wins_logged=%u open_now=%d reads_total=%u"
              " by_src=usb:%u,cache:%u,wrwait:%u,nobit:%u,norelay:%u,usbfail:%u rd_lines=%u rd_dropped=%u"
              " win_sum_lines=%u win_sum_dropped=%u\n",
              g_mx68k_frame_num, why, (unsigned)s_e5p_win, (unsigned)s_e5p_wins_logged, s_e5p_open,
              (unsigned)s_e5p_reads_total, (unsigned)s_e5p_by_src_total[E5P_SRC_USB],
              (unsigned)s_e5p_by_src_total[E5P_SRC_CACHE], (unsigned)s_e5p_by_src_total[E5P_SRC_WRWAIT],
              (unsigned)s_e5p_by_src_total[E5P_SRC_NOBIT], (unsigned)s_e5p_by_src_total[E5P_SRC_NORELAY],
              (unsigned)s_e5p_by_src_total[E5P_SRC_USBFAIL], (unsigned)s_e5p_rd_lines_total,
              (unsigned)s_e5p_rd_dropped_total, (unsigned)s_e5p_winsum_lines, (unsigned)s_e5p_winsum_dropped);
}

/* ===========================================================================
 *  P1055: 交換後READ柵のユニット別化 [P1055-ATTR](U-9 解消、normal のときだけ状態を変える)
 * ---------------------------------------------------------------------------
 *  'D' で割当て全ユニットを判別保留(閉扱い)にし、ゲスト自身の $E94005 走査(src=usb・単一ビット・割当てキー)で
 *  割当て全ユニットの値が揃い、かつ 'd' がちょうど1回届いた時点で判定する。全条件成立なら変化ユニット(ちょうど1つ)
 *  だけエポックを進め、他ユニットは保留解除で元の開閉状態へ戻る(★「開く」方向の遷移はここだけ)。
 *  どれか不成立・想定外の順序・打切りは全て全閉(ue[全]++、現行 U-9 挙動と同じ安全側)。
 *  基準値は「完全な走査(A⊆hold_read かつ amb==0)を伴う判定」でだけ更新し、それ以外の打切りは A 全体を無効化する
 *  (Fable5 C-1。規則は rl_nm_base_settle の1箇所に集約し、全閉は全て rl_nm_attr_abort を通す)
 * =========================================================================== */
static uint8_t rl_nm_assigned_keys(void) { return (uint8_t)(p955_ctrl_mx_to_dev(s_p955_mask) & 0x0Fu); }
static int rl_nm_key_unit(uint8_t key) { for (int u = 0; u < 4; u++) if ((key >> u) & 1u) return u; return 0; }
static int rl_nm_popcount4(uint8_t m) { int n = 0; for (int u = 0; u < 4; u++) n += (m >> u) & 1u; return n; }

/* 詳細行の予算(超過分は lines_dropped で数える) */
static int rl_nm_attr_line_ok(void)
{
    if (s_nm_at_lines >= FU_NM_ATTR_LINE_MAX) { s_nm_at_dropped++; return 0; }
    s_nm_at_lines++;
    return 1;
}
/* "k1:xx/ok,k2:xx/ok"(装置ユニット0/1) */
static void rl_nm_kv(char* out, size_t sz, const uint8_t* val, const int* ok)
{
    snprintf(out, sz, "k1:%02x/%d,k2:%02x/%d", (unsigned)val[0], ok[0], (unsigned)val[1], ok[1]);
}

/* 基準値更新規則(全経路共通)。scan_trusted=1(判定経由)かつ完全な走査なら hold_val を基準値へ、それ以外は A 全体を無効化 */
static void rl_nm_base_settle(int scan_trusted)
{
    uint8_t a = rl_nm_assigned_keys();
    int complete = scan_trusted && (a & (uint8_t)~s_nm_hold_read) == 0u && !s_nm_hold_amb;
    for (int u = 0; u < 4; u++) {
        if (!((a >> u) & 1u)) continue;
        if (complete) { s_nm_base_val[u] = s_nm_hold_val[u]; s_nm_base_ok[u] = 1; }
        else          s_nm_base_ok[u] = 0;
    }
}
static void rl_nm_hold_clear(void)
{
    s_nm_st = NM_ST_I;
    s_nm_hold_f = 0; s_nm_hold_rf = 0;
    s_nm_hold_nD = 0; s_nm_hold_nd = 0; s_nm_hold_co0 = 0;
    s_nm_hold_read = 0; memset(s_nm_hold_val, 0, sizeof(s_nm_hold_val));
    s_nm_hold_amb = 0; s_nm_hold_multibit = 0; s_nm_hold_pc = 0;
    s_nm_oc_mask = 0;
    memset(s_nm_oc_frame, 0, sizeof(s_nm_oc_frame));
}

/* 全閉の共通処理。ue[全]++・保留文脈破棄・st=I・oc_mask=0・基準値更新規則の適用を必ずここで行う。
 * why_cl は閉鎖記録の種別(attr_all / gen_changed / startup / install_reset)、clear は fo_valid を落とすか。
 * gen/install は基準値を全ユニット無効化する(装置・媒体の対応が切れるため) */
static void rl_nm_attr_abort(int ar, int why_cl, int clear, int scan_trusted, uint8_t was)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    int prev = s_nm_st;
    if (ar == NM_AR_GEN || ar == NM_AR_INSTALL) memset(s_nm_base_ok, 0, sizeof(s_nm_base_ok));
    else if (prev != NM_ST_I)                   rl_nm_base_settle(scan_trusted);
    s_nm_at_ar[ar]++;
    if (prev != NM_ST_I) {   /* 保留の終わり方の内訳(scan_trusted=1 は判定手順3 経由のみ) */
        if (scan_trusted)              s_nm_at_res_all++;
        else if (ar == NM_AR_MULTI_D)  s_nm_at_end_multi_D++;
        else                           s_nm_at_end_other++;   /* timeout/timeout_max/gen/install */
    }
    rl_nm_hold_clear();
    rl_nm_fence_close(-1, why_cl, 1, clear, was, NM_AR_NAME[ar]);
}

/* 保留開始(E1、I 列)。co_before はこの 'D' 処理前の coalesced 計数 */
static void rl_nm_hold_begin(uint32_t co_before, int st_prev)
{
    s_nm_st = NM_ST_H1;
    s_nm_hold_f = g_mx68k_frame_num;
    s_nm_hold_rf = g_mx68k_frame_num;
    s_nm_hold_nD = 1; s_nm_hold_nd = 0; s_nm_hold_co0 = co_before;
    s_nm_hold_read = 0; memset(s_nm_hold_val, 0, sizeof(s_nm_hold_val));
    s_nm_hold_amb = 0; s_nm_hold_multibit = 0; s_nm_hold_pc = 0;
    s_nm_at_holds++;
    if (rl_nm_attr_line_ok()) {
        char bs[48];
        rl_nm_kv(bs, sizeof(bs), s_nm_base_val, s_nm_base_ok);
        debug_log(P1055_TAG " hold f=%d D=%u d=%u co=%u st_prev=%s base=%s oc=0x%x ue=%u,%u,%u,%u\n",
                  g_mx68k_frame_num, (unsigned)s_rl_D_events, (unsigned)s_rl_dl_events, (unsigned)co_before,
                  NM_ST_NAME[st_prev], bs, (unsigned)s_nm_oc_mask, (unsigned)s_nm_ue[0], (unsigned)s_nm_ue[1],
                  (unsigned)s_nm_ue[2], (unsigned)s_nm_ue[3]);
    }
}

/* 判定手順(H1 で走査完了かつ 'd' 済み、または H2 で 'd' 受信時)。trigger は "read" / "d" */
static void rl_nm_attr_resolve(const char* trigger)
{
    uint8_t a = rl_nm_assigned_keys();
    uint8_t changed = 0;
    int base_all = 1;
    for (int u = 0; u < 4; u++) {
        if (!((a >> u) & 1u)) continue;
        if (!s_nm_base_ok[u]) { base_all = 0; continue; }
        if (s_nm_hold_val[u] != s_nm_base_val[u]) changed |= (uint8_t)(1u << u);
    }
    /* 保留外で先に見えた変化の和集合(既に閉じたユニットも除外しない)。対象は「'D' が処理される前の最大1フレームの競合」
     * だけ=保留開始の同一フレームか直前1フレームに立ったビットのみ。それより古いビットは無視する(無視しても最悪は
     * zero_changed/multi_changed の全閉=安全側。古いビットで別ユニットの 'D' を unit 判定してしまう欠陥の防止) */
    uint8_t oc_used = 0;
    for (int u = 0; u < 4; u++)
        if (((s_nm_oc_mask & a) >> u) & 1u) {
            int age = s_nm_hold_f - s_nm_oc_frame[u];
            if (age >= 0 && age <= FU_NM_OC_FRESH_FRAMES) oc_used |= (uint8_t)(1u << u);
        }
    changed |= oc_used;
    int ar = -1;
    if ((a & (uint8_t)~s_nm_hold_read) != 0u)       ar = NM_AR_NOREAD;
    else if (s_nm_hold_nD != 1u)                    ar = NM_AR_MULTI_D;
    else if (s_nm_hold_nd != 1u)                    ar = NM_AR_ND;
    else if (s_nm_hold_amb)                         ar = NM_AR_AMB;
    else if (s_nm_hold_multibit)                    ar = NM_AR_MULTIBIT;
    else if (s_rl_fi_coalesced != s_nm_hold_co0)    ar = NM_AR_COALESCED;
    else if (!base_all)                             ar = NM_AR_NO_BASE;
    else if (changed == 0u)                         ar = NM_AR_ZERO;
    else if (rl_nm_popcount4(changed) != 1)         ar = NM_AR_MULTI_CH;
    /* ログ用に保留文脈を退避(判定後に破棄されるため) */
    int dt_f = g_mx68k_frame_num - s_nm_hold_f;
    uint8_t hv[4], bv[4], rd = s_nm_hold_read, oc = s_nm_oc_mask;
    int bo[4], ho[4], amb = s_nm_hold_amb, mb = s_nm_hold_multibit;
    uint32_t nD = s_nm_hold_nD, nd = s_nm_hold_nd, cod = s_rl_fi_coalesced - s_nm_hold_co0, pc = s_nm_hold_pc;
    memcpy(hv, s_nm_hold_val, 4); memcpy(bv, s_nm_base_val, 4); memcpy(bo, s_nm_base_ok, sizeof(bo));
    for (int u = 0; u < 4; u++) ho[u] = (rd >> u) & 1u;
    s_nm_at_dtf[dt_f <= 0 ? 0 : dt_f == 1 ? 1 : dt_f <= 5 ? 2 : 3]++;
    int unit = -1;
    uint8_t was = rl_nm_open_mask_nohold();
    if (ar < 0) {
        /* ★唯一の「開く」分岐: 変化ユニットだけ閉じたまま(ue++)、他ユニットは保留解除 */
        unit = rl_nm_key_unit(changed);
        rl_nm_base_settle(1);
        s_nm_at_res_unit++;
        rl_nm_hold_clear();
        rl_nm_fence_close(unit, NM_CL_ATTR_UNIT, 1, 0, was, NULL);
    } else {
        rl_nm_attr_abort(ar, NM_CL_ATTR_ALL, 0, 1, was);
    }
    if (rl_nm_attr_line_ok()) {
        char bs[48], hs[48];
        rl_nm_kv(bs, sizeof(bs), bv, bo);
        rl_nm_kv(hs, sizeof(hs), hv, ho);
        debug_log(P1055_TAG " resolve f=%d dt_f=%d trigger=%s verdict=%s reason=%s unit=%d changed=0x%x oc=0x%x oc_used=0x%x base=%s hold=%s"
                  " read=0x%x nD=%u nd=%u co_delta=%u multibit=%d amb=%d ue=%u,%u,%u,%u PC=0x%06x\n",
                  g_mx68k_frame_num, dt_f, trigger, ar < 0 ? "unit" : "all", ar < 0 ? "ok" : NM_AR_NAME[ar], unit,
                  (unsigned)changed, (unsigned)oc, (unsigned)oc_used, bs, hs, (unsigned)rd, (unsigned)nD, (unsigned)nd, (unsigned)cod, mb, amb,
                  (unsigned)s_nm_ue[0], (unsigned)s_nm_ue[1], (unsigned)s_nm_ue[2], (unsigned)s_nm_ue[3], (unsigned)pc);
    }
}

/* E1/E2: 'D'/'d' 受信時(既存の計数・記録の直後、e5p_on_event の前)。co_before は case 先頭で退避した coalesced */
static void rl_nm_attr_on_ev(char ev, uint32_t co_before)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    if (ev == 'D') {
        int prev = s_nm_st;
        if (prev != NM_ST_I) {
            uint8_t was = rl_nm_open_mask_nohold();
            if (rl_nm_attr_line_ok())
                debug_log(P1055_TAG " abort f=%d reason=multi_D st_prev=%s dt_f=%d read=0x%x nd=%u\n",
                          g_mx68k_frame_num, NM_ST_NAME[prev], g_mx68k_frame_num - s_nm_hold_f,
                          (unsigned)s_nm_hold_read, (unsigned)s_nm_hold_nd);
            rl_nm_attr_abort(NM_AR_MULTI_D, NM_CL_ATTR_ALL, 0, 0, was);
        }
        rl_nm_hold_begin(co_before, prev);
        return;
    }
    if (s_nm_st == NM_ST_I) {
        uint8_t was = rl_nm_open_mask_nohold();
        if (rl_nm_attr_line_ok()) debug_log(P1055_TAG " unpaired_d f=%d\n", g_mx68k_frame_num);
        rl_nm_attr_abort(NM_AR_UNPAIRED_D, NM_CL_ATTR_ALL, 0, 0, was);
        return;
    }
    s_nm_hold_nd++;
    if (s_nm_hold_nd >= 2u) s_nm_hold_amb = 1;
    if (s_nm_st == NM_ST_H2) rl_nm_attr_resolve("d");
}

/* E3: ゲストの $E94005 読出し(p955_route_read case 0x05、e5p_on_read の直前)。戻り値・分岐には影響しない */
static void rl_nm_attr_on_e5_read(uint8_t dev_part, int src)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    uint8_t key = s_p955_dev_ctrl & 0x0Fu;
    uint8_t a = rl_nm_assigned_keys();
    int eligible = e5p_single_bit(key) && (key & a) != 0u && src == E5P_SRC_USB;
    if (s_nm_st == NM_ST_I) {
        if (!eligible) return;
        int u = rl_nm_key_unit(key);
        if (s_nm_base_ok[u] && s_nm_base_val[u] != dev_part) {
            uint8_t was = rl_nm_open_mask_nohold();
            uint8_t bv = s_nm_base_val[u];
            s_nm_oc_mask |= key;
            s_nm_oc_frame[u] = g_mx68k_frame_num;
            s_nm_at_outside++;
            rl_nm_fence_close(u, NM_CL_OUTSIDE, 1, 0, was, NULL);
            if (rl_nm_attr_line_ok())
                debug_log(P1055_TAG " outside_change f=%d unit=%d base=%02x val=%02x ue=%u oc=0x%x PC=0x%06x\n",
                          g_mx68k_frame_num, u, (unsigned)bv, (unsigned)dev_part, (unsigned)s_nm_ue[u],
                          (unsigned)s_nm_oc_mask, (unsigned)(m68000_get_reg(M68K_PC) & 0xFFFFFFu));
        }
        s_nm_base_val[u] = dev_part;
        s_nm_base_ok[u] = 1;
        return;
    }
    if (!eligible) {
        if ((key & a) != 0u && !e5p_single_bit(key)) s_nm_hold_multibit = 1;
        return;
    }
    int u = rl_nm_key_unit(key);
    if ((s_nm_hold_read & key) && s_nm_hold_val[u] != dev_part) s_nm_hold_amb = 1;
    s_nm_hold_val[u] = dev_part;
    s_nm_hold_read |= key;
    s_nm_hold_pc = m68000_get_reg(M68K_PC) & 0xFFFFFFu;
    if (s_nm_st == NM_ST_H1 && (a & (uint8_t)~s_nm_hold_read) == 0u) {
        if (s_nm_hold_nd >= 1u) rl_nm_attr_resolve("read");
        else                    s_nm_st = NM_ST_H2;
    }
}

/* E4: rl_tick の rl_fdd_int_service() の後に毎フレーム呼ぶ。打切りの起点は割込みを実際に発行したフレーム(Fable5 C-3) */
static void rl_nm_attr_tick(void)
{
    if (s_rl_write_mode != WRM_NORMAL || s_nm_st == NM_ST_I) return;
    int now = g_mx68k_frame_num;
    if (s_rl_fdd_int_pending) s_nm_hold_rf = now;
    int ar = -1;
    if (now - s_nm_hold_rf >= FU_NM_HOLD_FRAMES)         ar = NM_AR_TIMEOUT;
    else if (now - s_nm_hold_f >= FU_NM_HOLD_MAX_FRAMES) ar = NM_AR_TIMEOUT_MAX;
    if (ar < 0) return;
    if (rl_nm_attr_line_ok())
        debug_log(P1055_TAG " timeout f=%d dt_f=%d dt_rf=%d st=%s read=0x%x nd=%u reason=%s\n",
                  now, now - s_nm_hold_f, now - s_nm_hold_rf, NM_ST_NAME[s_nm_st], (unsigned)s_nm_hold_read,
                  (unsigned)s_nm_hold_nd, NM_AR_NAME[ar]);
    /* H2(走査完了)でも 'd' 未着=線の状態が不明なので基準値は無効化する(scan_trusted=0) */
    rl_nm_attr_abort(ar, NM_CL_ATTR_ALL, 0, 0, rl_nm_open_mask_nohold());
}

/* E5/E6: USB 世代変化(rl_reset_link_state)・install(rl_install_reset)。閉鎖記録は既存の gen_changed/startup/install_reset */
static void rl_nm_attr_reset(int ar, int why_cl, int clear, uint8_t was)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    if (s_nm_st != NM_ST_I && rl_nm_attr_line_ok())
        debug_log(P1055_TAG " abort f=%d reason=%s st_prev=%s dt_f=%d read=0x%x nd=%u\n",
                  g_mx68k_frame_num, NM_AR_NAME[ar], NM_ST_NAME[s_nm_st], g_mx68k_frame_num - s_nm_hold_f,
                  (unsigned)s_nm_hold_read, (unsigned)s_nm_hold_nd);
    rl_nm_attr_abort(ar, why_cl, clear, 0, was);
}

static void rl_nm_attr_log_sum(void)
{
    /* 恒等式: holds = resolved_unit + resolved_all + ended_multi_D + ended_other(timeout/timeout_max/gen/install) + open_now */
    int open_now = s_nm_st != NM_ST_I;
    int ident = s_nm_at_holds == s_nm_at_res_unit + s_nm_at_res_all + s_nm_at_end_multi_D + s_nm_at_end_other + (uint32_t)open_now;
    debug_log(P1055_TAG " sum f=%d holds=%u resolved_unit=%u resolved_all=%u ended_multi_D=%u ended_other=%u open_now=%d ident_ok=%d"
              " all_by_reason=noread:%u,multi_D:%u,nd:%u,amb:%u,multibit:%u,coalesced:%u,no_base:%u,zero_changed:%u,"
              "multi_changed:%u,timeout:%u,timeout_max:%u,unpaired_d:%u reset_gen=%u reset_install=%u outside_change=%u"
              " dtf_hist=0:%u,1:%u,2-5:%u,6+:%u ref_attr_hold=%u st=%s ue=%u,%u,%u,%u lines_dropped=%u\n",
              g_mx68k_frame_num, (unsigned)s_nm_at_holds, (unsigned)s_nm_at_res_unit, (unsigned)s_nm_at_res_all,
              (unsigned)s_nm_at_end_multi_D, (unsigned)s_nm_at_end_other, open_now, ident,
              (unsigned)s_nm_at_ar[NM_AR_NOREAD], (unsigned)s_nm_at_ar[NM_AR_MULTI_D], (unsigned)s_nm_at_ar[NM_AR_ND],
              (unsigned)s_nm_at_ar[NM_AR_AMB], (unsigned)s_nm_at_ar[NM_AR_MULTIBIT], (unsigned)s_nm_at_ar[NM_AR_COALESCED],
              (unsigned)s_nm_at_ar[NM_AR_NO_BASE], (unsigned)s_nm_at_ar[NM_AR_ZERO], (unsigned)s_nm_at_ar[NM_AR_MULTI_CH],
              (unsigned)s_nm_at_ar[NM_AR_TIMEOUT], (unsigned)s_nm_at_ar[NM_AR_TIMEOUT_MAX], (unsigned)s_nm_at_ar[NM_AR_UNPAIRED_D],
              (unsigned)s_nm_at_ar[NM_AR_GEN], (unsigned)s_nm_at_ar[NM_AR_INSTALL], (unsigned)s_nm_at_outside,
              (unsigned)s_nm_at_dtf[0], (unsigned)s_nm_at_dtf[1], (unsigned)s_nm_at_dtf[2], (unsigned)s_nm_at_dtf[3],
              (unsigned)s_nm_ref_attr_hold, NM_ST_NAME[s_nm_st], (unsigned)s_nm_ue[0], (unsigned)s_nm_ue[1],
              (unsigned)s_nm_ue[2], (unsigned)s_nm_ue[3], (unsigned)s_nm_at_dropped);
}

static int32_t rl_fdd_int_cb(uint8_t irq);

/* イベント行。各行は IRQ1 の生の状態(irq1/cb_fdc/cb_fdd/intstat)を自分で持つ */
static void rl_fi_log_ev(const char* ev)
{
    if (s_rl_fi_lines >= FU_LOG_LIMIT) return;
    s_rl_fi_lines++;
    debug_log(P1052_TAG " ev=%s f=%d state=%s irq1=%d cb_fdc=%d cb_fdd=%d intstat=0x%02x pending=%d inflight=%d"
              " PC=0x%06x SR=0x%04x\n",
              ev, g_mx68k_frame_num, RL_STATE_NAME[s_rl_state], (int)IRQH_IRQ[1],
              IRQH_CallBack[1] == (void*)FDC_Int ? 1 : 0, IRQH_CallBack[1] == (void*)rl_fdd_int_cb ? 1 : 0,
              (unsigned)IOC_IntStat, s_rl_fdd_int_pending, s_rl_fi_inflight,
              (unsigned)m68000_get_reg(M68K_PC), (unsigned)m68000_get_reg(M68K_SR));
}

/* ゲストがFDD割込みを受理したときに呼ばれる。数えてから Core の FDD_Int へそのまま委譲する(ベクタ $61) */
static int32_t rl_fdd_int_cb(uint8_t irq)
{
    if (s_rl_fi_inflight) {
        s_rl_fi_acked++;
        s_rl_fi_inflight = 0;
        rl_fi_log_ev("ack");
    } else {
        /* インストール時リセットで inflight を捨てた後に、IRQ1 に残っていた要求が受理された(reset_drop_inflight 側で計上済み) */
        s_rl_fi_ack_after_reset++;
        rl_fi_log_ev("ack_after_reset");
    }
    return FDD_Int(irq);
}

static void rl_raise_int(void)
{
    /* P1028: 保留中の'C'由来割込みは、この発行1回にまとめる(割込み線は1本なので二重発行しない) */
    if (s_rl_c_irq_deferred) {
        s_rl_c_irq_deferred = 0;
        s_p1028_merged++;
    }
    s_rl_c_irq_live = 0;
    /* Core:fdc.c:137-141 FDC_SetInt と同じ発行(IOC bit7 共有) */
    IOC_IntStat |= 0x80;
    if (IOC_IntStat & 4) {
        /* P1052: FDCを優先する。保留中のFDD割込みを実際に上書きする場合だけ、次の安全点で出し直す(Fable5 C-1) */
        if (IRQH_IRQ[1] && IRQH_CallBack[1] == (void*)rl_fdd_int_cb) {
            s_rl_fdd_int_pending = 1;
            s_rl_fi_inflight = 0;
            s_rl_fi_requeued++;
            rl_fi_log_ev("requeue");
        }
        IRQH_Int(1, &FDC_Int);
    }
    s_rl_int_raised++;
}

/* P1028: 保留中の'C'由来割込みを再発行する。why は集計先の選択とログ用 */
static void rl_c_irq_flush(const char* why)
{
    if (!s_rl_c_irq_deferred) return;
    s_rl_c_irq_deferred = 0;
    if      (strcmp(why, "result_read") == 0) s_p1028_reraise_result++;
    else if (strcmp(why, "seek") == 0)        s_p1028_reraise_seek++;
    else if (strcmp(why, "tick") == 0)        s_p1028_reraise_tick++;
    else                                      s_p1028_reraise_nonrelay++;
    rl_raise_int();
    s_rl_c_irq_live = (IRQH_IRQ[1] && IRQH_CallBack[1] == (void*)FDC_Int) ? 1 : 0;
    if (s_p1028_lines < FU_LOG_LIMIT) {
        s_p1028_lines++;
        debug_log(P1028_DEFER_TAG " reraise f=%d why=%s defer_f=%d dt_frames=%d irq1=%d PC=0x%06x SR=0x%04x\n",
                  g_mx68k_frame_num, why, s_p1028_defer_f, g_mx68k_frame_num - s_p1028_defer_f,
                  (int)IRQH_IRQ[1], (unsigned)m68000_get_reg(M68K_PC), (unsigned)m68000_get_reg(M68K_SR));
    }
}

/* P1052: 'D'由来のFDD割込みの発行(rl_tick の安全点から毎フレーム呼ぶ)。発行式は Core:fdd.c:117/:205 と同じ */
static void rl_fdd_int_service(void)
{
    /* Fable5 C-2: 発行済み・未受理なのに IRQ1 が自分でない=振り分け器の外による上書き。出し直す */
    if (s_rl_fi_inflight && !(IRQH_IRQ[1] && IRQH_CallBack[1] == (void*)rl_fdd_int_cb)) {
        s_rl_fi_inflight = 0;
        s_rl_fdd_int_pending = 1;
        s_rl_fi_requeued_ext++;
        rl_fi_log_ev("requeue_ext");
    }
    if (!s_rl_fdd_int_pending) { s_rl_fi_streak = 0; return; }
    /* 持ち越し(理由別、Fable5 C-5)。読出し待ち(RD_WAIT/RID_WAIT)では持ち越さない(Fable5 C-3) */
    int defer = 1;
    if (IRQH_IRQ[1])                                 s_rl_fi_deferred_irq1++;
    else if (s_rl_c_irq_deferred)                    s_rl_fi_deferred_cdef++;
    else if (s_rl_sync_active || rl_in_write_wait()) s_rl_fi_deferred_wr++;
    else                                             defer = 0;
    if (defer) {
        if (++s_rl_fi_streak > s_rl_fi_streak_max) s_rl_fi_streak_max = s_rl_fi_streak;
        return;
    }
    s_rl_fdd_int_pending = 0;
    s_rl_fi_streak = 0;
    if (IOC_IntStat & 2) {
        IRQH_Int(1, &rl_fdd_int_cb);
        s_rl_fi_raised++;
        s_rl_fi_inflight = 1;
        rl_fi_log_ev("raise");
    } else {
        s_rl_fi_masked++;   /* Core:FDD_SetFDInt と同じく、許可されていなければ捨てる */
        rl_fi_log_ev("mask");
    }
}

static void rl_set_result(const uint8_t* r, uint32_t n)
{
    memcpy(s_rl_rsp, r, n);
    s_rl_rdptr = 0;
    s_rl_rdnum = n;
    s_rl_state = n ? RL_RESULT : RL_IDLE;
}

/* P1058: 書込み系の合成エラーの直近記録(rl_e5_write の排出ビット抑止窓の起点) */
static int      s_rl_se_valid = 0;
static int      s_rl_se_f     = 0;      /* 合成時のエミュフレーム */
static uint8_t  s_rl_se_us_mx = 0;      /* 合成時のユニット選択(ログ用、窓判定はユニット非依存) */
static uint8_t  s_rl_se_st1   = 0;
static uint8_t  s_rl_se_cmd   = 0;
static uint32_t s_rl_e5_ej_supp = 0;    /* 抑止回数カウンタ(sumログ用) */
static uint32_t s_rl_se_n     = 0;      /* 書込み系合成エラーの発生回数累積(自己反証可能性の分母) */

/* P1058: 書込み系(WRITE DATA/WRITE DELETED/FORMAT)のエラーをゲストへ返した記録(排出ビット抑止窓の起点)。
 * P1087(C'-E): rl_synth_err から切り出し(挙動同一)、装置の本物の異常終了を素通しするとき(device_err_nm 系)にも呼ぶ。
 * ゲストからは合成エラーと本物のエラーを区別できないため、書込み系エラー直後は排出を中継しないという不変条件を統一する */
static void rl_se_note(uint8_t st1)
{
    uint8_t cmd5 = s_rl_cmd[0] & 0x1Fu;
    if (cmd5 == 5u || cmd5 == 9u || cmd5 == 13u) {
        s_rl_se_valid = 1;
        s_rl_se_f = g_mx68k_frame_num;
        s_rl_se_us_mx = s_rl_us_mx;
        s_rl_se_st1 = st1;
        s_rl_se_cmd = s_rl_cmd[0];
        s_rl_se_n++;
    }
}

/* 合成エラー: ST0=0x40|US_mx, ST1=st1, ST2=0, C/H/R/N=パラメータエコー(XM6t fdc_bridger.h:851-858) */
static void rl_synth_err(uint8_t st1, int with_chrn)
{
    uint8_t r[FU_RESULT_LEN] = { 0 };
    r[0] = (uint8_t)(0x40u | s_rl_us_mx);
    r[1] = st1;
    if (with_chrn) { r[3] = s_rl_cmd[2]; r[4] = s_rl_cmd[3]; r[5] = s_rl_cmd[4]; r[6] = s_rl_cmd[5]; }
    rl_set_result(r, FU_RESULT_LEN);
    rl_se_note(st1);
}

/* P1087(D): 書込み系の異常で bringup を無効化した直後に呼ぶ。次のゲストコマンドが no_bringup でスタブ NR
 * (「ディスクが入っていません」)へ落ちるのを避けるため、p955_resolve_pending で1回だけ inline 再 bringup を許す。
 * install/reset 経路・selftest フックでは呼ばない(リセット後は従来どおり tick で再 bringup) */
static void rl_rebringup_arm(const char* trigger)
{
    s_rl_rebringup_pending = 1;
    s_rl_rebringup_gen = mx68k_floppyu_usb_generation();
    s_rl_rebringup_trigger = trigger;
}

/* ST3 の装置→MX変換。ST3 は bit7=FT・bit6=WP なので p955_st_dev_to_mx の IC 判定を使わず、常に下位2bitを変換 */
static uint8_t rl_st3_dev_to_mx(uint8_t st)
{
    return (uint8_t)((st & ~3u) | (((st & 3u) + s_p955_base) & 3u));
}

static void rl_invalidate_caches(void)
{
    memset(s_rl_sds_valid, 0, sizeof(s_rl_sds_valid));
    s_rl_e5c_valid = 0;
}

static void rl_reset_link_state(void)
{
    e5p_close("gen");   /* P1054: 観測窓の強制終端(観測のみ) */
    /* P1053: 世代変化で柵は閉じる(fo_gen 不一致に加え、P1055 では ue[全]++)。呼出し元が s_rl_gen_seen を更新する前に呼ぶので、
     * 閉鎖前に開いていたユニットは旧世代 s_rl_gen_seen との比較で求める。初回接続(gen_seen 無効)は世代変化ではない。
     * P1055(E5): 判別保留も破棄し、基準値を全ユニット無効化する(全閉の共通処理 rl_nm_attr_abort 経由) */
    if (s_rl_write_mode == WRM_NORMAL && s_rl_gen_seen_valid) {
        uint8_t was = 0;
        for (uint8_t u = 0; u < 4u; u++)
            if (s_nm_fo_valid[u] && s_nm_fo_ue[u] == s_nm_ue[u] && s_nm_fo_gen[u] == s_rl_gen_seen) was |= (uint8_t)(1u << u);
        rl_nm_attr_reset(NM_AR_GEN, NM_CL_GEN, 0, was);
    }
    /* P1089(B-11): 抜去等の MX 起点でない切断では flush できない。dirty なら未確定セクタを uncertain にしてラッチ */
    if (s_fu_wc_dirty) { s_fu_wc_gen_dirty++; rl_wc_discard("gen_changed_dirty"); }
    rl_invalidate_caches();
    rl_nmf_pcn_invalidate(-1, &s_nmf_pcn_inval_all);   /* P1067: 世代変化(gen 比較でも落ちるが明示する) */
    s_rl_cf_first = 1;   /* P1051: 接続し直した装置の状態は不明。would_cache を過大に数えない側へ倒す */
    memset(s_rl_seek_t_us, 0, sizeof(s_rl_seek_t_us));
    rl_dnb_clear_all("link_reset");   /* P1010: 旧接続のシーク完了は届かない */
    s_rl_e5_sent_valid = 0;
    s_rl_e7_sent_valid = 0;
    s_rl_p2_phase = 0;          /* 新しい接続のストリームは先頭から(旧接続の未読は通信層が捨てる) */
    s_rl_p2_kind = RL_P2_NONE;
}

/* P1035: 末尾に読出し開始時の C/H/R/N・装置側ユニット・データFNV(成功時のみ)を追加(既存フィールドは不変) */
static void rl_log_read_end(const char* result, int fnv_valid, uint32_t fnv)
{
    int64_t now = rl_now_us();
    char fh[16];
    if (fnv_valid) snprintf(fh, sizeof(fh), "0x%08x", (unsigned)fnv);
    else           snprintf(fh, sizeof(fh), "-");
    debug_log(P1009_TAG " read end f=%d gen=%u result=%s len=%d got=%u wait_us=%lld wait_frames=%d msr_wait=%u other_bytes=%u"
              " C=%u H=%u R=%u N=%u us_dev=%u data_fnv=%s\n",
              g_mx68k_frame_num, (unsigned)s_rl_op_gen, result,
              s_rl_seen_r ? (int)s_rl_frame_len : -1, (unsigned)(s_rl_seen_r ? s_rl_p2_got : 0),
              (long long)(now - s_rl_op_t0_us), g_mx68k_frame_num - s_rl_op_f0,
              (unsigned)s_rl_msr_wait, (unsigned)s_rl_other_bytes,
              s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2], s_rl_rd_chrn[3], (unsigned)s_rl_rd_us, fh);
}

static void rl_log_rid_end(const char* result, const uint8_t* res)
{
    char mh[2 * 16 + 4], rh[2 * 8 + 4];
    rl_hex(mh, sizeof(mh), s_rl_rid_msr, s_rl_rid_msr_n, 16);
    rl_hex(rh, sizeof(rh), res, res ? FU_RESULT_LEN : 0, FU_RESULT_LEN);
    debug_log(P1009_TAG " rid end f=%d gen=%u result=%s polls=%u msr_seq=%s result_dev=%s wait_us=%lld\n",
              g_mx68k_frame_num, (unsigned)s_rl_op_gen, result, (unsigned)s_rl_rid_polls, mh, rh,
              (long long)(rl_now_us() - s_rl_op_t0_us));
}

/* P1032: 書込み待ち中に受けたイベントを「<文字>@<送信からのus>」の列にする */
static void rl_wr_events_str(char* out, size_t sz)
{
    size_t o = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < s_rl_wr_ev_n && o + 24 < sz; i++)
        o += (size_t)snprintf(out + o, sz - o, "%s%c@%lld", i ? "," : "", s_rl_wr_ev_c[i], (long long)s_rl_wr_ev_us[i]);
    if (o == 0) snprintf(out, sz, "-");
}

/* P1032: 書込み待ちを結果無しで終えるときの end 行(呼出し側でラッチを立ててから呼ぶ) */
static void rl_wr_log_end(const char* result, const char* outcome)
{
    char ev[FU_WR_EV_MAX * 24 + 4], mh[2 * 16 + 4];
    rl_wr_events_str(ev, sizeof(ev));
    /* P1033: 結果待ちで収集済みの装置MSR列とポーリング回数も出す(新規のUSB通信は行わない) */
    rl_hex(mh, sizeof(mh), s_rl_wr_res_msr, s_rl_wr_res_msr_n, 16);
    if (s_rl_wr_lines < FU_LOG_LIMIT) {
        s_rl_wr_lines++;
        debug_log(P1032_TAG " end f=%d gen=%u op_gen=%u state=%s result=%s write_outcome=%s events=%s data_sent=%d latched=%d"
                  " sends=%u elapsed_us=%lld polls=%u msr_seq=%s\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_rl_op_gen,
                  RL_STATE_NAME[s_rl_state], result, outcome, ev, s_rl_wr_data_sent, s_rl_wr_latched_off,
                  (unsigned)s_rl_wr_sends, (long long)(rl_now_us() - s_rl_wr_send_t_us),
                  (unsigned)s_rl_wr_res_polls, s_rl_wr_res_msr_n ? mh : "-");
    }
}

static const char* rl_wr_form(const uint8_t* r);
/* P1037: armed の r 行(送信した書込み1回の outcome)。res=NULL は装置リザルト未取得(結果不明)。P1051: 末尾に form= */
static void rl_ar_log_r(const uint8_t* res, const char* outcome, int form_a, int form_c, uint32_t written_fnv,
                        uint32_t writes)
{
    if (s_rl_ar_r_lines >= FU_AR_LOG_LIMIT) return;
    char rh[2 * 8 + 4];
    const uint8_t* p = s_rl_cmd;
    s_rl_ar_r_lines++;
    if (res) rl_hex(rh, sizeof(rh), res, FU_RESULT_LEN, FU_RESULT_LEN);
    else     snprintf(rh, sizeof(rh), "-");
    debug_log(P1037_TAG " r seq=%u f=%d C=%u H=%u R=%u N=%u result_dev=%s outcome=%s form_a=%d form_c=%d written_fnv=0x%08x"
              " writes_this_sector=%u form=%s\n",
              (unsigned)s_rl_ar_cur_seq, g_mx68k_frame_num, p[2], p[3], p[4], p[5], rh, outcome, form_a,
              form_c, (unsigned)written_fnv, (unsigned)writes, res ? rl_wr_form(res) : "-");
}

/* P1037: 送信後に結果不明・不採用で既存ラッチへ入る経路(rl_abort_op の書込み待ち・送信途中失敗・不採用・
 * 書込み待ち中のリセット)から呼ぶ。armed のときだけ、送信した書込みのセクタを uncertain にして永久停止する */
static void rl_ar_after_unknown(const char* why, const uint8_t* res)
{
    rl_nm_after_unknown(why);   /* P1053: normal のみ(該当セクタを照合対象外にしてラッチ理由を記録) */
    if (s_rl_write_mode != WRM_ARMED) return;
    if (s_rl_ar_cur_valid) {
        rl_ar_sec_t* e = &s_rl_ar_map[s_rl_ar_cur_idx];
        e->uncertain = 1;
        s_rl_ar_cur_valid = 0;
        rl_ar_log_r(res, why, 0, 0, rl_fnv(s_rl_wrcollect_buf, s_rl_wr_n), e->writes);
    }
    rl_ar_disarm(why);
}

/* 進行中の READ/READ ID を打ち切る。aborted_by_cmd 以外は合成エラー+割込み(DMACには一切触れない)。
 * P1032: 書込み待ち(装置へ書込みコマンド送信済み)を打ち切るときは、RE値を問わず同一プロセスでの以後の書込みを
 * 全て停止する(ラッチ)。前のコマンドで装置が送出した古い'W'が受信リングに滞留し、後の新しいコマンドの待ち中に
 * 解釈されると、'W'応答の4条件ではコマンド個体を区別できず新コマンドのデータを送ってしまうため、その状況自体を
 * 作らない(Fable5 M1)。次段(モード1)では、装置応答とコマンドを対応付ける識別手段の導入が必要 */
static void rl_abort_op(int re)
{
    if (rl_in_write_wait()) {
        if (re == RE_MEDIA) s_rl_g3_abort++;   /* P1062: RE_MEDIA は 'D'/'d' 処理の WAIT_W 打切りからのみ */
        s_rl_fmt1_inflight = 0;                /* P1064: spent はそのまま */
        s_rl_wr_latched_off = 1;
        s_rl_bringup_valid = 0;   /* 次tickの再bringupで装置FDCの残留結果フェーズを読み捨てさせる */
        rl_rebringup_arm("abort");   /* P1087(D): 次のゲストコマンド受付時に inline 再 bringup を許す */
        rl_wr_log_end(RE_NAME[re], "unknown");
        rl_ar_after_unknown(RE_NAME[re], NULL);   /* P1037: armed のみ(該当セクタ uncertain+永久停止) */
        if (s_nmf_inflight) { s_nmf_inflight = 0; rl_nmf_halt(RE_NAME[re]); }   /* P1067: FORMAT 側の停止表示も連動 */
        if (re == RE_ABORTED) { s_rl_aborted_by_cmd++; s_rl_state = RL_IDLE; s_rl_rdnum = 0; return; }
        rl_synth_err(ST1_OR, 1);   /* 成功(IC=00)は合成しない */
        rl_raise_int();
        return;
    }
    if (s_rl_state == RL_RD_WAIT || s_rl_state == RL_RD_SUPPLY) {
        s_rl_read_err[re]++;
        rl_log_read_end(RE_NAME[re], 0, 0);
        if (s_rl_p2_kind == RL_P2_TARGET) s_rl_p2_kind = RL_P2_DISCARD;
        if (re == RE_ABORTED) { s_rl_aborted_by_cmd++; s_rl_state = RL_IDLE; s_rl_rdnum = 0; return; }
        rl_synth_err(0, 1);
        rl_raise_int();
        s_rl_synth_after_read = 1;
    } else if (s_rl_state == RL_RID_WAIT) {
        s_rl_rid_err++;
        rl_log_rid_end(RE_NAME[re], NULL);
        if (re == RE_ABORTED) { s_rl_aborted_by_cmd++; s_rl_state = RL_IDLE; s_rl_rdnum = 0; return; }
        rl_synth_err(0, 0);
        rl_raise_int();
    }
}

static void rl_fault(const char* what, int ret)
{
    uint32_t gen = mx68k_floppyu_usb_generation();
    s_rl_fault_valid = 1;
    s_rl_fault_gen = gen;
    s_rl_fault_n++;
    debug_log(P1009_TAG " fault f=%d gen=%u what=%s ret=%d state=%s\n",
              g_mx68k_frame_num, (unsigned)gen, what, ret, RL_STATE_NAME[s_rl_state]);
    if (rl_in_read_phase() || rl_in_write_wait()) rl_abort_op(RE_FAULT);
    rl_dnb_clear_all("fault");   /* P1010: 故障停止中は'C'もSENSE INTも中継されない */
}

/* port1 同期往復(flush_input=1)。-2/-3 はその接続世代の中継を故障停止させる。
 * P1089: 上限は rl_p1_timeout_ms()(dirty 中のみ延長)。受信つき往復の成功で dirty のクリア判定を行う */
static int rl_p1(const char* what, const uint8_t* tx, size_t txlen, uint8_t* rx, size_t rxlen)
{
    int tmo = rl_p1_timeout_ms();
    int64_t t0 = rl_now_us();
    int r = mx68k_floppyu_usb_port1_exec(tx, txlen, rx, rxlen, tmo, 1);
    int64_t dt = rl_now_us() - t0;
    s_rl_blk_frame_us += dt;
    s_rl_p1_calls++;
    if (r == -2) s_rl_p1_timeouts++;
    else if (r == -3) s_rl_p1_errs++;
    rl_p1_after(tmo != FU_P1_TIMEOUT_MS, dt, rxlen, r);
    if (r == -2 || r == -3) rl_fault(what, r);
    return r;
}

/* rl_p1 と同じ往復・集計だが故障停止へ遷移しない(bringup ポーリング専用) */
static int rl_p1_nofault(const uint8_t* tx, size_t txlen, uint8_t* rx, size_t rxlen)
{
    int tmo = rl_p1_timeout_ms();
    int64_t t0 = rl_now_us();
    int r = mx68k_floppyu_usb_port1_exec(tx, txlen, rx, rxlen, tmo, 1);
    int64_t dt = rl_now_us() - t0;
    s_rl_blk_frame_us += dt;
    s_rl_p1_calls++;
    if (r == -2) s_rl_p1_timeouts++;
    else if (r == -3) s_rl_p1_errs++;
    rl_p1_after(tmo != FU_P1_TIMEOUT_MS, dt, rxlen, r);
    return r;
}

/* 中継可能か評価し、判定が変わったときだけ mode 行を出す */
static int rl_relay_ok(void)
{
    int conn = mx68k_floppyu_usb_is_connected();
    uint32_t gen = mx68k_floppyu_usb_generation();
    int claimed = mx68k_floppyu_vslice_claimed();
    int ok = 0;
    const char* reason;
    if (rl_op_needs_device() && (!conn || gen != s_rl_op_gen)) rl_abort_op(RE_DISCONNECT);
    rl_dnb_check_link(conn, gen);   /* P1010 */
    if (conn && (!s_rl_gen_seen_valid || gen != s_rl_gen_seen)) {
        rl_reset_link_state();
        s_rl_gen_seen = gen;
        s_rl_gen_seen_valid = 1;
    }
    if (!s_p955_enabled)                                   reason = "router_off";
    else if (!s_rl_usb_on)                                 reason = "usb_off";
    else if (claimed)                                      reason = "vslice_claimed";
    else if (!conn)                                        reason = "not_connected";
    else if (s_rl_fault_valid && s_rl_fault_gen == gen)    reason = "fault";
    else if (!s_rl_bringup_valid || s_rl_bringup_gen != gen) reason = "no_bringup";
    else { ok = 1; reason = "ok"; s_rl_frame_active = 1; }
    s_rl_reason = reason;
    if (ok != s_rl_last_mode || reason != s_rl_last_reason) {
        s_rl_last_mode = ok;
        s_rl_last_reason = reason;
        /* P1054: usb_env= は後方互換のため残す(値は usb_on と同じ) */
        debug_log(P1009_TAG " mode f=%d gen=%u mode=%s reason=%s connected=%d vslice_claimed=%d enabled=%d usb_env=%d usb_on=%d\n",
                  g_mx68k_frame_num, (unsigned)gen, ok ? "relay" : "stub", reason, conn, claimed,
                  s_p955_enabled, s_rl_usb_on, s_rl_usb_on);
    }
    if (ok && !s_p1028_vec_dumped) {
        /* P1028: 初回 relay 時にベクタ$60(0x60×4=0x180)の実体を1回だけ記録。RAM域なので dma_readmem24 は副作用なし */
        s_p1028_vec_dumped = 1;
        uint32_t v = ((uint32_t)dma_readmem24(0x000180u) << 24) | ((uint32_t)dma_readmem24(0x000181u) << 16)
                   | ((uint32_t)dma_readmem24(0x000182u) << 8) | (uint32_t)dma_readmem24(0x000183u);
        debug_log("[P1028-VEC60] f=%d addr=0x000180 val=0x%08x ioc_intvect=0x%02x\n",
                  g_mx68k_frame_num, (unsigned)v, (unsigned)IOC_IntVect);
    }
    return ok;
}

/* 中継コマンドの全バイトを 83 b の連続として1回の port1_exec で送る(P1004 S7 と同形) */
static int rl_send_cmd(const char* name)
{
    uint8_t tx[20];
    char th[2 * 20 + 4];
    size_t n = 0;
    for (uint32_t i = 0; i < s_rl_cmd_n && n + 2 <= sizeof(tx); i++) {
        tx[n++] = (uint8_t)(FU_WR | FU_REG_DATA);
        tx[n++] = s_rl_cmd[i];
    }
    /* P1089: WRITE DATA/WRITE DELETED/SENSE INT 以外のコマンド1バイト目は装置キャッシュの flush 契機(FW: app.c:2140-2149) */
    uint8_t c1 = s_rl_cmd[0] & 0x1Fu;
    if (c1 != 5u && c1 != 9u && c1 != 8u) rl_wc_note_trigger(WCF_CMD);
    int64_t t0 = rl_now_us();
    int r = rl_p1(name, tx, n, NULL, 0);
    rl_hex(th, sizeof(th), tx, n, 20);
    debug_log(P1009_TAG " cmd f=%d gen=%u name=%s cmd=0x%02x us_mx=0x%02x us_dev=0x%02x tx=%s ret=%d us=%lld\n",
              g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), name, (unsigned)s_rl_cmd[0],
              (unsigned)s_rl_us_mx, (unsigned)(s_rl_cmd[1] & 7u), th, r, (long long)(rl_now_us() - t0));
    return r;
}

/* 送信失敗時の SEEK/RECAL はスタブで処理する(Not Ready の割込み+FIFO に STUB を積む) */
static void rl_fallback_stub(void)
{
    s_p955_owner = P955_OWNER_STUB;
    p955_stub_start(s_rl_cmd[0]);
    for (uint32_t i = 1; i < s_rl_cmd_n; i++) p955_stub_param(s_rl_cmd[i]);
    uint8_t cmd = s_rl_cmd[0] & 0x1Fu;
    if (cmd == 7u || cmd == 15u) p955_int_fifo_push(P955_OWNER_STUB);
}

static void rl_refuse(int nw, const char* why)
{
    uint8_t cmd = s_rl_cmd[0] & 0x1Fu;
    s_rl_refused_hist[cmd]++;
    if (s_rl_refused_lines < FU_LOG_LIMIT) {
        char ph[2 * 10 + 4];
        s_rl_refused_lines++;
        rl_hex(ph, sizeof(ph), s_rl_cmd, s_rl_cmd_n, 10);
        debug_log(P1009_TAG " refused f=%d cmd=0x%02x us_mx=0x%02x reason=%s prm_dev=%s\n",
                  g_mx68k_frame_num, (unsigned)s_rl_cmd[0], (unsigned)s_rl_us_mx, why, ph);
    }
    /* 書込み系は NW(ST1 bit1)。MX独自方針で、実機/XM6t忠実ではない */
    rl_synth_err(nw ? 0x02u : 0x00u, cmd != 13u);
    rl_raise_int();
}

static void rl_read_begin(void)
{
    const uint8_t* p = s_rl_cmd;   /* [0]cmd [1]US/HD [2]C [3]H [4]R [5]N [6]EOT [7]GPL [8]DTL */
    s_rl_rd_d_begin = s_rl_d_events;
    memcpy(s_rl_rd_ue_begin, s_nm_ue, sizeof(s_rl_rd_ue_begin));   /* P1055: 4ユニット分(s_rl_rd_us の設定順に依存しない) */
    s_rl_rd_gen_begin = mx68k_floppyu_usb_generation();
    if (s_rl_prev_chr_valid && s_rl_prev_chr[0] == p[2] && s_rl_prev_chr[1] == p[3] && s_rl_prev_chr[2] == p[4])
        s_rl_retry_same_chr++;
    s_rl_prev_chr[0] = p[2]; s_rl_prev_chr[1] = p[3]; s_rl_prev_chr[2] = p[4];
    s_rl_prev_chr_valid = 1;
    memcpy(s_rl_rd_chrn, p + 2, 4);   /* P1035: read end ログ・読み戻し照合用(後続コマンドで s_rl_cmd が上書きされるため保存) */
    s_rl_rd_us = p[1] & 3u;
    s_rl_read_begin++;
    s_rl_rp_total++;
    debug_log(P1009_TAG " read begin f=%d gen=%u C=%u H=%u R=%u N=%u EOT=%u GPL=0x%02x DTL=0x%02x MT=%u cmd=0x%02x us_mx=0x%02x us_dev=0x%02x"
              " MTC=0x%04x MAR=0x%06x OCR=0x%02x CSR=0x%02x\n",
              g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), p[2], p[3], p[4], p[5], p[6], p[7], p[8],
              (unsigned)(p[0] >> 7), p[0], (unsigned)s_rl_us_mx, (unsigned)(p[1] & 7u),
              (unsigned)DMA[0].MTC, (unsigned)(DMA[0].MAR & 0xFFFFFFu), (unsigned)DMA[0].OCR, (unsigned)DMA[0].CSR);
    if (rl_send_cmd("read") < 0) {
        /* 送信失敗(fault)。装置へは何も届いていない扱いで合成エラー */
        s_rl_op_gen = mx68k_floppyu_usb_generation();
        s_rl_op_t0_us = rl_now_us();
        s_rl_op_f0 = g_mx68k_frame_num;
        s_rl_seen_r = 0; s_rl_msr_wait = 0; s_rl_other_bytes = 0;
        s_rl_read_err[RE_FAULT]++;
        rl_log_read_end(RE_NAME[RE_FAULT], 0, 0);
        rl_synth_err(0, 1);
        rl_raise_int();
        return;
    }
    s_rl_last_io_frame[fu_status_dev_to_mx(s_rl_rd_us)] = g_mx68k_frame_num;   /* P1092: 表示用(s_rl_rd_us は装置側) */
    s_rl_cf_first = 1;   /* P1051: READ 系は装置の同期フラグを立てる(app.c@2191) */
    s_rl_op_gen = mx68k_floppyu_usb_generation();
    s_rl_op_t0_us = rl_now_us();
    s_rl_last_rx_us = s_rl_op_t0_us;
    s_rl_op_f0 = g_mx68k_frame_num;
    s_rl_seen_r = 0;
    s_rl_frame_len = 0;
    s_rl_msr_wait = 0;
    s_rl_other_bytes = 0;
    s_rl_state = RL_RD_WAIT;
}

/* P1037: changed の判定(書込み前読出しがあればその FNV と、無ければ E5埋め FNV と比較) */
static int rl_ar_changed(const rl_ar_sec_t* e)
{
    return e->pre_valid ? (e->last_fnv != e->pre_fnv) : (e->last_fnv != s_rl_ar_fill_fnv);
}

/* P1037: 武装と読み戻し照合(READ DATA 成功経路のみ、ログ機械照合。ゲストへ返すデータ・リザルトには影響しない)。
 * 装置リザルトが IC=00・ST1=0・ST2=0 でない READ は、武装判定・pre_fnv の記録・照合のいずれにも使わない */
static void rl_ar_on_read_ok(uint32_t n)
{
    if (s_rl_write_mode != WRM_ARMED || !s_rl_ar_env_ok || s_rl_rd_us != (uint8_t)s_rl_ar_unit) return;
    if ((s_rl_dev_result[0] & 0xC0u) != 0x00u || s_rl_dev_result[1] != 0x00u || s_rl_dev_result[2] != 0x00u) {
        s_rl_ar_read_err_skip++;
        return;
    }
    if (memcmp(s_rl_rd_chrn, FU_AR_MARK_CHRN, 4) == 0) {   /* 武装判定 */
        s_rl_ar_marker_reads++;
        int ref = -1;
        if (s_rl_wr_latched_off)                                         ref = AR_REF_LATCHED;
        else if (s_rl_ar_arm_used)                                       ref = AR_REF_ARM_USED;   /* 武装は1プロセス1回 */
        else if (s_rl_d_events != s_rl_rd_d_begin)                       ref = AR_REF_D_DURING;   /* このREAD発行以降 'D'/'d' 0回が条件 */
        else if (n != FU_AR_SECLEN)                                      ref = AR_REF_LEN;
        else if (memcmp(s_rl_frame, s_rl_ar_marker, FU_AR_SECLEN) != 0) ref = AR_REF_MISMATCH;
        if (ref < 0) {
            s_rl_ar_armed = 1;
            s_rl_ar_arm_used = 1;
            s_rl_ar_arm_gen = mx68k_floppyu_usb_generation();
            s_rl_ar_arm_f = g_mx68k_frame_num;
        } else {
            s_rl_ar_arm_ref[ref]++;
        }
        if (s_rl_ar_arm_lines < FU_AR_LOG_LIMIT) {
            s_rl_ar_arm_lines++;
            debug_log(P1037_ARM_TAG " %s f=%d reason=%s us_dev=%u C=%u H=%u R=%u N=%u n=%u read_fnv=0x%08x marker_fnv=0x%08x"
                      " d_events=%u gen=%u latched=%d arm_used=%d\n",
                      ref < 0 ? "arm" : "arm_refused", g_mx68k_frame_num, ref < 0 ? "-" : AR_REF_NAME[ref],
                      (unsigned)s_rl_rd_us, s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2], s_rl_rd_chrn[3],
                      (unsigned)n, (unsigned)s_rl_frame_fnv, (unsigned)s_rl_ar_marker_fnv, (unsigned)s_rl_d_events,
                      (unsigned)mx68k_floppyu_usb_generation(), s_rl_wr_latched_off, s_rl_ar_arm_used);
        }
        return;
    }
    if (s_rl_rd_chrn[0] >= FU_AR_NCYL || s_rl_rd_chrn[1] >= FU_AR_NHEAD || s_rl_rd_chrn[2] < 1u ||
        s_rl_rd_chrn[2] > FU_AR_NSEC || s_rl_rd_chrn[3] != 3u) return;
    rl_ar_sec_t* e = &s_rl_ar_map[rl_ar_idx(s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2])];
    if (!e->written) { if (!e->pre_valid) { e->pre_valid = 1; e->pre_fnv = s_rl_frame_fnv; } return; }
    if (e->uncertain) return;   /* 結果不明・不一致検出済みのセクタは照合しない(既にラッチ済み) */
    int match = (n == FU_AR_SECLEN) && (s_rl_frame_fnv == e->last_fnv);
    int was_pending = e->pending;
    e->pending = 0;
    if (match) { e->vmatch++; if (was_pending) { e->verified = 1; s_rl_ar_vmatch++; } }
    else {
        e->vmismatch++; e->verified = 0; s_rl_ar_vmismatch++;
        e->uncertain = 1;            /* 同一セクタの再照合を止める(停止後の再読出しで不一致→disarm→ログが繰り返されないように) */
        rl_ar_disarm("readback_mismatch");
    }
    if ((was_pending || !match) && s_rl_ar_v_lines < FU_AR_LOG_LIMIT) {
        char ph[16];
        s_rl_ar_v_lines++;
        if (e->pre_valid) snprintf(ph, sizeof(ph), "0x%08x", (unsigned)e->pre_fnv);
        else              snprintf(ph, sizeof(ph), "-");
        debug_log(P1037_VERIFY_TAG " verify f=%d C=%u H=%u R=%u N=%u written_fnv=0x%08x read_fnv=0x%08x read_n=%u"
                  " pre_fnv=%s match=%d changed=%d changed_basis=%s first=%d vmatch=%u vmismatch=%u\n",
                  g_mx68k_frame_num, s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2], s_rl_rd_chrn[3],
                  (unsigned)e->last_fnv, (unsigned)s_rl_frame_fnv, (unsigned)n, ph, match, rl_ar_changed(e),
                  e->pre_valid ? "pre" : "fill", was_pending, (unsigned)e->vmatch, (unsigned)e->vmismatch);
    }
}

/* P1053: normal の柵解除と読み戻し照合(READ DATA 成功経路のみ。ゲストへ返すデータ・リザルトには影響しない)。
 * 解除条件: 装置リザルト IC=00・ST1=0・ST2=0、かつ READ 発行以降 'D'/'d' 無し(装置共通の計数のまま、安全側)、かつ USB 世代不変。
 * どの CHRN でも開く(R1)。P1055: 記録するエポックは READ 発行時の ue[us](判定でその後 ue[us]++ されれば古くなり閉じたまま)。
 * open 行は閉→開の遷移時だけ、open_refused 行は閉じているユニットで不成立のときだけ出す(Fable5 C-5)。
 * 判別保留中は was_open が常に0になるため、hold= で区別する */
static void rl_nm_on_read_ok(uint32_t n)
{
    if (s_rl_write_mode != WRM_NORMAL) return;
    uint8_t us = s_rl_rd_us & 3u;
    const uint8_t* res = s_rl_dev_result;
    uint32_t gen_now = mx68k_floppyu_usb_generation();
    int clean = (res[0] & 0xC0u) == 0x00u && res[1] == 0x00u && res[2] == 0x00u;
    int d_ok = (s_rl_d_events == s_rl_rd_d_begin);   /* P1058: 判定には使わない。ログ用に生値を保持 */
    int g_ok = (gen_now == s_rl_rd_gen_begin);
    int was_open = rl_nm_fence_open(us);
    if (clean && g_ok) {
        s_nm_fo_valid[us] = 1;
        s_nm_fo_ue[us] = s_rl_rd_ue_begin[us];
        s_nm_fo_gen[us] = s_rl_rd_gen_begin;
    }
    if (!was_open) {
        int ok = clean && g_ok;
        int why = !clean ? NM_OR_ST : NM_OR_GEN;
        if (ok) s_nm_open_n++; else s_nm_open_ref[why]++;
        if (s_nm_fence_lines < FU_AR_LOG_LIMIT) {
            char rh[2 * 8 + 4], wy[32];
            s_nm_fence_lines++;
            rl_hex(rh, sizeof(rh), res, FU_RESULT_LEN, FU_RESULT_LEN);
            if (ok) snprintf(wy, sizeof(wy), "first_after_close=1");
            else    snprintf(wy, sizeof(wy), "why=%s", NM_OR_NAME[why]);
            debug_log(P1053_FENCE_TAG " %s f=%d unit=%u C=%u H=%u R=%u N=%u res=%s rd_d_begin=%u d_now=%u d_ok=%d rd_gen=%u gen=%u %s"
                      " open_n=%u hold=%d rd_ue=%u ue=%u\n",
                      ok ? "open" : "open_refused", g_mx68k_frame_num, (unsigned)us, s_rl_rd_chrn[0], s_rl_rd_chrn[1],
                      s_rl_rd_chrn[2], s_rl_rd_chrn[3], rh, (unsigned)s_rl_rd_d_begin, (unsigned)s_rl_d_events, d_ok,
                      (unsigned)s_rl_rd_gen_begin, (unsigned)gen_now, wy, (unsigned)s_nm_open_n, s_nm_st != NM_ST_I ? 1 : 0,
                      (unsigned)s_rl_rd_ue_begin[us], (unsigned)s_nm_ue[us]);
        }
    }
    /* 読み戻し照合。異常終了の READ のデータは部分的な場合があるため照合に使わない(誤ラッチ防止) */
    if (s_rl_rd_chrn[0] >= FU_AR_NCYL || s_rl_rd_chrn[1] >= FU_AR_NHEAD || s_rl_rd_chrn[2] < 1u ||
        s_rl_rd_chrn[2] > FU_AR_NSEC || s_rl_rd_chrn[3] != 3u) return;
    rl_nm_sec_t* e = &s_nm_map[us][rl_ar_idx(s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2])];
    if (!e->written || e->uncertain) return;
    if (!clean) { s_nm_v_skip_err++; return; }
    if (s_nm_st != NM_ST_I) { s_nm_v_skip_hold++; return; }   /* P1058: 判別保留中は ue 比較が未確定のため延期(written/pending は落とさない) */
    if (e->epoch_ue != s_nm_ue[us] || e->epoch_gen != gen_now) {   /* 交換後の別ディスクと比較しない */
        s_nm_v_skip_epoch++;
        e->written = 0; e->pending = 0;   /* 以後このエポック外の内容は照合しない(次の書込みで記録し直す) */
        return;
    }
    int match = (n == e->len) && (s_rl_frame_fnv == e->last_fnv);
    int was_pending = e->pending;
    e->pending = 0;
    if (match) { e->vmatch++; s_nm_v_match++; if (e->cached) s_nm_v_match_cached++; }
    else {
        e->vmismatch++; s_nm_v_mismatch++;
        if (e->cached) s_nm_v_mismatch_cached++;   /* P1089: 高速モードで受理した書込みの flush 失敗の事後検知 */
        e->uncertain = 1;   /* 再照合を止める(同じ不一致のログを繰り返さない) */
        rl_nm_latch("readback_mismatch");
    }
    if ((was_pending || !match) && s_nm_v_lines < FU_AR_LOG_LIMIT) {
        s_nm_v_lines++;
        debug_log(P1053_TAG " verify f=%d unit=%u C=%u H=%u R=%u N=%u written_fnv=0x%08x read_fnv=0x%08x read_n=%u written_n=%u"
                  " match=%d epoch_ok=1 first=%d writes=%u v_match=%u v_mismatch=%u cached=%d\n",
                  g_mx68k_frame_num, (unsigned)us, s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2], s_rl_rd_chrn[3],
                  (unsigned)e->last_fnv, (unsigned)s_rl_frame_fnv, (unsigned)n, (unsigned)e->len, match, was_pending,
                  (unsigned)e->writes, (unsigned)s_nm_v_match, (unsigned)s_nm_v_mismatch, e->cached);
    }
}

/* 'R'フレーム完了(進行中READ宛て) */
static void rl_read_frame_done(void)
{
    uint32_t len = s_rl_frame_len;
    if (len < FU_RESULT_LEN) {
        s_rl_read_err[RE_SHORT]++;
        rl_log_read_end(RE_NAME[RE_SHORT], 0, 0);
        rl_synth_err(0, 1);
        rl_raise_int();
        s_rl_synth_after_read = 1;
        return;
    }
    uint32_t n = len - FU_RESULT_LEN;
    memcpy(s_rl_dev_result, s_rl_frame + n, FU_RESULT_LEN);
    s_rl_read_ok++;
    s_rl_frame_fnv = rl_fnv(s_rl_frame, n);   /* P1035: read end 行へ出すため、ログより先に計算する */
    rl_log_read_end("ok", 1, s_rl_frame_fnv);
    rl_ar_on_read_ok(n);   /* P1037: armed の武装・照合(armed 以外では何もしない) */
    rl_nm_on_read_ok(n);   /* P1053: normal の柵解除・照合(normal 以外では何もしない) */
    /* P1035: mode1 の読み戻しベリファイ(ログ機械照合のみ。ゲストへ返すデータ・リザルトには影響しない) */
    if (s_rl_write_mode == WRM_WRITE && s_rl_wf_env_ok && s_rl_rd_us == (uint8_t)s_rl_wf_unit &&
        memcmp(s_rl_rd_chrn, s_rl_wf_chrn, 4) == 0) {
        s_rl_m1_fence_reads_ok++;
        if (s_rl_wr_sends == 0) { s_rl_wv_pre_valid = 1; s_rl_wv_pre_fnv = s_rl_frame_fnv; }   /* 書込み前の元データ */
    }
    if (s_rl_wv_pending && s_rl_rd_us == s_rl_wv_us && memcmp(s_rl_rd_chrn, s_rl_wv_chrn, 4) == 0) {
        int match = (n == s_rl_wr_n) && (s_rl_frame_fnv == s_rl_wv_fnv);
        int changed = s_rl_wv_pre_valid ? (s_rl_frame_fnv != s_rl_wv_pre_fnv) : -1;
        if (match) s_rl_wv_match++; else s_rl_wv_mismatch++;
        s_rl_wv_pending = 0;   /* 照合は書込み後の最初の1回のみ */
        if (s_rl_wv_lines < FU_LOG_LIMIT) {
            char ph[16];
            s_rl_wv_lines++;
            if (s_rl_wv_pre_valid) snprintf(ph, sizeof(ph), "0x%08x", (unsigned)s_rl_wv_pre_fnv);
            else                   snprintf(ph, sizeof(ph), "-");
            debug_log(P1035_VERIFY_TAG " verify f=%d us_dev=%u C=%u H=%u R=%u N=%u outcome=%s written_fnv=0x%08x"
                      " read_fnv=0x%08x read_n=%u written_n=%u pre_fnv=%s match=%d changed=%d\n",
                      g_mx68k_frame_num, (unsigned)s_rl_rd_us, s_rl_rd_chrn[0], s_rl_rd_chrn[1], s_rl_rd_chrn[2],
                      s_rl_rd_chrn[3], s_rl_wv_outcome, (unsigned)s_rl_wv_fnv, (unsigned)s_rl_frame_fnv,
                      (unsigned)n, (unsigned)s_rl_wr_n, ph, match, changed);
        }
    }
    int all_same = (n > 0);
    for (uint32_t i = 0; i < n; i++) if (s_rl_frame[i] != s_rl_frame[0]) { all_same = 0; break; }
    char dh[2 * 32 + 8];
    rl_hex(dh, sizeof(dh), s_rl_frame, n, 32);
    debug_log(P1009_TAG " read data f=%d n=%u frame_fnv=0x%08x all_same=%d head=%s\n",
              g_mx68k_frame_num, (unsigned)n, (unsigned)s_rl_frame_fnv, all_same, dh);
    if (!s_rl_first_read_dumped && n > 0) {
        s_rl_first_read_dumped = 1;
        for (uint32_t off = 0; off < n; off += 16) {
            char line[16 * 3 + 4];
            size_t o = 0;
            for (uint32_t i = off; i < n && i < off + 16; i++)
                o += (size_t)snprintf(line + o, sizeof(line) - o, "%s%02X", o ? " " : "", s_rl_frame[i]);
            debug_log(P1009_TAG " read dump %03x: %s\n", (unsigned)off, line);
        }
    }
    const uint8_t* r = s_rl_dev_result;
    debug_log(P1009_TAG " read result f=%d dev=%02X %02X %02X %02X %02X %02X %02X st0_mx=0x%02x\n",
              g_mx68k_frame_num, r[0], r[1], r[2], r[3], r[4], r[5], r[6], (unsigned)p955_st_dev_to_mx(r[0]));
    if (n == 0) {
        uint8_t res[FU_RESULT_LEN];
        memcpy(res, r, FU_RESULT_LEN);
        res[0] = p955_st_dev_to_mx(res[0]);
        rl_set_result(res, FU_RESULT_LEN);
        rl_raise_int();
        s_rl_synth_after_read = 1;
        return;
    }
    s_rl_supply_n = n;
    s_rl_supply_pos = 0;
    s_rl_mar0_valid = 0;
    s_rl_state = RL_RD_SUPPLY;
}

/* RD_SUPPLY: $E94003 読出し(DMA経由・CPUポーリングとも)で次の1Bを返す */
static uint8_t rl_supply_byte(void)
{
    if (s_rl_supply_pos == 0) {
        /* DMA が有効(CSR ACT)なら最初の1Bの転送先を照合の起点にする */
        s_rl_mar0_valid = (DMA[0].CSR & 0x08u) ? 1 : 0;
        s_rl_mar0 = DMA[0].MAR & 0xFFFFFFu;
        s_rl_ocr0 = DMA[0].OCR;
    }
    uint8_t b = s_rl_frame[s_rl_supply_pos++];
    if (s_rl_supply_pos >= s_rl_supply_n) {
        /* 最後の1Bを返す時点でリザルトへ(Core 標準 FDC_NextTrack→FDC_EPhaseEnd と同じく dma_readmem24 内) */
        uint8_t res[FU_RESULT_LEN];
        memcpy(res, s_rl_dev_result, FU_RESULT_LEN);
        res[0] = p955_st_dev_to_mx(res[0]);
        rl_set_result(res, FU_RESULT_LEN);
        rl_raise_int();
        s_rl_synth_after_read = 1;
        s_rl_verify_pending = 1;
        s_rl_verify_n = s_rl_supply_n;
    }
    return b;
}

/* P1031: WRITE DATA dryrun の収集開始(装置へは何も送らない) */
static void rl_collect_begin(void)
{
    const uint8_t* p = s_rl_cmd;   /* [0]cmd [1]US/HD [2]C [3]H [4]R [5]N [6]EOT [7]GPL [8]DTL */
    p1100_begin(0);   /* P1100: 早期 return が無いので先頭で記録してよい */
    s_rl_wrcollect_begin++;
    s_rl_wrcollect_pos = 0;
    s_rl_wrcollect_n = 128u << p[5];   /* p[5]<=3 を確認済み(WR_BUF_CAP 以内) */
    s_rl_wrcollect_f0 = g_mx68k_frame_num;
    s_rl_wrcollect_mar0_valid = 0;
    /* P1032: wponly の送信前検査で使う(収集数・世代・'D'受信の照合元) */
    s_rl_wr_mtc_begin = DMA[0].MTC;
    s_rl_wr_gen_begin = mx68k_floppyu_usb_generation();
    s_rl_wr_d_begin = s_rl_d_events;
    s_rl_wr_ue_begin[p[1] & 3u] = s_nm_ue[p[1] & 3u];   /* P1058: モード問わず常に採取、消費側は normal 限定 */
    if (s_rl_wrcollect_lines < FU_LOG_LIMIT) {
        s_rl_wrcollect_lines++;
        debug_log(P1031_TAG " begin f=%d C=%u H=%u R=%u N=%u EOT=%u GPL=0x%02x DTL=0x%02x cmd=0x%02x us_mx=0x%02x us_dev=0x%02x"
                  " expect=%u MTC=0x%04x MAR=0x%06x OCR=0x%02x CSR=0x%02x begin=%u\n",
                  g_mx68k_frame_num, p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[0], (unsigned)s_rl_us_mx,
                  (unsigned)(p[1] & 7u), (unsigned)s_rl_wrcollect_n,
                  (unsigned)DMA[0].MTC, (unsigned)(DMA[0].MAR & 0xFFFFFFu), (unsigned)DMA[0].OCR, (unsigned)DMA[0].CSR,
                  (unsigned)s_rl_wrcollect_begin);
    }
    s_rl_state = RL_WR_COLLECT;
}

/* P1063: FORMAT(WRITE ID)dryrun の収集開始。rl_collect_begin とは別関数で、装置への送信・照合元の採取は一切しない。
 * C-PHASE は [0]cmd [1]HD/US [2]N [3]SC [4]GPL [5]D(Inside X68000 図23)。E-PHASE のホスト→FDC は ID 表 SC×4B */
static void rl_fmt_collect_begin(void)
{
    const uint8_t* p = s_rl_cmd;
    uint32_t sc = p[3];
    if (sc == 0u) {   /* 0B 収集は即完了扱いになる縮退ケースなので受けない */
        if (s_rl_write_mode == WRM_NORMAL) s_nmf_ref[NMF_REF_SC_ZERO]++;   /* P1067 */
        rl_refuse(1, "fmt_sc_zero");
        return;
    }
    if (s_rl_write_mode == WRM_DRYRUN) s_rl_fmt_dryrun_n++;
    else if (s_rl_write_mode == WRM_FMT1) s_rl_fmt1_seen++;   /* P1064: fmt1 の分母 */
    s_rl_wrcollect_pos = 0;
    s_rl_wrcollect_n = sc * 4u;   /* SC<=255 なら最大 1020B(WR_BUF_CAP 以内) */
    s_rl_wrcollect_f0 = g_mx68k_frame_num;
    s_rl_wrcollect_mar0_valid = 0;
    /* P1064: 収集開始時点のスナップショット(rl_collect_begin と同じ3変数、モードを問わず採取)。
     * fmt1 の送信時ゲート(f1/f3/C0)と 'W' 受理ゲートの d_begin 比較がこの採取点を共有する。dryrun では消費者なし */
    s_rl_wr_mtc_begin = DMA[0].MTC;
    s_rl_wr_gen_begin = mx68k_floppyu_usb_generation();
    s_rl_wr_d_begin = s_rl_d_events;
    /* P1067(R1): rl_collect_begin と同一の式で、FORMAT 収集開始時点のユニット別エポックを採取する(モード問わず)。
     * normal の 'W' 受理ゲート(rl_nm_unit_unchanged)が直前の WRITE DATA 収集時の古い値と比較しないようにする */
    s_rl_wr_ue_begin[p[1] & 3u] = s_nm_ue[p[1] & 3u];
    s_rl_wr_is_format = 1;
    if (s_rl_write_mode == WRM_DRYRUN && s_rl_fmt_lines < FU_LOG_LIMIT) {
        s_rl_fmt_lines++;
        debug_log(P1063_TAG " begin f=%d cmd=0x%02x hd_us=0x%02x us_mx=0x%02x N=%u SC=%u GPL=0x%02x D=0x%02x expect=%u"
                  " MTC=0x%04x MAR=0x%06x OCR=0x%02x CSR=0x%02x n=%u\n",
                  g_mx68k_frame_num, p[0], p[1], (unsigned)s_rl_us_mx, p[2], p[3], p[4], p[5], (unsigned)s_rl_wrcollect_n,
                  (unsigned)DMA[0].MTC, (unsigned)(DMA[0].MAR & 0xFFFFFFu), (unsigned)DMA[0].OCR, (unsigned)DMA[0].CSR,
                  (unsigned)s_rl_fmt_dryrun_n);
    }
    p1100_begin(1);   /* P1100: sc==0 の早期 return(rl_refuse)より後に置く(縮退ケースで active を立てない) */
    s_rl_state = RL_WR_COLLECT;
}

/* P1063: FORMAT dryrun の収集完了。ID 表を生値と解読値の両方で記録し、装置へは送らずに正常終了リザルトを合成する。
 * ST0=IC00+HD/US(コマンドのものをそのまま)、ST1=ST2=0、C/H/R=0(意味を持たない)、N=コマンドの N */
static void rl_fmt_dryrun_complete(void)
{
    uint32_t n = s_rl_wrcollect_pos;
    if (s_rl_fmt_lines < FU_LOG_LIMIT) {
        char dh[2 * WR_BUF_CAP + 8];
        char ids[WR_BUF_CAP / 4u * 16u + 8];
        size_t o = 0;
        s_rl_fmt_lines++;
        rl_hex(dh, sizeof(dh), s_rl_wrcollect_buf, n, WR_BUF_CAP);
        ids[0] = '\0';
        for (uint32_t i = 0; i + 4u <= n && o + 16u < sizeof(ids); i += 4u)
            o += (size_t)snprintf(ids + o, sizeof(ids) - o, "%s%u/%u/%u/%u", i ? "," : "",
                                  s_rl_wrcollect_buf[i], s_rl_wrcollect_buf[i + 1], s_rl_wrcollect_buf[i + 2],
                                  s_rl_wrcollect_buf[i + 3]);
        debug_log(P1063_TAG " done f=%d got=%u expect=%u frames=%d MAR0_valid=%d MAR0=0x%06x MTC0=0x%04x OCR0=0x%02x"
                  " CSR0=0x%02x MAR_now=0x%06x MTC_now=0x%04x data_fnv=0x%08x data_hex=%s ids_chrn=%s sent=0\n",
                  g_mx68k_frame_num, (unsigned)n, (unsigned)s_rl_wrcollect_n, g_mx68k_frame_num - s_rl_wrcollect_f0,
                  s_rl_wrcollect_mar0_valid, (unsigned)s_rl_wrcollect_mar0, (unsigned)s_rl_wrcollect_mtc0,
                  (unsigned)s_rl_wrcollect_ocr0, (unsigned)s_rl_wrcollect_csr0,
                  (unsigned)(DMA[0].MAR & 0xFFFFFFu), (unsigned)DMA[0].MTC,
                  (unsigned)rl_fnv(s_rl_wrcollect_buf, n), dh, ids);
    }
    uint8_t res[FU_RESULT_LEN] = { 0 };
    res[0] = (uint8_t)(s_rl_cmd[1] & 0x07u);
    res[6] = s_rl_cmd[2];
    res[0] = p955_st_dev_to_mx(res[0]);   /* s_rl_cmd[1] は rl_start で装置座標へ変換済み */
    if (s_rl_fmt_lines < FU_LOG_LIMIT) {
        s_rl_fmt_lines++;
        debug_log(P1063_TAG " result f=%d res=%02X %02X %02X %02X %02X %02X %02X n=%u\n", g_mx68k_frame_num,
                  res[0], res[1], res[2], res[3], res[4], res[5], res[6], (unsigned)s_rl_fmt_dryrun_n);
    }
    rl_set_result(res, FU_RESULT_LEN);
    rl_raise_int();
}

/* P1032: 非キャッシュ SDS の結果記録(precheck 行に全て出す) */
typedef struct {
    int      ret_tx, ret_st3, ret_post;
    uint8_t  msr_pre[FU_WR_SDS_RQM_POLL_MAX];
    uint32_t polls;
    uint8_t  st3, msr_post;
    const char* fail;
} rl_wr_sds_log_t;

/* P1032: 送信直前の非キャッシュ SDS(Fable5 M2)。キャッシュ・s_rl_state・リザルトには触れない。
 *   1) 83 04 83 <US/HD> を送信(結果は読まない)
 *   2) 装置MSRを最大 FU_WR_SDS_RQM_POLL_MAX 回問い合わせ、(msr&0xC0)==0xC0(結果フェーズ)になるのを待つ
 *   3) 03 で1B読む(=ST3)
 *   4) 装置MSRをもう一度読み、(msr&0xF0)==0x80(RQM=1・DIO=0・EXM=0・CB=0)へ戻ったことを確認する
 * 戻り値: 1=手順1〜4が全て成立(*st3 は結果フェーズで読んだ鮮度のある1B)、0=いずれかが不成立
 * P1089(N2): US/HD を引数で受け取る版。s_rl_cmd[] には一切触れない(能動 flush 確認・bringup 前の flush 確認から呼ぶ)。
 * SDS(0x04)は装置キャッシュの flush 契機なので wc_src で dirty の flush_req を立てる(dirty でなければ何もしない) */
static int rl_wr_sds_fresh_us(uint8_t us_hd, uint8_t* st3, rl_wr_sds_log_t* lg, int wc_src)
{
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    uint8_t tx[4] = { FU_WR | FU_REG_DATA, 0x04, FU_WR | FU_REG_DATA, us_hd };
    rl_wc_note_trigger(wc_src);
    memset(lg, 0, sizeof(*lg));
    lg->ret_st3 = -9;
    lg->ret_post = -9;
    lg->fail = "none";
    lg->ret_tx = rl_p1("wr_sds_cmd", tx, sizeof(tx), NULL, 0);
    if (lg->ret_tx < 0) { lg->fail = "tx"; return 0; }
    int in_result = 0;
    while (lg->polls < FU_WR_SDS_RQM_POLL_MAX) {
        uint8_t msr = 0;
        if (rl_p1("wr_sds_msr", TX_MSR, 1, &msr, 1) < 1) { lg->fail = "msr_rx"; return 0; }
        lg->msr_pre[lg->polls++] = msr;
        if ((msr & 0xC0u) == 0xC0u) { in_result = 1; break; }
    }
    if (!in_result) { lg->fail = "no_result_phase"; return 0; }
    lg->ret_st3 = rl_p1("wr_sds_st3", TX_DATA, 1, &lg->st3, 1);
    if (lg->ret_st3 < 1) { lg->fail = "st3_rx"; return 0; }
    lg->ret_post = rl_p1("wr_sds_msr_post", TX_MSR, 1, &lg->msr_post, 1);
    if (lg->ret_post < 1) { lg->fail = "msr_post_rx"; return 0; }
    if ((lg->msr_post & 0xF0u) != 0x80u) { lg->fail = "msr_not_idle"; return 0; }
    *st3 = lg->st3;
    return 1;
}

/* 書込みコマンド送信直前の非キャッシュ SDS(US/HD は収集中のコマンド s_rl_cmd[1])。P1089: rl_wr_sds_fresh_us の薄いラッパー */
static int rl_wr_sds_fresh(uint8_t* st3, rl_wr_sds_log_t* lg)
{
    return rl_wr_sds_fresh_us(s_rl_cmd[1], st3, lg, WCF_SDS);
}

/* P1032: 送信後に装置の状態が不明になった(port1/port2 の送信途中失敗)。ラッチして合成エラー */
static void rl_wr_fail_unknown(const char* outcome)
{
    s_rl_fmt1_inflight = 0;   /* P1064: spent はそのまま */
    s_rl_wr_latched_off = 1;
    s_rl_bringup_valid = 0;
    rl_rebringup_arm("send_fail");   /* P1087(D) */
    rl_wr_log_end("send_fail", outcome);
    rl_ar_after_unknown(outcome, NULL);   /* P1037: armed のみ */
    if (s_nmf_inflight) { s_nmf_inflight = 0; rl_nmf_halt(outcome); }   /* P1067 */
    rl_synth_err(ST1_OR, 1);
    rl_raise_int();
}

/* P1032: 書込みコマンド(MTC設定+コマンド9B)を1回の port1 送信で送る。tx = B0 <n&FF> B1 <n>>8> 83 c0 … 83 c8 */
static int rl_send_write_cmd(uint32_t n)
{
    uint8_t tx[22];
    char th[2 * 22 + 4];
    size_t k = 0;
    tx[k++] = (uint8_t)(FU_WR | FU_REG_MTC_LO);
    tx[k++] = (uint8_t)(n & 0xFFu);
    tx[k++] = (uint8_t)(FU_WR | FU_REG_MTC_HI);
    tx[k++] = (uint8_t)(n >> 8);
    for (uint32_t i = 0; i < s_rl_cmd_n && k + 2 <= sizeof(tx); i++) {
        tx[k++] = (uint8_t)(FU_WR | FU_REG_DATA);
        tx[k++] = s_rl_cmd[i];
    }
    /* P1089: FORMAT(0x0D)は書込みキャッシュ対象外のコマンドなので flush 契機(FW: app.c:1284-1291, 2140-2149)。
     * WRITE DATA/WRITE DELETED の満杯時 flush は 'W' 待ちに現れる(M2)ので契機として扱わない */
    uint8_t c1 = s_rl_cmd[0] & 0x1Fu;
    if (c1 != 5u && c1 != 9u) rl_wc_note_trigger(WCF_CMD);
    int64_t t0 = rl_now_us();
    int r = rl_p1("write_cmd", tx, k, NULL, 0);
    int64_t us = rl_now_us() - t0;
    rl_hex(th, sizeof(th), tx, k, 22);
    debug_log(P1009_TAG " cmd f=%d gen=%u name=write_cmd cmd=0x%02x us_mx=0x%02x us_dev=0x%02x tx=%s ret=%d us=%lld\n",
              g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_rl_cmd[0],
              (unsigned)s_rl_us_mx, (unsigned)(s_rl_cmd[1] & 7u), th, r, (long long)us);
    if (s_rl_wr_lines < FU_LOG_LIMIT) {
        char mx[16];
        s_rl_wr_lines++;
        debug_log(P1032_TAG " send f=%d gen=%u n=%u tx=%s ret=%d us=%lld sends_after=%u/%s\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)n, th, r, (long long)us,
                  (unsigned)(r == -1 ? s_rl_wr_sends : s_rl_wr_sends + 1u), rl_wr_max_str(mx, sizeof(mx)));
    }
    return r;
}

/* P1032: wponly の送信前検査と送信。検査が1つでも不成立なら装置へ書込みコマンドを送らず NW を合成する。
 * 実行コンテキスト: Core DMA ch0 が最終バイトを $E94003 へ書き込むコールバック内(rl_collect_done 経由)で
 * 同期実行される。SDS 往復と書込みコマンド送信の間エミュレーションスレッドが止まる(rl_read_begin と同種、
 * 所要時間は precheck/send 行の us と s_rl_blk_frame_us に出る) */
static void rl_poll(void);
static int rl_nmf_verify(int trig, int* post_relay);   /* P1067: 定義は fmt1 の検証関数群の後 */

/* P1051: 同期手順の記録(1書込み1行の [P1051-SYNC] に全て出す) */
typedef struct {
    int      attempted;
    int      msr_ret;
    uint8_t  msr_after_seek;
    int64_t  c_wait_us, drain_us, us;
    const char* via;
    uint32_t sis_iter, st0_n, rdychg_n, rdychg_target, c_seen, cc_seen, d_delta;
    uint8_t  st0_list[FU_SYNC_ST0_LIST_MAX];
    int      have_pcn;
    uint8_t  pcn;
    uint32_t gen0, gen1;
    const char* fail;
} rl_sync_log_t;

/* P1051: 同期手順中の port2 読出し待ち。眠った時間はフレームのブロック時間へ計上する(M3) */
static void rl_sync_sleep(void)
{
    int64_t t0 = rl_now_us();
    struct timespec ts = { 0, (long)FU_SYNC_POLL_SLEEP_US * 1000L };
    nanosleep(&ts, NULL);
    s_rl_blk_frame_us += rl_now_us() - t0;
}

/* P1051: 書込みコマンド送信の直前に、同一シリンダへの SEEK+SENSE INT を装置へ送り、装置ファームの同期フラグ
 * (app.c m_isFirstWrite)を立てる。戻り値: 1=成立(書込みを送ってよい)、0=不成立(lg->fail に理由)。
 *   (a) 83 0F 83 <US/HD> 83 <C> を送り、続けて装置MSRを1回だけ読む(msr_after_seek、DnB の挙動の観測用)
 *   (b) port2 の'C'を FU_SYNC_C_WAIT_MS まで待つ(来なければ上限で SIS へ。DnB 解除は待たない)
 *   (c) SIS(83 08 03→ST0、01→MSR、RQM|DIO なら 03→PCN)を最大 FU_SYNC_SIS_MAX 回。IC=11(Ready 変化)は読み捨てて続行、
 *       IC=00・SE・US 一致で終了して PCN==C を検査。IC=11 の後の 0x80 は、待ち時間が残っていれば (b) へ戻る(b′)
 *   (d) port2 を FU_SYNC_DRAIN_MS まで読み、'C'を見ていれば'c'を要求する
 *   (e) 'D'無し・USB世代不変・ラッチ無し・状態不変・armed なら武装中を再検査する
 * 実行中は s_rl_sync_active を立て、'C'はゲストへ渡さない(rl_on_c で吸収)。port2 は手順内から rl_poll() で読む。
 * P1053: 第2引数をモード種別へ一般化。normal は (e) の最後に柵を再検査する。手順中に柵が閉じる経路は 'D'/'d' と世代変化だけで、
 * どちらも先の d_event/gen 検査で止まるため、この分岐は到達しない設計(多重防御。到達したら sy_fence_closed に数える) */
static int rl_wr_sync_seek(rl_sync_log_t* lg, int mode)
{
    int is_ar = (mode == WRM_ARMED);
    int is_nm = (mode == WRM_NORMAL);
    static const uint8_t TX_SI[]   = { FU_WR | FU_REG_DATA, 0x08, FU_REG_DATA };
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    const uint8_t* p = s_rl_cmd;
    uint8_t us = p[1] & 3u;
    int64_t t0 = rl_now_us();
    memset(lg, 0, sizeof(*lg));
    lg->attempted = 1;
    lg->msr_ret = -9;
    lg->via = "-";
    lg->fail = "none";
    lg->gen0 = mx68k_floppyu_usb_generation();
    uint32_t d0 = s_rl_d_events;
    int state0 = s_rl_state;
    if (s_rl_in_poll) {   /* port2 を読めない(再入中)ので手順を始めない */
        lg->fail = "reentry";
        lg->gen1 = lg->gen0;
        lg->us = rl_now_us() - t0;
        return 0;
    }
    s_rl_sync_active = 1;   /* SEEK の'C'は約0.2msで届くので、送信より前に立てる */
    s_rl_sync_c = 0;
    s_rl_sync_cc = 0;
    int ok = 0;
    rl_nmf_pcn_invalidate(us, &s_nmf_pcn_inval_sync);   /* P1067: MX 自身の SEEK でヘッドが動く。ゲストの SEEK 由来の PCN 記録は古くなる */
    {
        uint8_t tx[7] = { FU_WR | FU_REG_DATA, 0x0F, FU_WR | FU_REG_DATA, p[1], FU_WR | FU_REG_DATA, p[2], FU_REG_MSR };
        rl_wc_note_trigger(WCF_CMD);   /* P1089: SEEK は flush 契機(高速では呼ばれないが dirty が残る経路に備える) */
        lg->msr_ret = rl_p1("sync_seek", tx, sizeof(tx), &lg->msr_after_seek, 1);
        if (lg->msr_ret < 1) { lg->fail = "seek_tx"; goto out; }
    }
    int64_t deadline = rl_now_us() + (int64_t)FU_SYNC_C_WAIT_MS * 1000;
    uint32_t c_mark = 0;
    int prev_rdy = 0;
    for (;;) {
        int64_t tw = rl_now_us();
        while (s_rl_sync_c <= c_mark && rl_now_us() < deadline) {
            rl_poll();
            if (s_rl_sync_c > c_mark) break;
            rl_sync_sleep();
        }
        lg->c_wait_us += rl_now_us() - tw;
        lg->via = (s_rl_sync_c > c_mark) ? "c" : "timeout";
        c_mark = s_rl_sync_c;
        int back_to_wait = 0;
        for (;;) {
            if (lg->sis_iter >= (uint32_t)FU_SYNC_SIS_MAX) { lg->fail = "sis_limit"; goto out; }
            uint8_t st0 = 0x80, msr = 0, pcn = 0;
            int have_pcn = 0;
            int r = rl_p1("sync_sense_st0", TX_SI, sizeof(TX_SI), &st0, 1);
            if (r >= 1) r = rl_p1("sync_sense_msr", TX_MSR, 1, &msr, 1);
            if (r >= 1 && (msr & 0xC0u) == 0xC0u) {
                r = rl_p1("sync_sense_pcn", TX_DATA, 1, &pcn, 1);
                have_pcn = (r >= 1);
            }
            lg->sis_iter++;
            if (r < 1) { lg->fail = "st0"; goto out; }   /* 通信失敗(-2/-3 は rl_p1 内で故障停止済み) */
            if (lg->st0_n < FU_SYNC_ST0_LIST_MAX) lg->st0_list[lg->st0_n++] = st0;
            if ((st0 & 0xC0u) == 0xC0u) {   /* Ready 変化(任意ユニット): 読み捨てて次の SIS へ(M2) */
                lg->rdychg_n++;
                if ((st0 & 3u) == us) lg->rdychg_target++;
                prev_rdy = 1;
                continue;
            }
            if ((st0 & 0xC0u) == 0x80u) {   /* 保留割込み無し: シーク未完了。Ready 変化の後で時間が残っていれば待ちへ戻る(b′) */
                if (prev_rdy && rl_now_us() < deadline) { prev_rdy = 0; back_to_wait = 1; break; }
                lg->fail = "invalid80";
                goto out;
            }
            if ((st0 & 0xC0u) == 0x00u && (st0 & 0x20u) && (st0 & 3u) == us) {   /* SEEK 正常終了(HD は比較しない) */
                lg->have_pcn = have_pcn;
                lg->pcn = pcn;
                if (!have_pcn || pcn != p[2]) { lg->fail = "pcn"; goto out; }
                break;
            }
            lg->fail = "st0";
            goto out;
        }
        if (!back_to_wait) break;
    }
    {
        int64_t td = rl_now_us(), dend = td + (int64_t)FU_SYNC_DRAIN_MS * 1000;
        for (;;) {
            rl_poll();
            if (s_rl_sync_c > 0u && s_rl_sync_cc > 0u) break;
            if (rl_now_us() >= dend) break;
            rl_sync_sleep();
        }
        lg->drain_us = rl_now_us() - td;
    }
    if (s_rl_sync_c > 0u && s_rl_sync_cc == 0u) { lg->fail = "c_without_cc"; goto out; }
    if (!is_nm && (s_rl_d_events != d0 || s_rl_d_events != s_rl_wr_d_begin)) lg->fail = "d_event";
    else if (mx68k_floppyu_usb_generation() != s_rl_wr_gen_begin)       lg->fail = "gen";
    else if (s_rl_wr_latched_off || (is_ar && !s_rl_ar_armed))          lg->fail = "latched";
    else if (s_rl_state != state0 || s_rl_state != RL_WR_COLLECT)       lg->fail = "state";
    else if (is_nm && !rl_nm_fence_open(us))                            { lg->fail = "fence_closed"; s_nm_sy_fence_closed++; }
    else                                                                ok = 1;
out:
    s_rl_sync_active = 0;
    lg->c_seen = s_rl_sync_c;
    lg->cc_seen = s_rl_sync_cc;
    lg->d_delta = s_rl_d_events - d0;
    lg->gen1 = mx68k_floppyu_usb_generation();
    lg->us = rl_now_us() - t0;
    return ok;
}

/* P1051: 1書込み1行(mode1/armed、送信の有無によらず出す)。decision=sent|send_fail|sync_refused|no_sync */
static void rl_sync_log(const rl_sync_log_t* lg, const char* precheck, const char* decision, int would,
                        int cf_first0, uint8_t cf_cmd0, int send_ret)
{
    if (s_rl_sy_lines >= FU_AR_LOG_LIMIT) return;
    s_rl_sy_lines++;
    const uint8_t* p = s_rl_cmd;
    char sl[FU_SYNC_ST0_LIST_MAX * 3 + 4], ph[8];
    size_t o = 0;
    sl[0] = '\0';
    for (uint32_t i = 0; i < lg->st0_n; i++) o += (size_t)snprintf(sl + o, sizeof(sl) - o, "%s%02X", i ? "," : "", lg->st0_list[i]);
    if (o == 0) snprintf(sl, sizeof(sl), "-");
    if (lg->have_pcn) snprintf(ph, sizeof(ph), "%u", lg->pcn);
    else              snprintf(ph, sizeof(ph), "-");
    debug_log(P1051_TAG " w f=%d mode=%d C=%u H=%u R=%u N=%u cmd=0x%02x us_dev=%u precheck=%s decision=%s sync_fail=%s"
              " attempted=%d msr_after_seek=0x%02x msr_ret=%d c_wait_us=%lld c_seen=%u via=%s sis_iter=%u sis_st0_list=%s"
              " rdychg_n=%u rdychg_target=%u pcn=%s cc_seen=%u drain_us=%lld d_delta=%u gen=%u/%u sync_us=%lld"
              " would_cache=%d cf_first=%d cf_cmd=0x%02x send_ret=%d"
              " sends=%u sync_ok=%u sync_refused=%u would_cache_n=%u form_a=%u form_c=%u form4=%u form_other=%u\n",
              g_mx68k_frame_num, s_rl_write_mode, p[2], p[3], p[4], p[5], (unsigned)p[0], (unsigned)(p[1] & 7u),
              precheck, decision, lg->attempted ? lg->fail : "-",
              lg->attempted, (unsigned)lg->msr_after_seek, lg->msr_ret, (long long)lg->c_wait_us, (unsigned)lg->c_seen,
              lg->via, (unsigned)lg->sis_iter, sl, (unsigned)lg->rdychg_n, (unsigned)lg->rdychg_target, ph,
              (unsigned)lg->cc_seen, (long long)lg->drain_us, (unsigned)lg->d_delta, (unsigned)lg->gen0,
              (unsigned)lg->gen1, (long long)lg->us, would, cf_first0, (unsigned)cf_cmd0, send_ret,
              (unsigned)s_rl_wr_sends, (unsigned)s_rl_sy_ok, (unsigned)s_rl_sy_refused, (unsigned)s_rl_sy_would_cache_n,
              (unsigned)s_rl_sy_form_a, (unsigned)s_rl_sy_form_c, (unsigned)s_rl_sy_form4, (unsigned)s_rl_sy_form_other);
}

/* P1064: 書込みコマンド送信成功後の WAIT_W 突入時の状態初期化(rl_wr_try_send から抽出、rl_fmt1_try_send と共用) */
static void rl_wr_enter_wait_w(uint32_t n)
{
    /* P1099(診断専用、挙動変更なし): 前回サイクルが完了していた場合(s_rl_wr_result_f!=0)、
     * 段階別フレーム差を記録してから通常の初期化へ進む。初回呼出し(前回サイクルが無い)はスキップ。
     * 基準点はs_rl_op_f0ではなく書込み/FORMAT専用のs_rl_wr_op_f0を使う(共有変数の汚染を回避) */
    if (s_rl_wr_result_f != 0) {
        int d_cmd2sent = s_rl_wr_data_sent_f - s_rl_wr_op_f0;
        int d_sent2result = s_rl_wr_result_f - s_rl_wr_data_sent_f;
        int d_result2next = g_mx68k_frame_num - s_rl_wr_result_f;
        debug_log(P1099_TAG " stage prev_f0=%d data_sent_f=%d result_f=%d next_f=%d "
                  "d_cmd2sent=%d d_sent2result=%d d_result2next=%d\n",
                  s_rl_wr_op_f0, s_rl_wr_data_sent_f, s_rl_wr_result_f, g_mx68k_frame_num,
                  d_cmd2sent, d_sent2result, d_result2next);
    }
    int64_t now = rl_now_us();
    s_rl_op_gen = mx68k_floppyu_usb_generation();
    s_rl_op_t0_us = now;
    s_rl_op_f0 = g_mx68k_frame_num;
    s_rl_wr_op_f0 = g_mx68k_frame_num;   /* P1099: 書込み/FORMAT専用の複製 */
    s_rl_wr_send_t_us = now;
    s_rl_wr_state_t_us = now;
    s_rl_wr_data_sent = 0;
    s_rl_wr_data_sent_f = 0;   /* P1099: 新サイクル用にリセット */
    s_rl_wr_n = n;
    s_rl_wr_ev_n = 0;
    s_rl_wr_c_seen = 0;
    s_rl_wr_result_t_us = 0;
    s_rl_wr_result_f = 0;      /* P1099 */
    s_rl_wr_res_msr_n = 0;
    s_rl_wr_res_polls = 0;
    s_rl_wr_res_last_poll_us = 0;
    s_rl_state = RL_WR_WAIT_W;   /* 割込みは発行しない(ゲストは MSR=0x10 を見て割込み待ち) */
}

static void rl_wr_try_send(void)
{
    const uint8_t* p = s_rl_cmd;
    uint32_t n = s_rl_wrcollect_pos;
    uint32_t expect = 128u << p[5];
    int64_t t0 = rl_now_us();
    s_rl_wr_checks++;
    /* P1037(Fable5 M1): armed は判定の前に port2 を読み切り、未読の 'D'/'d'/'W' 等を先に処理する。
     * s_rl_d_events・USB世代・ラッチは最後に rl_poll() が走った時点の値でしかなく、rl_poll() は通常 rl_tick()(フレーム毎)
     * からしか呼ばれないため、到着済み・未読の 'D' が層(1)「武装後の 'D' で永久停止」をすり抜ける窓を閉じる。
     * ここへは rl_collect_done 経由で s_rl_state==RL_WR_COLLECT のまま入る。この状態では rl_op_needs_device()==0 で
     * rl_abort_op(RE_DISCONNECT) は走らず、rl_check_timeouts も書込み待ちを持たない。'C' は rl_on_c で発行保留になるだけ。
     * 想定と違って状態が変わった/読み切れない(再入中)なら送らない(安全側) */
    int is_ar = (s_rl_write_mode == WRM_ARMED);
    int is_nm = (s_rl_write_mode == WRM_NORMAL);   /* P1053: normal も同じ読み切りを行う(未読 'D'/'d' で柵を閉じてから判定) */
    int ar_drained = 1;
    if (is_ar || is_nm) {
        int st_before = s_rl_state;
        if (s_rl_in_poll) ar_drained = 0;
        else {
            rl_poll();                               /* 'D' を読めば rl_ar_disarm("ev_D") 済みになる */
            if (s_rl_state != st_before) ar_drained = 0;
        }
        if (s_rl_ar_armed && mx68k_floppyu_usb_generation() != s_rl_ar_arm_gen) rl_ar_disarm("gen_changed");
    }
    /* 検査10(P1067、normal のみ): normal FORMAT の検証予約が残っていれば、この書込みを装置へ送る前に先に検証する
     * (最終トラック C76H1 の検証が後続の SEEK に依存するため)。ユニットを問わない。同期手順(rl_wr_sync_*)より前に
     * 評価し、以下の検査(c3・c8 等)は検証の後の状態で評価する。検証が ok でなければ停止済み(rl_nmf_halt)で、この書込みは拒否 */
    int c10 = 1;
    if (is_nm && s_nmf_verify_pending) c10 = rl_nmf_verify(NMF_VSRC_WRITE, NULL);
    /* P1070 X2: normal WRITE DATA で完走記録を失効(c10 の検証で完走した場合もここで消える) */
    if (is_nm && s_nmf_ld_valid) { s_nmf_ld_valid = 0; s_nmf_ld_exp_write++; }
    /* 検査1: 収集数 == 期待長 == 収集開始時 DMAC MTC */
    int c1 = (n == s_rl_wrcollect_n) && (n == expect) && (n == (uint32_t)s_rl_wr_mtc_begin);
    /* 検査2: DMA 経由かつ Memory→Device */
    int c2 = (s_rl_wrcollect_mar0_valid == 1) && ((s_rl_wrcollect_ocr0 & 0x80u) == 0u);
    /* 検査3: 中継可能・世代不変・'D' 無し */
    int relay = rl_relay_ok();
    uint32_t gen_now = mx68k_floppyu_usb_generation();
    uint32_t d_now = s_rl_d_events;
    int c3 = relay && (gen_now == s_rl_wr_gen_begin) &&
             (is_nm ? rl_nm_unit_unchanged(s_rl_cmd[1] & 3u) : (d_now == s_rl_wr_d_begin));   /* P1058: normal はユニット別エポックで判定 */
    /* 検査4: 送信回数上限・ラッチ(P1035: 上限はモード別。wponly 8 / mode1 1) */
    uint32_t send_max = rl_wr_send_max();
    int latched = s_rl_wr_latched_off;
    int c4 = is_nm ? !latched : ((s_rl_wr_sends < send_max) && !latched);   /* P1053: normal はプロセス通算上限を持たない */
    /* 検査6(P1035、mode1のみ): 柵。装置側ユニットと C,H,R,N が環境変数の許可値と完全一致 */
    int is_w1 = (s_rl_write_mode == WRM_WRITE);
    uint8_t us_dev = s_rl_cmd[1] & 3u;
    int f_unit = is_w1 && s_rl_wf_env_ok && (int)us_dev == s_rl_wf_unit;
    int f_chr  = is_w1 && s_rl_wf_env_ok && p[2] == s_rl_wf_chrn[0] && p[3] == s_rl_wf_chrn[1] &&
                 p[4] == s_rl_wf_chrn[2] && p[5] == s_rl_wf_chrn[3];
    int c6 = !is_w1 || (f_unit && f_chr);
    /* 検査7(P1037、armedのみ): 柵。許可ユニット・許可シリンダ範囲(H∈{0,1}かつHDビット一致・R∈1..8・N=3)・
     * マーカーセクタ以外・武装中・読み切り成立。c6 は armed では常に1(!is_w1)なので armed の送信式には入れない */
    int a_unit  = is_ar && s_rl_ar_env_ok && (int)us_dev == s_rl_ar_unit;
    int a_range = is_ar && s_rl_ar_env_ok && p[2] >= s_rl_ar_cmin && p[2] <= s_rl_ar_cmax && p[2] < FU_AR_NCYL &&
                  p[3] < FU_AR_NHEAD && p[4] >= 1u && p[4] <= FU_AR_NSEC && p[5] == 3u &&
                  ((s_rl_cmd[1] >> 2) & 1u) == p[3];                       /* US/HD の HD とパラメータ H の一致も要求 */
    int a_mark  = is_ar && p[2] == FU_AR_MARK_CHRN[0] && p[3] == FU_AR_MARK_CHRN[1] &&
                  p[4] == FU_AR_MARK_CHRN[2] && p[5] == FU_AR_MARK_CHRN[3];
    int c7 = !is_ar || (a_unit && a_range && !a_mark && s_rl_ar_armed && ar_drained);
    /* 検査8・9(P1053、normalのみ): 許可ユニット(未設定=全許可・不正値=全拒否)・交換後READ柵・同一キー連続書込み検知・読み切り成立。
     * c9 の候補値 loop_next は「この書込みを送れば同一キーの何回目か」(時間 FU_WR_LOOP_RESET_MS 超過で1へ戻る) */
    int nm_unit = is_nm && (s_nm_unit_state == 0 || (s_nm_unit_state == 1 && (int)us_dev == s_nm_unit));
    int c8 = is_nm && rl_nm_fence_open(us_dev);
    uint8_t nm_key[5] = { us_dev, p[2], p[3], p[4], p[5] };
    int64_t nm_now = rl_now_us();
    int nm_same = is_nm && s_nm_loop_valid && memcmp(nm_key, s_nm_loop_key, 5) == 0;
    int nm_time_reset = nm_same && (nm_now - s_nm_loop_t_us) > (int64_t)FU_WR_LOOP_RESET_MS * 1000;
    uint32_t loop_next = (nm_same && !nm_time_reset) ? s_nm_loop_cnt + 1u : 1u;
    int c9 = loop_next <= FU_WR_LOOP_MAX;
    int cn = !is_nm || (nm_unit && c8 && c9 && ar_drained && c10);
    /* 検査5: 非キャッシュ SDS。検査1〜4(mode1 は検査6も、armed は検査7も、normal は検査8・9も)が全て成立したときだけ
     * 装置へ問い合わせる。柵が不成立なら装置へ一切問い合わせない */
    rl_wr_sds_log_t lg;
    memset(&lg, 0, sizeof(lg));
    lg.ret_tx = -9; lg.ret_st3 = -9; lg.ret_post = -9; lg.fail = "skipped";
    uint8_t st3 = 0;
    int sds_ok = 0;
    /* P1089(B-2): 高速モードでは、反事実モデルで次の書込みがキャッシュ経路になる見込み(s_rl_cf_first==0)のときだけ、
     * 非キャッシュ SDS(装置キャッシュの flush 契機)を送らず MX 内の SDS キャッシュ値を有効期間を問わず使う。
     * cf_first==1(SEEK/RECAL/READ の直後=装置キャッシュは空)とキャッシュ無しのときは従来どおり非キャッシュ SDS を送り、
     * その鮮度のある結果をキャッシュへ記録する(以後のキャッシュ経路の書込みで使う)。判定式・拒否理由分類は不変 */
    int nm_fast = is_nm && s_nm_fast;
    if (c1 && c2 && c3 && c4 && c6 && c7 && cn) {
        uint8_t key = s_rl_cmd[1] & 7u;
        if (nm_fast && !s_rl_cf_first && s_rl_sds_valid[key]) {
            st3 = s_rl_sds_val[key];
            sds_ok = 1;
            lg.st3 = st3;
            lg.fail = "fast_cache";
            s_nm_sds_skip_fast++;
        } else {
            if (nm_fast && s_rl_cf_first) s_nm_sds_cf_first++;
            sds_ok = rl_wr_sds_fresh(&st3, &lg);
            if (nm_fast && sds_ok) {
                s_rl_sds_valid[key] = 1;
                s_rl_sds_val[key] = st3;
                s_rl_sds_t[key] = rl_now_us();
            }
        }
    }
    uint8_t want = (uint8_t)(0x60u | (s_rl_cmd[1] & 7u));
    uint8_t want_w = (uint8_t)(0x20u | (s_rl_cmd[1] & 7u));
    uint8_t want_a = (uint8_t)(0x20u | (s_rl_cmd[1] & 7u));   /* P1037 armed: FT=0・WP=0・RY=1・US/HD一致 */
    int send;
    if (is_nm)          /* P1053 normal(armed と同じ ST3 等式: FT=0・WP=0・RY=1・US/HD一致) */
        send = c1 && c2 && c3 && c4 && c8 && c9 && nm_unit && ar_drained && c10 && sds_ok && ((st3 & 0xE7u) == want_a);
    else if (is_ar)     /* P1037 armed(WP=1 のディスクへは送らない。マスク 0xE7 で WP を見る) */
        send = c1 && c2 && c3 && c4 && c7 && sds_ok && ((st3 & 0xE7u) == want_a);
    else if (!is_w1)    /* wponly: 送信判断はこの単一の正論理等式のみ(FT=0・WP=1・RY=1・US/HD一致。T0/TS は問わない) */
        send = c1 && c2 && c3 && c4 && sds_ok && ((st3 & 0xE7u) == want);
    else                /* P1035 mode1: FT=0・RY=1・US/HD一致(WPは問わない)+柵 */
        send = c1 && c2 && c3 && c4 && c6 && sds_ok && ((st3 & 0xA7u) == want_w);
    if (is_ar && sds_ok && !(st3 & ST3_RY)) rl_ar_disarm("ry0_at_write");   /* 書込み直前に媒体が無い=交換途中の疑い */
    if (is_nm && sds_ok && !(st3 & ST3_RY)) {   /* P1053: 当該ユニットの柵を閉じる(以後 READ 成功まで拒否) */
        uint8_t was = rl_nm_open_mask();
        rl_nm_fence_close(us_dev, NM_CL_RY0, 1, 1, was, NULL);
    }
    /* 以下の理由分類はログ用で、送信判断には使わない */
    const char* reason;
    if (is_nm) {
        /* P1053: 優先順は計画の「理由分類の優先順」どおり。refused_loop のときだけラッチする(上位の柵がすべて成立した上で
         * 連続上限を超えた場合。柵で拒否される書込みはラッチの契機にしない) */
        if (send)                              { reason = "pass_normal";        s_nm_pass++; }
        else if (!c10)                         { reason = "refused_format_verify"; s_nmf_wr_refused++; }   /* P1067 */
        else if (sds_ok && !(st3 & ST3_RY))    { reason = "refused_no_media";   s_nm_ref_no_media++; s_rl_wr_ref_no_media++; }
        else if (s_rl_wr_latched_off)          { reason = "refused_latched";    s_nm_ref_latched++; s_rl_wr_ref_latched++; }
        else if (s_nm_unit_state < 0)          { reason = "refused_fence_env";  s_nm_ref_env++; }
        else if (!nm_unit)                     { reason = "refused_fence_unit"; s_nm_ref_unit++; }
        else if (!c8 && s_nm_st != NM_ST_I && rl_nm_fence_open_nohold(us_dev))
                                               { reason = "refused_attr_hold";  s_nm_ref_attr_hold++; }   /* P1055: 判別保留中のみが原因 */
        else if (!c8)                          { reason = "refused_need_read";  s_nm_ref_need_read++; }
        else if (!c9)                          { reason = "refused_loop";       s_nm_ref_loop++; s_nm_loop_trip++;
                                                 rl_nm_latch("loop"); }
        else if (!ar_drained)                  { reason = "refused_drain";      s_nm_ref_drain++; }
        else if (!(c1 && c2 && c3) || !sds_ok) { reason = "refused_unknown";    s_nm_ref_unknown++; s_rl_wr_ref_unknown++; }
        else if (st3 & ST3_WP)                 { reason = "refused_wp";         s_nm_ref_wp++; }
        else                                   { reason = "refused_unknown";    s_nm_ref_unknown++; s_rl_wr_ref_unknown++; }   /* FT=1・US/HD不一致等 */
    } else if (is_ar) {
        /* P1037: 柵を送信上限より先に判定する(mode1 と同じ理由)。latched はこの呼出し内の disarm を反映するため再読する(Fable5 R2) */
        if (send)                              { reason = "pass_armed";           s_rl_ar_pass++; }
        else if (sds_ok && !(st3 & ST3_RY))    { reason = "refused_no_media";     s_rl_wr_ref_no_media++; }
        else if (s_rl_wr_latched_off)          { reason = "refused_latched";      s_rl_wr_ref_latched++; }
        else if (!s_rl_ar_env_ok)              { reason = "refused_fence_env";    s_rl_ar_ref_env++; }
        else if (!a_unit)                      { reason = "refused_fence_unit";   s_rl_ar_ref_unit++; }
        else if (a_mark)                       { reason = "refused_fence_marker"; s_rl_ar_ref_mark++; }   /* 武装の有無・範囲より先 */
        else if (!a_range)                     { reason = "refused_fence_range";  s_rl_ar_ref_range++; }
        else if (!s_rl_ar_armed)               { reason = "refused_not_armed";    s_rl_ar_ref_notarmed++; }
        else if (s_rl_wr_sends >= send_max)    { reason = "refused_send_max";     s_rl_ar_ref_max++; }
        else if (!ar_drained)                  { reason = "refused_drain";        s_rl_ar_ref_drain++; }
        else if (!(c1 && c2 && c3) || !sds_ok) { reason = "refused_unknown";      s_rl_wr_ref_unknown++; }
        else if (st3 & ST3_WP)                 { reason = "refused_wp";           s_rl_ar_ref_wp++; }
        else                                   { reason = "refused_unknown";      s_rl_wr_ref_unknown++; }   /* FT=1・US/HD不一致等 */
    } else if (!is_w1) {
        if (send)                                   reason = "pass";
        else if (latched)                           { reason = "refused_latched";  s_rl_wr_ref_latched++; }
        else if (!(c1 && c2 && c3 && c4) || !sds_ok) { reason = "refused_unknown";  s_rl_wr_ref_unknown++; }
        else if ((st3 & ST3_RY) && !(st3 & ST3_WP)) { reason = "refused_writable"; s_rl_wr_ref_writable++; }
        else if (!(st3 & ST3_RY))                   { reason = "refused_no_media"; s_rl_wr_ref_no_media++; }
        else                                        { reason = "refused_unknown";  s_rl_wr_ref_unknown++; }
    } else {
        /* 柵を送信上限より先に判定する(送信回数を消費済みの状態でも柵の拒否理由を区別して検証できるように) */
        if (send)                                   { reason = "pass_write";         s_rl_m1_pass++; }
        else if (latched)                           { reason = "refused_latched";    s_rl_wr_ref_latched++; }
        else if (!s_rl_wf_env_ok)                   { reason = "refused_fence_env";  s_rl_m1_ref_env++; }
        else if (!f_unit)                           { reason = "refused_fence_unit"; s_rl_m1_ref_unit++; }
        else if (!f_chr)                            { reason = "refused_fence_chr";  s_rl_m1_ref_chr++; }
        else if (s_rl_wr_sends >= send_max)         { reason = "refused_send_max";   s_rl_m1_ref_max++; }
        else if (!(c1 && c2 && c3) || !sds_ok)      { reason = "refused_unknown";    s_rl_wr_ref_unknown++; }
        else if (!(st3 & ST3_RY))                   { reason = "refused_no_media";   s_rl_wr_ref_no_media++; }
        else                                        { reason = "refused_unknown";    s_rl_wr_ref_unknown++; }
    }
    uint8_t want_log = (is_ar || is_nm) ? want_a : (is_w1 ? want_w : want);
    uint8_t mask_log = (is_ar || is_nm) ? 0xE7u : (is_w1 ? 0xA7u : 0xE7u);
    char mxs[16];
    (void)rl_wr_max_str(mxs, sizeof(mxs));   /* P1053: normal は上限なしなので "-" */
    if (s_rl_wr_pre_lines < FU_LOG_LIMIT) {
        char mh[2 * FU_WR_SDS_RQM_POLL_MAX + 4];
        s_rl_wr_pre_lines++;
        rl_hex(mh, sizeof(mh), lg.msr_pre, lg.polls, FU_WR_SDS_RQM_POLL_MAX);
        debug_log(P1032_TAG " precheck f=%d C=%u H=%u R=%u N=%u cmd=0x%02x us_mx=0x%02x us_dev=0x%02x"
                  " n=%u collect_n=%u expect=%u mtc_begin=0x%04x c1=%d mar0_valid=%d ocr0=0x%02x c2=%d"
                  " relay=%d gen_begin=%u gen_now=%u d_begin=%u d_now=%u c3=%d sends=%u/%s latched=%d c4=%d"
                  " sds_ret_tx=%d sds_msr_pre=%s sds_polls=%u sds_ret_st3=%d st3_dev=0x%02x sds_ret_post=%d"
                  " sds_msr_post=0x%02x sds_ok=%d sds_fail=%s want=0x%02x st3_masked=0x%02x RY=%d WP=%d FT=%d"
                  " decision=%s us=%lld\n",
                  g_mx68k_frame_num, p[2], p[3], p[4], p[5], (unsigned)p[0], (unsigned)s_rl_us_mx,
                  (unsigned)(p[1] & 7u), (unsigned)n, (unsigned)s_rl_wrcollect_n, (unsigned)expect,
                  (unsigned)s_rl_wr_mtc_begin, c1, s_rl_wrcollect_mar0_valid, (unsigned)s_rl_wrcollect_ocr0, c2,
                  relay, (unsigned)s_rl_wr_gen_begin, (unsigned)gen_now, (unsigned)s_rl_wr_d_begin, (unsigned)d_now,
                  c3, (unsigned)s_rl_wr_sends, mxs, latched, c4,
                  lg.ret_tx, lg.polls ? mh : "-", (unsigned)lg.polls, lg.ret_st3, (unsigned)lg.st3, lg.ret_post,
                  (unsigned)lg.msr_post, sds_ok, lg.fail, (unsigned)want_log, (unsigned)(lg.st3 & mask_log),
                  (lg.st3 & ST3_RY) ? 1 : 0, (lg.st3 & ST3_WP) ? 1 : 0, (lg.st3 & ST3_FT) ? 1 : 0,
                  reason, (long long)(rl_now_us() - t0));
    }
    if (is_w1 && s_rl_m1_lines < FU_LOG_LIMIT) {
        /* P1035: 柵の判定元(許可値と実際の値)を全て出す。fence_ok は本コマンドでの柵判定(f_unit&&f_chr) */
        char ac[24];
        s_rl_m1_lines++;
        if (s_rl_wf_env_ok) snprintf(ac, sizeof(ac), "%u,%u,%u,%u", s_rl_wf_chrn[0], s_rl_wf_chrn[1], s_rl_wf_chrn[2], s_rl_wf_chrn[3]);
        else                snprintf(ac, sizeof(ac), "-");
        debug_log(P1035_TAG " precheck f=%d us_mx=0x%02x us_dev=%u allow_unit=%d f_unit=%d C=%u H=%u R=%u N=%u"
                  " allow_chr=%s f_chr=%d fence_ok=%d sends=%u/%u want_w=0x%02x st3_masked_w=0x%02x WP_obs=%d decision=%s\n",
                  g_mx68k_frame_num, (unsigned)s_rl_us_mx, (unsigned)us_dev, s_rl_wf_unit, f_unit,
                  p[2], p[3], p[4], p[5], ac, f_chr, (f_unit && f_chr) ? 1 : 0,
                  (unsigned)s_rl_wr_sends, (unsigned)send_max, (unsigned)want_w, (unsigned)(lg.st3 & 0xA7u),
                  (lg.st3 & ST3_WP) ? 1 : 0, reason);
    }
    if (is_ar) {
        /* P1037: 1書込み1行(専用予算、送信の有無によらず出す)。柵の判定元・生値を全て出す */
        uint32_t seq = ++s_rl_ar_w_seq;
        if (s_rl_ar_w_lines < FU_AR_LOG_LIMIT) {
            char cy[8], df[16];
            s_rl_ar_w_lines++;
            if (s_rl_ar_env_ok) snprintf(cy, sizeof(cy), "%u-%u", s_rl_ar_cmin, s_rl_ar_cmax);
            else                snprintf(cy, sizeof(cy), "-");
            if (c1) snprintf(df, sizeof(df), "0x%08x", (unsigned)rl_fnv(s_rl_wrcollect_buf, n));
            else    snprintf(df, sizeof(df), "-");
            debug_log(P1037_TAG " w seq=%u f=%d us_mx=0x%02x us_dev=%u allow_unit=%d C=%u H=%u R=%u N=%u hd_bit=%u"
                      " cyl=%s f_unit=%d f_range=%d f_mark=%d armed=%d sends=%u/%u gen=%u arm_gen=%u drained=%d"
                      " sds_ok=%d st3_dev=0x%02x decision=%s data_fnv=%s\n",
                      (unsigned)seq, g_mx68k_frame_num, (unsigned)s_rl_us_mx, (unsigned)us_dev, s_rl_ar_unit,
                      p[2], p[3], p[4], p[5], (unsigned)((s_rl_cmd[1] >> 2) & 1u), cy, a_unit, a_range, a_mark,
                      s_rl_ar_armed, (unsigned)s_rl_wr_sends, (unsigned)send_max, (unsigned)gen_now,
                      (unsigned)s_rl_ar_arm_gen, ar_drained, sds_ok, (unsigned)lg.st3, reason, df);
        }
        if (send) { s_rl_ar_cur_valid = 1; s_rl_ar_cur_idx = rl_ar_idx(p[2], p[3], p[4]); s_rl_ar_cur_seq = seq; }   /* a_range で範囲検査済み */
    }
    if (is_nm) {
        /* P1053: 1書込み1行(専用予算、送信の有無によらず出す)。派生値 fence_open をその生値(スナップショットと現在値)と
         * 同じ行に出す。loop_key は直前に装置へ送った書込みのキー、loop_cnt はこの書込みを送れば同一キーの何回目か */
        uint32_t seq = ++s_nm_w_seq;
        if (s_nm_w_lines < FU_AR_LOG_LIMIT) {
            char lk[32], df[16], ua[8];
            s_nm_w_lines++;
            if (s_nm_loop_valid) snprintf(lk, sizeof(lk), "%u,%u,%u,%u,%u", s_nm_loop_key[0], s_nm_loop_key[1], s_nm_loop_key[2],
                                          s_nm_loop_key[3], s_nm_loop_key[4]);
            else                 snprintf(lk, sizeof(lk), "-");
            if (c1) snprintf(df, sizeof(df), "0x%08x", (unsigned)rl_fnv(s_rl_wrcollect_buf, n));
            else    snprintf(df, sizeof(df), "-");
            if (s_nm_unit_state == 0)      snprintf(ua, sizeof(ua), "all");
            else if (s_nm_unit_state == 1) snprintf(ua, sizeof(ua), "%d", s_nm_unit);
            else                           snprintf(ua, sizeof(ua), "bad");
            debug_log(P1053_TAG " w seq=%u f=%d us_mx=0x%02x us_dev=%u C=%u H=%u R=%u N=%u allow_unit=%s f_unit=%d"
                      " fo_valid=%d fo_ue=%u ue=%u hold=%d fo_gen=%u d_now=%u gen_now=%u fence_open=%d loop_key=%s loop_cnt=%u/%u"
                      " loop_age_ms=%lld loop_time_reset=%d drained=%d latched=%d sends=%u/- sds_ok=%d st3_dev=0x%02x"
                      " decision=%s data_fnv=%s c3=%d d_begin=%u sds_fail=%s ue_begin=%u c3n=%d c10=%d\n",
                      (unsigned)seq, g_mx68k_frame_num, (unsigned)s_rl_us_mx, (unsigned)us_dev, p[2], p[3], p[4], p[5], ua,
                      nm_unit, s_nm_fo_valid[us_dev], (unsigned)s_nm_fo_ue[us_dev], (unsigned)s_nm_ue[us_dev],
                      s_nm_st != NM_ST_I ? 1 : 0, (unsigned)s_nm_fo_gen[us_dev],
                      (unsigned)d_now, (unsigned)gen_now, c8, lk, (unsigned)loop_next, (unsigned)FU_WR_LOOP_MAX,
                      s_nm_loop_valid ? (long long)((nm_now - s_nm_loop_t_us) / 1000) : -1LL, nm_time_reset, ar_drained,
                      s_rl_wr_latched_off, (unsigned)s_rl_wr_sends, sds_ok, (unsigned)lg.st3, reason, df,
                      /* P1057: s64型(refused_unknown)の真因確定用。c3='D'/'d'収集一致、d_begin=収集開始時点のd計数、sds_fail=SDS失敗理由 */
                      c3, (unsigned)s_rl_wr_d_begin, lg.fail, (unsigned)s_rl_wr_ue_begin[us_dev], c3, c10);
        }
    }
    /* P1051: mode1/armed は送信直前に同期手順。would_cache は挿入 SEEK が無かった場合に装置がキャッシュ経路を選ぶか。
     * P1053: normal も同期手順を通す(P1051 と同じ write-behind 回避の保証) */
    int is_sync = is_ar || is_w1 || is_nm;
    int cf_first0 = s_rl_cf_first;
    uint8_t cf_cmd0 = s_rl_cf_cmd;
    int would = (!cf_first0) && (cf_cmd0 == s_rl_cmd[0]);
    rl_sync_log_t sl;
    memset(&sl, 0, sizeof(sl));
    sl.via = "-";
    sl.msr_ret = -9;
    if (!send) {
        if (is_sync) rl_sync_log(&sl, reason, "no_sync", would, cf_first0, cf_cmd0, -9);
        rl_synth_err(0x02u, 1);
        rl_raise_int();
        return;
    }
    /* P1089(B-3): 高速モードは同期 SEEK を挿入しない(装置の write-behind を使う)。[P1051-SYNC] 行は decision=fast_nosync */
    if (nm_fast) {
        s_nm_fast_nosync++;
    } else if (is_sync) {
        if (!rl_wr_sync_seek(&sl, s_rl_write_mode)) {
            /* 書込みは送らない。従来の拒否と同形(NW+割込み、ラッチしない。-2/-3 は rl_p1 内で故障停止済み)。
             * P1053: normal は loop_cnt を更新しない(装置へ書込みを送っていない)。迷子'C'の計数を始める(R11) */
            s_rl_sy_refused++;
            s_rl_ar_cur_valid = 0;
            if (is_nm) s_nm_sy_fail_pending = 1;
            rl_sync_log(&sl, reason, "sync_refused", would, cf_first0, cf_cmd0, -9);
            rl_synth_err(0x02u, 1);
            rl_raise_int();
            return;
        }
        s_rl_sy_ok++;
    }
    s_rl_wr_wp_at_send = (st3 & ST3_WP) ? 1 : 0;   /* P1035: classify の期待値選択用(wponly では等式上常に1) */
    int r = rl_send_write_cmd(n);
    if (r != -1) s_rl_last_io_frame[s_rl_us_mx & 3u] = g_mx68k_frame_num;   /* P1092: 表示用 */
    if (is_nm && r != -1) {
        /* P1053: 装置へ書込みコマンドを送った(送信途中失敗 -2/-3 を含む)時点でだけ連続検知のキーと回数を更新する */
        if (nm_time_reset) s_nm_loop_time_reset++;
        s_nm_loop_cnt = loop_next;
        memcpy(s_nm_loop_key, nm_key, 5);
        s_nm_loop_valid = 1;
        s_nm_loop_t_us = rl_now_us();
        if (s_nm_loop_cnt > s_nm_loop_cnt_max) s_nm_loop_cnt_max = s_nm_loop_cnt;
        /* 照合表の記録先。エポックは収集開始時(送信判定 c3 で現在値と一致を確認済み)の値 */
        s_nm_cur_valid = 1;
        s_nm_cur_us = us_dev;
        s_nm_cur_mapped = p[2] < FU_AR_NCYL && p[3] < FU_AR_NHEAD && p[4] >= 1u && p[4] <= FU_AR_NSEC && p[5] == 3u;
        s_nm_cur_idx = s_nm_cur_mapped ? rl_ar_idx(p[2], p[3], p[4]) : 0u;
        s_nm_cur_ue = s_rl_wr_ue_begin[us_dev];
        s_nm_cur_gen = s_rl_wr_gen_begin;
    }
    if (r == -1) {   /* 未接続: 何も届いていない */
        s_rl_ar_cur_valid = 0;   /* P1037: 送っていないので照合表には記録しない */
        if (is_sync) rl_sync_log(&sl, reason, "send_fail", would, cf_first0, cf_cmd0, r);
        rl_synth_err(0x02u, 1);
        rl_raise_int();
        return;
    }
    if (r < 0) {     /* -2/-3: 送信途中で失敗し得る(rl_p1 内で故障停止済み) */
        s_rl_wr_sends++;
        if (is_sync) rl_sync_log(&sl, reason, "send_fail", would, cf_first0, cf_cmd0, r);
        rl_wr_fail_unknown("unknown_p1");
        return;
    }
    s_rl_wr_sends++;
    if (is_sync) {
        if (would) s_rl_sy_would_cache_n++;
        rl_sync_log(&sl, reason, nm_fast ? "fast_nosync" : "sent", would, cf_first0, cf_cmd0, r);
    }
    s_fu_wc_send_est = s_fu_wc_est;   /* P1089(M2): 'W' 待ち時間を送信時の推定件数別に記録する */
    rl_wr_enter_wait_w(n);
}

/* P1032: 'W' 応答。呼出し側で4条件(状態=WR_WAIT_W・世代一致・未送信・'D'無し)を確認済み */
static void rl_wr_send_data(void)
{
    uint32_t n = s_rl_wr_n;
    s_rl_wr_p2buf[0] = FU_P2_DATA_START;
    memcpy(s_rl_wr_p2buf + 1, s_rl_wrcollect_buf, n);
    s_rl_wr_data_sent = 1;   /* 送信直前に立てる。戻り値に関わらず再送しない */
    s_rl_wr_data_sent_f = g_mx68k_frame_num;   /* P1099 */
    int64_t t0 = rl_now_us();
    int r = mx68k_floppyu_usb_port2_write(s_rl_wr_p2buf, n + 1u, FU_P2_WRITE_TIMEOUT_MS);
    int64_t us = rl_now_us() - t0;
    s_rl_blk_frame_us += us;
    if (s_rl_wr_lines < FU_LOG_LIMIT) {
        s_rl_wr_lines++;
        debug_log(P1032_TAG " data f=%d gen=%u len=%u ret=%d us=%lld data_fnv=0x%08x collect_fnv=0x%08x since_send_us=%lld\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)(n + 1u), r, (long long)us,
                  (unsigned)rl_fnv(s_rl_wr_p2buf + 1, n), (unsigned)rl_fnv(s_rl_wrcollect_buf, s_rl_wrcollect_pos),
                  (long long)(t0 - s_rl_wr_send_t_us));
    }
    if (r == (int)(n + 1u)) {
        s_rl_wr_data_sent_n++;
        s_rl_state = RL_WR_WAIT_DONE;
        s_rl_wr_state_t_us = rl_now_us();
        return;
    }
    rl_wr_fail_unknown("unknown_p2");   /* -1/-2/-3/部分送信(-3 は通信層が teardown 投入済み) */
}

/* ===========================================================================
 *  P1064: fmt1(単一トラック限定の実FORMAT送信)
 * =========================================================================== */

/* 同期 READ ID の記録(precheck/verify 行に全て出す) */
typedef struct {
    int      ret_tx, ret_post, res_valid;
    uint8_t  msr_seq[16];
    uint32_t msr_n, polls;
    uint8_t  res[FU_RESULT_LEN];
    uint8_t  msr_post;
    uint32_t c, cc;
    int64_t  us, drain_us;
    const char* fail;
} rl_rid_fresh_log_t;

/* READ ID 待ち中の装置MSR問合せ間隔。眠った時間はフレームのブロック時間へ計上する */
static void rl_fmt1_rid_sleep(void)
{
    int64_t t0 = rl_now_us();
    struct timespec ts = { 0, (long)FU_FMT_RID_POLL_SLEEP_US * 1000L };
    nanosleep(&ts, NULL);
    s_rl_blk_frame_us += rl_now_us() - t0;
}

/* 非キャッシュ・同期の READ ID(装置への読取り専用の問合せ)。s_rl_state・リザルト・READ ID 中継の状態には触れない。
 *   1) s_rl_sync_active=1(READ ID の'C'はゲストへ渡さず数えるだけ、'c'も数える。P1051 の既存機構)
 *   2) 83 4A 83 <ushd> を送る(結果は読まない)
 *   3) 装置MSRを FU_FMT_RID_DEADLINE_MS まで問い合わせ、(msr&0xC0)==0xC0 で7Bを 03 で1Bずつ読む
 *   4) 装置MSRを読み (msr&0xF0)==0x80(アイドル復帰)を確認する
 *   5) port2 を FU_FMT_DRAIN_MS まで rl_poll() で読む('C'と'c'を両方見た時点で打切り)。
 *      c>=1 && cc>=1 のときだけ成立。c>=1 && cc==0 は c_without_cc、c==0 && cc==0 は no_c(Fable5 C-1:
 *      0件は「'C'がまだ届いていない=送信後に遅れて届き得る」状態なので不成立)
 *   6) s_rl_sync_active=0
 * 戻り値: 1=全て成立。lg->res_valid=1 なら lg->res は結果フェーズで読んだ7B(手順の成否と独立に判定へ使える) */
static int rl_fmt1_readid_fresh(uint8_t ushd, rl_rid_fresh_log_t* lg)
{
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    int64_t t0 = rl_now_us();
    memset(lg, 0, sizeof(*lg));
    lg->ret_tx = -9;
    lg->ret_post = -9;
    lg->fail = "none";
    if (s_rl_in_poll) { lg->fail = "reentry"; lg->us = rl_now_us() - t0; return 0; }   /* port2 を読めない */
    s_rl_sync_active = 1;
    s_rl_sync_c = 0;
    s_rl_sync_cc = 0;
    {
        uint8_t tx[4] = { FU_WR | FU_REG_DATA, 0x4A, FU_WR | FU_REG_DATA, ushd };
        rl_wc_note_trigger(WCF_CMD);   /* P1089: READ ID は flush 契機 */
        lg->ret_tx = rl_p1("fmt1_rid_cmd", tx, sizeof(tx), NULL, 0);
        if (lg->ret_tx < 0) { lg->fail = "tx"; goto drain; }
    }
    {
        int64_t dl = t0 + (int64_t)FU_FMT_RID_DEADLINE_MS * 1000;
        int in_result = 0;
        for (;;) {
            uint8_t msr = 0;
            if (rl_p1("fmt1_rid_msr", TX_MSR, 1, &msr, 1) < 1) { lg->fail = "msr_rx"; goto drain; }
            lg->polls++;
            if (lg->msr_n < sizeof(lg->msr_seq)) lg->msr_seq[lg->msr_n++] = msr;
            if ((msr & 0xC0u) == 0xC0u) { in_result = 1; break; }
            if (rl_now_us() >= dl) break;
            rl_fmt1_rid_sleep();
        }
        if (!in_result) { lg->fail = "timeout"; goto drain; }
    }
    for (uint32_t i = 0; i < FU_RESULT_LEN; i++)
        if (rl_p1("fmt1_rid_result", TX_DATA, 1, &lg->res[i], 1) < 1) { lg->fail = "result_rx"; goto drain; }
    lg->res_valid = 1;
    lg->ret_post = rl_p1("fmt1_rid_msr_post", TX_MSR, 1, &lg->msr_post, 1);
    if (lg->ret_post < 1) { lg->fail = "msr_post_rx"; goto drain; }
    if ((lg->msr_post & 0xF0u) != 0x80u) { lg->fail = "msr_not_idle"; goto drain; }
drain:
    {
        int64_t td = rl_now_us(), dend = td + (int64_t)FU_FMT_DRAIN_MS * 1000;
        for (;;) {
            rl_poll();
            if (s_rl_sync_c > 0u && s_rl_sync_cc > 0u) break;
            if (rl_now_us() >= dend) break;
            rl_sync_sleep();
        }
        lg->drain_us = rl_now_us() - td;
    }
    lg->c = s_rl_sync_c;
    lg->cc = s_rl_sync_cc;
    s_rl_sync_active = 0;
    int ok = 0;
    if (strcmp(lg->fail, "none") == 0) {
        if (lg->c >= 1u && lg->cc >= 1u) ok = 1;
        else if (lg->c >= 1u)            lg->fail = "c_without_cc";
        else                             lg->fail = "no_c";
    }
    lg->us = rl_now_us() - t0;
    return ok;
}

/* 拒否: NW を合成する。CHRN 欄はエコーしない(FORMAT の s_rl_cmd[2..5] は N/SC/GPL/D で CHRN ではない、Fable5 F-6)。
 * 装置へ書込みを送っていないのでラッチしない */
static void rl_fmt1_refuse(int ref)
{
    s_rl_fmt1_ref[ref]++;
    rl_synth_err(0x02u, 0);
    rl_raise_int();
}

/* P1066: 永久停止ラッチ。未設定のときだけ理由を記録する(最初の理由を保持、解除経路なし) */
static void rl_fmt1_halt(const char* why)
{
    if (s_rl_fmt1_halt) return;
    s_rl_fmt1_halt = 1;
    s_rl_fmt1_halt_why = why;
}

/* P1066: 許可トラックのログ表記。単一: "C,H"(従来どおり)、複数: "0,0+0,1"。
 * dash_if_ng=1 なら柵不成立時は "-"(precheck 行の従来表記)、0 なら柵の成否に関わらず値を出す(sum 行の従来表記) */
static void rl_fmt1_ac_str(char* out, size_t sz, int dash_if_ng)
{
    if (dash_if_ng && !s_rl_fm_env_ok) snprintf(out, sz, "-");
    else if (s_rl_fm_multi) snprintf(out, sz, "%u,0+%u,1", s_rl_fm_c, s_rl_fm_c);
    else                    snprintf(out, sz, "%u,%u", s_rl_fm_c, (s_rl_fm_hmask & 2u) ? 1u : 0u);
}

/* 他方の装置ユニットの直近 SDS キャッシュ(補助情報。追加の装置問合せはしない、Requirements Review S2) */
static void rl_fmt1_other_sds_str(uint8_t us_dev, char* out, size_t sz)
{
    int64_t now = rl_now_us();
    size_t o = 0;
    out[0] = '\0';
    for (uint8_t h = 0; h < 2u; h++) {
        uint8_t k = (uint8_t)(((us_dev ^ 1u) & 3u) | (h << 2));
        if (s_rl_sds_valid[k])
            o += (size_t)snprintf(out + o, sz - o, "%sk%u:0x%02x@%lldms", h ? "," : "", (unsigned)k, (unsigned)s_rl_sds_val[k],
                                  (long long)((now - s_rl_sds_t[k]) / 1000));
        else
            o += (size_t)snprintf(out + o, sz - o, "%sk%u:-", h ? "," : "", (unsigned)k);
        if (o >= sz) break;
    }
}

/* READ ID の1回分を "<7B hex>" または "-(<fail>)" にする */
static void rl_fmt1_rid_str(const rl_rid_fresh_log_t* lg, char* out, size_t sz)
{
    if (lg->res_valid) rl_hex(out, sz, lg->res, FU_RESULT_LEN, FU_RESULT_LEN);
    else               snprintf(out, sz, "-");
}

/* 送信成功後の検証 READ ID(書込みなし)。起動条件が全成立のときだけ FU_FMT1_VERIFY_N 回行い、1行に出す。
 * 不成立なら verify_skipped_<理由>。いずれの場合も予約(s_rl_fmt1_verify_pending)は解除する。
 * P1065: at_seek=1 はゲストの SEEK/RECAL を装置へ中継する直前(rl_seek 冒頭)、at_seek=0 は Phase 0(FORMAT 収集完了時)。
 * 戻り値は検証を終えた時点の rl_relay_ok()(検証しなかった場合も評価して返す。SEEK 起動では中継可否の取り直しに使う) */
static int rl_fmt1_verify(int at_seek)
{
    const uint8_t* p = s_rl_cmd;
    uint8_t us_dev = p[1] & 3u;
    int relay = rl_relay_ok();
    uint32_t gen = mx68k_floppyu_usb_generation();
    const char* skip = NULL;
    const char* skip_why = NULL;   /* P1066: 停止ラッチの理由(文字列リテラルを保持する) */
    /* unit ゲートは SEEK 起動でも残す(別ユニットへの SEEK では検証しない、Fable5 M-1) */
    if ((int)us_dev != s_rl_fm_unit)             { skip = "unit";    skip_why = "verify_skipped_unit"; }
    else if (!relay)                             { skip = "link";    skip_why = "verify_skipped_link"; }
    else if (gen != s_rl_fmt1_v_gen)             { skip = "gen";     skip_why = "verify_skipped_gen"; }
    else if (s_rl_d_events != s_rl_fmt1_v_d)     { skip = "d_event"; skip_why = "verify_skipped_d_event"; }
    else if (s_rl_fmt1_seek_n != s_rl_fmt1_v_seek) { skip = "seek"; skip_why = "verify_skipped_seek"; }   /* SEEK 起動では seek_n++ より前に呼ぶので常に等しい(多重防御) */
    else if (s_rl_in_poll)                       { skip = "reentry"; skip_why = "verify_skipped_reentry"; }
    s_rl_fmt1_verify_pending = 0;
    char cond[352];
    snprintf(cond, sizeof(cond), "us_dev=%u allow_unit=%d relay=%d gen=%u/%u d=%u/%u seek_n=%u/%u in_poll=%d"
             " trigger=%s guest_cmd=0x%02x guest_us=0x%02x v_c=%u v_h=%u",
             (unsigned)us_dev, s_rl_fm_unit, relay, (unsigned)gen, (unsigned)s_rl_fmt1_v_gen, (unsigned)s_rl_d_events,
             (unsigned)s_rl_fmt1_v_d, (unsigned)s_rl_fmt1_seek_n, (unsigned)s_rl_fmt1_v_seek, s_rl_in_poll,
             at_seek ? "seek" : "fmt", (unsigned)p[0], (unsigned)p[1], (unsigned)s_rl_fm_c, (unsigned)s_rl_fmt1_v_h);
    if (skip) {
        s_rl_fmt1_verify_skipped++;
        rl_fmt1_halt(skip_why);   /* P1066: 検証できなかったトラックの次へ書込みを進めない */
        int post = rl_relay_ok();
        if (!post) s_rl_fmt1_relay_lost_n++;
        if (s_rl_fmt1_lines < FU_AR_LOG_LIMIT) {
            s_rl_fmt1_lines++;
            debug_log(P1064_TAG " verify f=%d verify=skipped_%s %s post_relay=%d halt_after=%d chain_ok_after=%d"
                      " runs=%u ok=%u bad=%u skipped=%u\n",
                      g_mx68k_frame_num, skip, cond, post, s_rl_fmt1_halt, s_rl_fmt1_chain_ok,
                      (unsigned)s_rl_fmt1_verify_runs, (unsigned)s_rl_fmt1_verify_ok,
                      (unsigned)s_rl_fmt1_verify_bad, (unsigned)s_rl_fmt1_verify_skipped);
        }
        return post;
    }
    s_rl_fmt1_verify_runs++;
    if (at_seek) s_rl_fmt1_verify_at_seek++; else s_rl_fmt1_verify_at_fmt++;
    /* P1066: 検証対象は直前に送信したトラック(C は s_rl_fm_c=0、H は s_rl_fmt1_v_h) */
    uint8_t ushd = (uint8_t)(((unsigned)s_rl_fm_unit & 3u) | ((unsigned)s_rl_fmt1_v_h << 2));
    char all[FU_FMT1_VERIFY_N * 48u + 8];
    size_t o = 0;
    int good = 1;
    uint32_t done = 0;
    uint32_t c_total = 0, cc_total = 0;   /* P1065(C-1): 各回の'C'/'c'吸収数の合計('C'の分母) */
    uint8_t rmask = 0;
    all[0] = '\0';
    for (uint32_t i = 0; i < FU_FMT1_VERIFY_N; i++) {
        rl_rid_fresh_log_t lg;
        (void)rl_fmt1_readid_fresh(ushd, &lg);
        done++;
        c_total += lg.c;
        cc_total += lg.cc;
        const uint8_t* r = lg.res;
        int ok_i = lg.res_valid && r[0] == (ushd & 7u) && r[1] == 0u && r[2] == 0u && r[3] == s_rl_fm_c &&
                   r[4] == s_rl_fmt1_v_h && r[6] == 3u && r[5] >= 1u && r[5] <= 8u;
        if (ok_i) rmask |= (uint8_t)(1u << (r[5] - 1u));
        else      good = 0;
        char rh[2 * 8 + 4];
        rl_fmt1_rid_str(&lg, rh, sizeof(rh));
        if (o + 48u < sizeof(all))
            o += (size_t)snprintf(all + o, sizeof(all) - o, "%s%s/%s/c%u,%u", i ? "," : "", rh, lg.fail,
                                  (unsigned)lg.c, (unsigned)lg.cc);
        if (lg.ret_tx < 0) break;   /* 通信不能(以後の問合せは意味を持たない) */
    }
    if (done < FU_FMT1_VERIFY_N) good = 0;
    /* P1065: 検証後の吸収窓と再検査(両起動点に共通)。
     * 1) s_rl_sync_active を明示的に立て直す(readid_fresh は終了時に毎回0へ戻す)。0のまま rl_poll() を呼ぶと、
     *    遅着した'C'が非同期経路へ入り、SEEK 送信前のため unsolicited としてゲストへ余分な割込みを出してしまう
     * 2) rl_poll() を必ず1回呼び、8回目の drain 打切り後に受信リングへ残った'C'/'D'等を取り込む
     * 3) 'C'が不足していれば(c_deficit>0)、FU_FMT_DRAIN_MS まで追加で吸収する(C-1)
     * 4) 吸収数を写してから s_rl_sync_active を0へ戻す
     * 5) 媒体交換('D'/'d')・世代変化・受信リング溢れがあれば verify=bad へ倒す */
    uint32_t c_deficit = done > c_total ? done - c_total : 0u;
    s_rl_sync_active = 1;
    s_rl_sync_c = 0;
    s_rl_sync_cc = 0;
    rl_poll();
    if (c_deficit > 0u) {
        int64_t dend = rl_now_us() + (int64_t)FU_FMT_DRAIN_MS * 1000;
        while (s_rl_sync_c < c_deficit && rl_now_us() < dend) {
            rl_sync_sleep();
            rl_poll();
        }
    }
    uint32_t late_c = s_rl_sync_c, late_cc = s_rl_sync_cc;
    s_rl_sync_active = 0;
    uint32_t c_deficit_final = c_deficit > late_c ? c_deficit - late_c : 0u;
    if (c_deficit_final > 0u) s_rl_fmt1_c_deficit_n++;
    uint32_t d_after = s_rl_d_events, gen_after = mx68k_floppyu_usb_generation();
    int rx_ovf = mx68k_floppyu_usb_port2_rx_overflowed() ? 1 : 0;
    char bad_after[48];
    size_t bo = 0;
    bad_after[0] = '\0';
    if (d_after != s_rl_fmt1_v_d)     bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%sd_event", bo ? "+" : "");
    if (gen_after != s_rl_fmt1_v_gen) bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%sgen", bo ? "+" : "");
    if (rx_ovf)                       bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%srx_overflow", bo ? "+" : "");
    if (bo) good = 0;
    else    snprintf(bad_after, sizeof(bad_after), "-");
    uint32_t cover = 0;
    for (uint32_t b = 0; b < 8u; b++) if ((rmask >> b) & 1u) cover++;
    if (good) s_rl_fmt1_verify_ok++; else s_rl_fmt1_verify_bad++;
    /* P1066(Fable5 C-1): 連鎖ゲートは good に加えて検証 READ ID の'C'を全て受け取ったこと(c_deficit_final==0)も要求する。
     * 飛行中の'C'を残したまま次トラックを送ると、遅着した'C'が 'W' 受理ゲートを不成立にして装置が固着し得るため */
    if (good && c_deficit_final == 0u) s_rl_fmt1_chain_ok = 1;
    else if (good)                     rl_fmt1_halt("verify_c_deficit");
    else                               rl_fmt1_halt("verify_bad");
    /* 吸収しきれなかった'C'がゲストへ渡る場合を数える(挙動は変えない)。P1065(C-1): SEEK 起動では直後に中継する
     * ゲスト自身の SEEK の正規の完了'C'を迷子として数えてしまうので窓を開かない(c_deficit_final で代わりに捕捉) */
    if (!at_seek) s_rl_fmt1_stray_pending = 1;
    int post = rl_relay_ok();   /* P1065(C-2): 検証の間に中継不可になっていないか取り直す */
    if (!post) s_rl_fmt1_relay_lost_n++;
    if (s_rl_fmt1_lines < FU_AR_LOG_LIMIT) {
        s_rl_fmt1_lines++;
        debug_log(P1064_TAG " verify f=%d verify=%s %s ushd=0x%02x n=%u rids=%s r_mask=0x%02x r_cover=%u/8"
                  " c_total=%u cc_total=%u c_deficit=%u late_c_absorbed=%u late_cc_absorbed=%u c_deficit_final=%u"
                  " d_after=%u gen_after=%u rx_overflow=%d bad_after=%s post_relay=%d halt_after=%d chain_ok_after=%d"
                  " runs=%u ok=%u bad=%u skipped=%u\n",
                  g_mx68k_frame_num, good ? "ok" : "bad", cond, (unsigned)ushd, (unsigned)done, all, (unsigned)rmask,
                  (unsigned)cover, (unsigned)c_total, (unsigned)cc_total, (unsigned)c_deficit, (unsigned)late_c,
                  (unsigned)late_cc, (unsigned)c_deficit_final, (unsigned)d_after, (unsigned)gen_after, rx_ovf, bad_after,
                  post, s_rl_fmt1_halt, s_rl_fmt1_chain_ok, (unsigned)s_rl_fmt1_verify_runs, (unsigned)s_rl_fmt1_verify_ok,
                  (unsigned)s_rl_fmt1_verify_bad, (unsigned)s_rl_fmt1_verify_skipped);
    }
    return post;
}

/* 収集完了時(rl_collect_done 経由、Core DMA ch0 の最終バイト書込みコールバック内で同期実行)。
 * Phase 0(送信済み)→Phase A(装置へ触れない検査)→Phase B(SDS・READ ID)→C0(再検査)→送信→同期'W'待ち。
 * 判定に使った生値は1 FORMAT につき1行の precheck 行へ全て出す(送信の有無に関わらず) */
static void rl_fmt1_try_send(void)
{
    const uint8_t* p = s_rl_cmd;   /* [0]cmd [1]HD/US(装置座標) [2]N [3]SC [4]GPL [5]D */
    const uint8_t* ids = s_rl_wrcollect_buf;
    uint32_t n = s_rl_wrcollect_pos;
    int64_t t0 = rl_now_us();
    uint8_t us_dev = p[1] & 3u;
    uint8_t hd = (uint8_t)((p[1] >> 2) & 1u);
    char idh[2 * 32 + 8], ac[16], mxs[16];
    rl_hex(idh, sizeof(idh), ids, n, 32);
    rl_fmt1_ac_str(ac, sizeof(ac), 1);

    /* Phase 0: 送信を試みた後なら、予約があれば検証 READ ID を1回だけ行ってから次のいずれかで拒否する(装置へ触れない)。
     * P1066: 評価順は !multi → halt → 全送信済み → chain(単一モードは halt の値に関係なく従来どおり refused_spent)。
     * ここを抜けて Phase A へ進むのは「複数モード・停止なし・未送信の面あり・前トラック検証 ok」のときだけ */
    if (s_rl_fmt1_spent) {
        if (s_rl_fmt1_verify_pending) (void)rl_fmt1_verify(0);   /* P1065: ゲストが SEEK せず次の FORMAT を出した場合の起動点 */
        int z = -1;
        if (!s_rl_fm_multi)                                                     z = FM_REF_SPENT;
        else if (s_rl_fmt1_halt)                                                z = FM_REF_HALT;
        else if ((s_rl_fmt1_spent_hmask & s_rl_fm_hmask) == s_rl_fm_hmask)      z = FM_REF_SPENT;
        else if (!s_rl_fmt1_chain_ok)                                           z = FM_REF_CHAIN;
        if (z >= 0) {
            if (s_rl_fmt1_pre_lines < FU_AR_LOG_LIMIT) {
                s_rl_fmt1_pre_lines++;
                debug_log(P1064_TAG " precheck f=%d phase=0 cmd=0x%02x hd_us=0x%02x us_mx=0x%02x us_dev=%u hd=%u N=%u SC=%u GPL=0x%02x"
                          " D=0x%02x n=%u ids=%s allow_unit=%d allow_ch=%s spent=%d sends=%u/%s latched=%d"
                          " multi=%d hmask=0x%02x spent_mask=0x%02x halt=%d halt_why=%s chain_ok=%d decision=%s\n",
                          g_mx68k_frame_num, p[0], p[1], (unsigned)s_rl_us_mx, (unsigned)us_dev, (unsigned)hd, p[2], p[3], p[4], p[5],
                          (unsigned)n, idh, s_rl_fm_unit, ac, s_rl_fmt1_spent, (unsigned)s_rl_wr_sends,
                          rl_wr_max_str(mxs, sizeof(mxs)), s_rl_wr_latched_off, s_rl_fm_multi, (unsigned)s_rl_fm_hmask,
                          (unsigned)s_rl_fmt1_spent_hmask, s_rl_fmt1_halt, s_rl_fmt1_halt_why, s_rl_fmt1_chain_ok,
                          FM_REF_NAME[z]);
            }
            rl_fmt1_refuse(z);
            return;
        }
    }

    /* Phase A: 装置へ一切触れない検査 */
    int f1 = (n == s_rl_wrcollect_n) && (n == (uint32_t)p[3] * 4u) && (n == (uint32_t)s_rl_wr_mtc_begin);
    int f2 = (s_rl_wrcollect_mar0_valid == 1) && ((s_rl_wrcollect_ocr0 & 0x80u) == 0u);
    int relay = rl_relay_ok();
    uint32_t gen_now = mx68k_floppyu_usb_generation();
    uint32_t d_now = s_rl_d_events;
    int f3 = relay && (gen_now == s_rl_wr_gen_begin) && (d_now == s_rl_wr_d_begin);
    int latched = s_rl_wr_latched_off;
    uint32_t send_max = rl_wr_send_max();
    int f4 = !latched && !s_rl_fmt1_halt && (s_rl_wr_sends < send_max);
    int f5 = s_rl_fm_env_ok;
    int f_unit = f5 && (int)us_dev == s_rl_fm_unit;
    int f_head = f5 && ((s_rl_fm_hmask >> hd) & 1u);   /* P1066: 面マスク(単一モードでは従来の hd == h と同値) */
    int f_param = (s_rl_cmd_n == 6u) && p[0] == 0x4Du && p[2] == 3u && p[3] == 8u && p[4] == 0x74u && p[5] == 0xE5u;
    int f6 = f_unit && f_head && f_param;
    int f7 = f5 && (n == 32u);
    for (uint32_t i = 0; f7 && i < 8u; i++)
        if (ids[i * 4u] != s_rl_fm_c || ids[i * 4u + 1u] != hd || ids[i * 4u + 2u] != (uint8_t)(i + 1u) ||
            ids[i * 4u + 3u] != 3u) f7 = 0;
    int f8 = !s_rl_in_poll;
    /* P1066: 追加ゲート(装置へ触れない)。単一モードの初回送信では spent_hmask==0 なので全て従来と同じ判定になる */
    uint8_t rem = (uint8_t)(s_rl_fm_hmask & (uint8_t)~s_rl_fmt1_spent_hmask);
    int expect_h = (rem & 1u) ? 0 : ((rem & 2u) ? 1 : -1);                 /* 次に許可する面(無ければ -1) */
    int f_order = f5 && (int)hd == expect_h;                                /* 単調順序: C0H0→C0H1 だけ */
    int f_repeat = !((s_rl_fmt1_spent_hmask >> hd) & 1u);                  /* 同じ面の再送禁止(f_order と重複する多重防御) */
    int f_chain = (s_rl_fmt1_spent_hmask == 0u) || (s_rl_fmt1_chain_ok && !s_rl_fmt1_verify_pending);
    int f_epoch = (s_rl_fmt1_spent_hmask == 0u) ||
                  (s_rl_fmt1_chain_set && d_now == s_rl_fmt1_chain_d0 && gen_now == s_rl_fmt1_chain_gen0);
    int phase_a = f1 && f2 && f3 && f4 && f5 && f6 && f7 && f8 && f_order && f_repeat && f_chain && f_epoch;

    /* Phase B: 装置への読取り専用の問合せ(Phase A 全成立時のみ) */
    rl_wr_sds_log_t sl;
    memset(&sl, 0, sizeof(sl));
    sl.ret_tx = -9; sl.ret_st3 = -9; sl.ret_post = -9; sl.fail = "skipped";
    rl_rid_fresh_log_t rg;
    memset(&rg, 0, sizeof(rg));
    rg.ret_tx = -9; rg.ret_post = -9; rg.fail = "skipped";
    uint8_t st3 = 0;
    uint8_t want = (uint8_t)(0x20u | (p[1] & 7u));   /* FT=0・WP=0・RY=1・US/HD エコー一致 */
    int sds_ok = 0, st3_eq = 0, b1_t0 = 0, b1 = 0, rid_ok = 0, rid_ic_ok = 0, rid_chs = 0, b2 = 0, c0 = 0;
    if (phase_a) {
        sds_ok = rl_wr_sds_fresh(&st3, &sl);
        st3_eq = sds_ok && ((st3 & 0xE7u) == want);
        /* T0 整合(ドライブの Track 0 センサー=記録IDと独立な物理信号)。本サイクルの許可Cは常に0なので実質 T0=1 必須 */
        b1_t0 = sds_ok && (((st3 & ST3_T0) != 0u) == (s_rl_fm_c == 0u));
        b1 = st3_eq && b1_t0;
        if (b1) {
            rid_ok = rl_fmt1_readid_fresh(p[1], &rg);   /* ゲストの FORMAT の US/HD バイトそのもの(f6 で許可値と一致確認済み) */
            const uint8_t* r = rg.res;
            rid_ic_ok = rg.res_valid && r[0] == (p[1] & 7u) && r[1] == 0u && r[2] == 0u;   /* IC=00・SE/EC/NR無し・HD/USエコー */
            rid_chs = rg.res_valid && r[3] == s_rl_fm_c && r[4] == hd;                   /* R・N はログのみ */
            b2 = rid_ok && rid_ic_ok && rid_chs;
        }
        if (b1 && b2) {
            /* C0: Phase B 中の rl_poll() で 'D'/'d'・世代変化・ラッチ・状態変化を読んだ可能性があるので再評価する。
             * P1066: 停止ラッチ・検証予約なし・連鎖・エポックを追加(エポックは Phase B 中に変わり得るので取り直す) */
            int relay2 = rl_relay_ok();
            uint32_t gen2 = mx68k_floppyu_usb_generation(), d2 = s_rl_d_events;
            int f_chain_now = (s_rl_fmt1_spent_hmask == 0u) || (s_rl_fmt1_chain_ok && !s_rl_fmt1_verify_pending);
            int f_epoch_now = (s_rl_fmt1_spent_hmask == 0u) ||
                              (s_rl_fmt1_chain_set && d2 == s_rl_fmt1_chain_d0 && gen2 == s_rl_fmt1_chain_gen0);
            c0 = relay2 && gen2 == s_rl_wr_gen_begin && d2 == s_rl_wr_d_begin &&
                 !s_rl_wr_latched_off && s_rl_state == RL_WR_COLLECT &&
                 !s_rl_fmt1_halt && !s_rl_fmt1_verify_pending && f_chain_now && f_epoch_now;
        }
    }
    int send = phase_a && b1 && b2 && c0;

    /* 拒否理由(優先順で1つ。ログ・計数用で、送信判断には使わない) */
    int ref = -1;
    if (!send) {
        int rid_proc_fail = strcmp(rg.fail, "none") != 0 && strcmp(rg.fail, "c_without_cc") != 0 &&
                            strcmp(rg.fail, "no_c") != 0;   /* 期限切れ・通信失敗・アイドル非復帰(手順不成立) */
        if (latched)                               ref = FM_REF_LATCHED;
        else if (s_rl_fmt1_halt)                   ref = FM_REF_HALT;          /* P1066 */
        else if (!f5)                              ref = FM_REF_ENV;
        else if (!f_unit)                          ref = FM_REF_UNIT;
        else if (!f_head)                          ref = FM_REF_HEAD;
        else if (!f_param)                         ref = FM_REF_PARAM;
        else if (!f7)                              ref = FM_REF_IDS;
        else if (!f_order)                         ref = FM_REF_ORDER;         /* P1066 */
        else if (!f_repeat)                        ref = FM_REF_SPENT;         /* P1066: 単独では到達しない(f_order が先に落ちる) */
        else if (!f_chain)                         ref = FM_REF_CHAIN;         /* P1066 */
        else if (!f_epoch)                         ref = FM_REF_CHAIN_EPOCH;   /* P1066 */
        else if (s_rl_wr_sends >= send_max)        ref = FM_REF_SEND_MAX;
        else if (!f1 || !f2)                       ref = FM_REF_COLLECT;
        else if (!f3)                              ref = FM_REF_LINK;
        else if (!f8)                              ref = FM_REF_REENTRY;
        else if (!sds_ok)                          ref = FM_REF_SDS_FAIL;
        else if (!(st3 & ST3_RY))                  ref = FM_REF_NO_MEDIA;
        else if (st3 & ST3_WP)                     ref = FM_REF_WP;
        else if (!b1_t0)                           ref = FM_REF_T0;
        else if (!b1)                              ref = FM_REF_ST3_OTHER;
        else if (rid_proc_fail)                    ref = FM_REF_RID_TIMEOUT;
        else if (!rid_ic_ok)                       ref = FM_REF_RID_IC;
        else if (strcmp(rg.fail, "c_without_cc") == 0) ref = FM_REF_RID_C_WITHOUT_CC;
        else if (strcmp(rg.fail, "no_c") == 0)     ref = FM_REF_RID_NO_C;
        else if (!rid_chs)                         ref = FM_REF_RID_CHS;
        else                                       ref = FM_REF_RECHECK;
    }

    if (s_rl_fmt1_pre_lines < FU_AR_LOG_LIMIT) {
        char sh[2 * FU_WR_SDS_RQM_POLL_MAX + 4], rh[2 * 8 + 4], rmh[2 * 16 + 4], os[64], trk[16], eh[8];
        s_rl_fmt1_pre_lines++;
        if (n >= 1u) snprintf(trk, sizeof(trk), "%u,%u", (unsigned)ids[0], (unsigned)hd);   /* ID 表の先頭 C とゲストの HD */
        else         snprintf(trk, sizeof(trk), "-,%u", (unsigned)hd);
        if (expect_h >= 0) snprintf(eh, sizeof(eh), "%d", expect_h);
        else               snprintf(eh, sizeof(eh), "-");
        rl_hex(sh, sizeof(sh), sl.msr_pre, sl.polls, FU_WR_SDS_RQM_POLL_MAX);
        rl_fmt1_rid_str(&rg, rh, sizeof(rh));
        rl_hex(rmh, sizeof(rmh), rg.msr_seq, rg.msr_n, 16);
        rl_fmt1_other_sds_str(us_dev, os, sizeof(os));
        debug_log(P1064_TAG " precheck f=%d phase=%s cmd=0x%02x hd_us=0x%02x us_mx=0x%02x us_dev=%u hd=%u N=%u SC=%u GPL=0x%02x"
                  " D=0x%02x cmd_n=%u n=%u collect_n=%u mtc_begin=0x%04x mar0_valid=%d ocr0=0x%02x ids=%s"
                  " allow_unit=%d allow_ch=%s env_ok=%d f1=%d f2=%d f3=%d f4=%d f5=%d f6=%d f_unit=%d f_head=%d f_param=%d"
                  " f7=%d f8=%d relay=%d gen_begin=%u gen_now=%u d_begin=%u d_now=%u spent=%d sends=%u/%s latched=%d"
                  " track=%s multi=%d hmask=0x%02x spent_mask=0x%02x expect_h=%s f_order=%d f_repeat=%d f_chain=%d f_epoch=%d"
                  " halt=%d halt_why=%s chain_ok=%d chain_d0=%u chain_gen0=%u chain_set=%d vpend=%d"
                  " sds_ret_tx=%d sds_msr_pre=%s sds_polls=%u sds_ret_st3=%d sds_ret_post=%d sds_msr_post=0x%02x sds_ok=%d"
                  " sds_fail=%s st3_dev=0x%02x want=0x%02x st3_masked=0x%02x FT=%d WP=%d RY=%d T0=%d b1_t0=%d b1=%d"
                  " rid_ushd=0x%02x rid_ret_tx=%d rid_polls=%u rid_msr_seq=%s rid_res=%s rid_ret_post=%d rid_msr_post=0x%02x"
                  " rid_c=%u rid_cc=%u rid_us=%lld rid_drain_us=%lld rid_fail=%s rid_ic_ok=%d rid_chs=%d b2=%d c0=%d"
                  " d_after=%u state=%s other_sds=%s turbo_q16=%d decision=%s us=%lld\n",
                  g_mx68k_frame_num, phase_a ? "B" : "A", p[0], p[1], (unsigned)s_rl_us_mx, (unsigned)us_dev, (unsigned)hd,
                  p[2], p[3], p[4], p[5], (unsigned)s_rl_cmd_n, (unsigned)n, (unsigned)s_rl_wrcollect_n,
                  (unsigned)s_rl_wr_mtc_begin, s_rl_wrcollect_mar0_valid, (unsigned)s_rl_wrcollect_ocr0, idh,
                  s_rl_fm_unit, ac, s_rl_fm_env_ok, f1, f2, f3, f4, f5, f6, f_unit, f_head, f_param, f7, f8, relay,
                  (unsigned)s_rl_wr_gen_begin, (unsigned)gen_now, (unsigned)s_rl_wr_d_begin, (unsigned)d_now,
                  s_rl_fmt1_spent, (unsigned)s_rl_wr_sends, rl_wr_max_str(mxs, sizeof(mxs)), latched,
                  trk, s_rl_fm_multi, (unsigned)s_rl_fm_hmask, (unsigned)s_rl_fmt1_spent_hmask, eh, f_order, f_repeat, f_chain,
                  f_epoch, s_rl_fmt1_halt, s_rl_fmt1_halt_why, s_rl_fmt1_chain_ok, (unsigned)s_rl_fmt1_chain_d0,
                  (unsigned)s_rl_fmt1_chain_gen0, s_rl_fmt1_chain_set, s_rl_fmt1_verify_pending,
                  sl.ret_tx, sl.polls ? sh : "-", (unsigned)sl.polls, sl.ret_st3, sl.ret_post, (unsigned)sl.msr_post,
                  sds_ok, sl.fail, (unsigned)st3, (unsigned)want, (unsigned)(st3 & 0xE7u),
                  (st3 & ST3_FT) ? 1 : 0, (st3 & ST3_WP) ? 1 : 0, (st3 & ST3_RY) ? 1 : 0, (st3 & ST3_T0) ? 1 : 0,
                  b1_t0, b1, (unsigned)p[1], rg.ret_tx, (unsigned)rg.polls, rg.msr_n ? rmh : "-", rh, rg.ret_post,
                  (unsigned)rg.msr_post, (unsigned)rg.c, (unsigned)rg.cc, (long long)rg.us, (long long)rg.drain_us,
                  rg.fail, rid_ic_ok, rid_chs, b2, c0, (unsigned)s_rl_d_events, RL_STATE_NAME[s_rl_state], os,
                  mx68k_turbo_audio_ratio_q16_get(), send ? "sent" : FM_REF_NAME[ref], (long long)(rl_now_us() - t0));
    }
    if (!send) {
        if (phase_a) s_rl_fmt1_stray_pending = 1;   /* Phase B に入ってから拒否: 吸収しきれなかった'C'を数える(F-8) */
        rl_fmt1_refuse(ref);
        return;
    }

    /* 送信。送信関数を呼ぶ前に spent・面マスクを立て、連鎖を0にする(送信失敗・ログ予算切れ・後続の例外経路に関わらず
     * 同じ面の2回目は起きず、次の面は検証 ok まで送れない)。
     * P1066: Phase 0 の検証(at_seek=0)が開いた迷子'C'の計数窓は、送信を跨いで WRITE ID の正規の遅着'C'を
     * 迷子と数えないよう、ここで閉じる */
    s_rl_fmt1_stray_pending = 0;
    s_rl_fmt1_spent = 1;
    s_rl_fmt1_spent_hmask |= (uint8_t)(1u << hd);
    s_rl_fmt1_chain_ok = 0;
    s_rl_fmt1_sent_h = hd;
    if (!s_rl_fmt1_chain_set) {   /* 最初の送信時(=1トラック目の収集開始時)の値をエポックの基準にする */
        s_rl_fmt1_chain_d0 = s_rl_wr_d_begin;
        s_rl_fmt1_chain_gen0 = s_rl_wr_gen_begin;
        s_rl_fmt1_chain_set = 1;
    }
    s_rl_fmt1_inflight = 1;
    s_rl_wr_wp_at_send = 0;   /* ログ整合用(fmt1 のリザルト判定は rl_fmt1_result_classify が行う) */
    int r;
#ifdef MX68K_FLOPPYU_USB_SELFTEST
    if (s_st_fmt1_force_ret != 0) { r = s_st_fmt1_force_ret; s_st_fmt1_force_ret = 0; }
    else
#endif
    r = rl_send_write_cmd(n);   /* B0 <n> B1 <n>>8> 83 4D 83 <HD/US> 83 N 83 SC 83 GPL 83 D(16B) */
    if (r != -1) s_rl_last_io_frame[s_rl_us_mx & 3u] = g_mx68k_frame_num;   /* P1092: 表示用 */
    if (s_rl_fmt1_lines < FU_AR_LOG_LIMIT) {
        s_rl_fmt1_lines++;
        debug_log(P1064_TAG " send f=%d ret=%d n=%u sends_before=%u spent=%d inflight=%d h=%u spent_mask=0x%02x\n",
                  g_mx68k_frame_num, r, (unsigned)n, (unsigned)s_rl_wr_sends, s_rl_fmt1_spent, s_rl_fmt1_inflight,
                  (unsigned)hd, (unsigned)s_rl_fmt1_spent_hmask);
    }
    if (r == -1) {   /* 未接続: 何も届いていない。spent は1のまま */
        s_rl_fmt1_inflight = 0;
        rl_synth_err(0x02u, 0);
        rl_raise_int();
        return;
    }
    if (r < 0) {     /* -2/-3: 送信途中で失敗し得る(装置状態不明 → ラッチ) */
        s_rl_wr_sends++;
        rl_wr_fail_unknown("unknown_p1");
        return;
    }
    s_rl_wr_sends++;
    s_rl_fmt1_send_seek_n = s_rl_fmt1_seek_n;
    rl_wr_enter_wait_w(n);

    /* 送信直後の同期'W'待ち(Fable5 C-2)。'W' は既存の 'W' ハンドラ(4条件ゲート)がそのまま rl_wr_send_data() を呼ぶ——
     * 送出判断をここで新たに作らない。上限内に来なければ従来の非同期経路(フレーム毎の rl_tick→rl_poll)へ戻る */
    int64_t ts = rl_now_us(), wdl = ts + (int64_t)FU_FMT1_W_WAIT_MS * 1000;
    while (s_rl_state == RL_WR_WAIT_W && !s_rl_wr_data_sent && rl_now_us() < wdl) {
        rl_poll();
        if (s_rl_state != RL_WR_WAIT_W || s_rl_wr_data_sent) break;
        rl_sync_sleep();
    }
    int64_t te = rl_now_us();
    const char* ws = s_rl_wr_data_sent ? "hit" : (s_rl_state != RL_WR_WAIT_W ? "state_changed" : "timeout");
    long long w_ev_us = -1;
    for (uint32_t i = 0; i < s_rl_wr_ev_n; i++) if (s_rl_wr_ev_c[i] == 'W') { w_ev_us = (long long)s_rl_wr_ev_us[i]; break; }
    long long w_to_data_us = (s_rl_wr_data_sent && w_ev_us >= 0) ? (long long)(te - s_rl_wr_send_t_us) - w_ev_us : -1;
    if (s_rl_fmt1_lines < FU_AR_LOG_LIMIT) {
        char ev[FU_WR_EV_MAX * 24 + 4];
        s_rl_fmt1_lines++;
        rl_wr_events_str(ev, sizeof(ev));
        debug_log(P1064_TAG " wsync f=%d w_sync=%s w_wait_us=%lld w_ev_us=%lld w_to_data_us=%lld data_sent=%d state=%s events=%s\n",
                  g_mx68k_frame_num, ws, (long long)(te - ts), w_ev_us, w_to_data_us, s_rl_wr_data_sent,
                  RL_STATE_NAME[s_rl_state], ev);
    }
}

/* ===========================================================================
 *  P1067: normal モードの FORMAT(設定「Allow FORMAT on FloppyU disks」)
 * =========================================================================== */

/* 送信成功後の検証 READ ID(書込みなし)。判定内容は rl_fmt1_verify と同じ(READ ID×8 で C/H 一致・R=1..8・N=3、
 * c_deficit_final==0 必須)を s_nmf_* の状態で行う。起動点: 同じユニットへの SEEK/RECAL 中継直前・次の FORMAT の Phase 0・
 * normal WRITE DATA の送信前(c10)。有効性は検証対象ユニットの ue と世代が送信時(この FORMAT の収集開始時の採取値)から
 * 不変で、判別保留中でないこと(ユニット別。後続の収集開始で s_rl_wr_ue_begin が取り直されても、ここは送信時の値と比べる)。
 * 戻り値: 1=検証 ok かつ検証後も中継可能 / 0=それ以外(停止済み)。*post_relay に検証後の rl_relay_ok() */
static int rl_nmf_verify(int trig, int* post_relay)
{
    const uint8_t* p = s_rl_cmd;
    uint8_t us = s_nmf_v_us & 3u;
    int relay = rl_relay_ok();
    uint32_t gen = mx68k_floppyu_usb_generation();
    int unit_ok = s_nm_ue[us] == s_nmf_v_ue && s_nm_st == NM_ST_I;
    const char* skip = NULL;
    const char* skip_why = NULL;
    if (!relay)                   { skip = "link";         skip_why = "verify_skipped_link"; }
    else if (gen != s_nmf_v_gen)  { skip = "gen";          skip_why = "verify_skipped_gen"; }
    else if (!unit_ok)            { skip = "unit_changed"; skip_why = "verify_skipped_unit_changed"; }
    else if (s_rl_in_poll)        { skip = "reentry";      skip_why = "verify_skipped_reentry"; }
    s_nmf_verify_pending = 0;
    char cond[384];
    snprintf(cond, sizeof(cond), "unit=%u relay=%d gen=%u/%u ue=%u/%u st=%s in_poll=%d trigger=%s guest_cmd=0x%02x"
             " guest_us=0x%02x v_c=%u v_h=%u v_k=%u run_active=%d run_us=%u run_pos=%u",
             (unsigned)us, relay, (unsigned)gen, (unsigned)s_nmf_v_gen, (unsigned)s_nm_ue[us], (unsigned)s_nmf_v_ue,
             NM_ST_NAME[s_nm_st], s_rl_in_poll, NMF_VSRC_NAME[trig], (unsigned)p[0], (unsigned)p[1], (unsigned)s_nmf_v_c,
             (unsigned)s_nmf_v_h, (unsigned)s_nmf_v_k, s_nmf_run_active, (unsigned)s_nmf_run_us, (unsigned)s_nmf_run_pos);
    if (skip) {
        s_nmf_verify_skipped++;
        rl_nmf_halt(skip_why);   /* 検証できなかったトラックの次へ書込みを進めない */
        int post = rl_relay_ok();
        if (!post) s_nmf_relay_lost_n++;
        if (post_relay) *post_relay = post;
        if (s_nmf_v_lines < FU_AR_LOG_LIMIT) {
            s_nmf_v_lines++;
            debug_log(P1067_TAG " verify f=%d verify=skipped_%s %s post_relay=%d halt_after=%d chain_ok_after=%d"
                      " runs=%u ok=%u bad=%u skipped=%u\n",
                      g_mx68k_frame_num, skip, cond, post, s_nmf_halt, s_nmf_chain_ok, (unsigned)s_nmf_verify_runs,
                      (unsigned)s_nmf_verify_ok, (unsigned)s_nmf_verify_bad, (unsigned)s_nmf_verify_skipped);
        }
        return 0;
    }
    s_nmf_verify_runs++;
    s_nmf_verify_at[trig]++;
    int64_t t0 = rl_now_us();
    uint8_t ushd = (uint8_t)(us | ((unsigned)s_nmf_v_h << 2));
    char all[FU_FMT1_VERIFY_N * 48u + 8];
    size_t o = 0;
    int good = 1;
    uint32_t done = 0, c_total = 0, cc_total = 0;
    uint8_t rmask = 0;
    all[0] = '\0';
    for (uint32_t i = 0; i < FU_FMT1_VERIFY_N; i++) {
        rl_rid_fresh_log_t lg;
        (void)rl_fmt1_readid_fresh(ushd, &lg);
        done++;
        c_total += lg.c;
        cc_total += lg.cc;
        const uint8_t* r = lg.res;
        int ok_i = lg.res_valid && r[0] == (ushd & 7u) && r[1] == 0u && r[2] == 0u && r[3] == s_nmf_v_c &&
                   r[4] == s_nmf_v_h && r[6] == 3u && r[5] >= 1u && r[5] <= 8u;
        if (ok_i) rmask |= (uint8_t)(1u << (r[5] - 1u));
        else      good = 0;
        char rh[2 * 8 + 4];
        rl_fmt1_rid_str(&lg, rh, sizeof(rh));
        if (o + 48u < sizeof(all))
            o += (size_t)snprintf(all + o, sizeof(all) - o, "%s%s/%s/c%u,%u", i ? "," : "", rh, lg.fail,
                                  (unsigned)lg.c, (unsigned)lg.cc);
        if (lg.ret_tx < 0) break;   /* 通信不能(以後の問合せは意味を持たない) */
    }
    if (done < FU_FMT1_VERIFY_N) good = 0;
    /* 検証後の吸収窓と再検査(rl_fmt1_verify と同じ手順。P1065 C-1/C-2) */
    uint32_t c_deficit = done > c_total ? done - c_total : 0u;
    s_rl_sync_active = 1;
    s_rl_sync_c = 0;
    s_rl_sync_cc = 0;
    rl_poll();
    if (c_deficit > 0u) {
        int64_t dend = rl_now_us() + (int64_t)FU_FMT_DRAIN_MS * 1000;
        while (s_rl_sync_c < c_deficit && rl_now_us() < dend) {
            rl_sync_sleep();
            rl_poll();
        }
    }
    uint32_t late_c = s_rl_sync_c, late_cc = s_rl_sync_cc;
    s_rl_sync_active = 0;
    uint32_t c_deficit_final = c_deficit > late_c ? c_deficit - late_c : 0u;
    if (c_deficit_final > 0u) s_nmf_c_deficit_n++;
    uint32_t gen_after = mx68k_floppyu_usb_generation();
    int rx_ovf = mx68k_floppyu_usb_port2_rx_overflowed() ? 1 : 0;
    char bad_after[64];
    size_t bo = 0;
    bad_after[0] = '\0';
    if (s_nm_ue[us] != s_nmf_v_ue) bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%sue", bo ? "+" : "");
    if (s_nm_st != NM_ST_I)        bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%shold", bo ? "+" : "");
    if (gen_after != s_nmf_v_gen)  bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%sgen", bo ? "+" : "");
    if (rx_ovf)                    bo += (size_t)snprintf(bad_after + bo, sizeof(bad_after) - bo, "%srx_overflow", bo ? "+" : "");
    if (bo) good = 0;
    else    snprintf(bad_after, sizeof(bad_after), "-");
    uint32_t cover = 0;
    for (uint32_t b = 0; b < 8u; b++) if ((rmask >> b) & 1u) cover++;
    if (good) s_nmf_verify_ok++; else s_nmf_verify_bad++;
    int pass = good && c_deficit_final == 0u;
    int completed = 0;
    if (pass) {
        s_nmf_chain_ok = 1;
        s_nmf_tracks_ok++;
        if (s_nmf_run_active && s_nmf_run_us == us && s_nmf_v_k == FU_NMF_LAST_POS) {   /* C76H1 の検証 ok で走行完走 */
            s_nmf_run_active = 0;
            s_nmf_runs_completed++;
            completed = 1;
            /* P1070(V-2): 完走記録。ue/gen は送信時の値(上の good 判定で現在値と不変を確認済み) */
            s_nmf_ld_valid = 1;
            s_nmf_ld_us = us;
            s_nmf_ld_ue = s_nmf_v_ue;
            s_nmf_ld_gen = s_nmf_v_gen;
            s_nmf_ld_f = g_mx68k_frame_num;
            s_nmf_ld_runs = s_nmf_runs_completed;
        }
    } else if (good) {
        rl_nmf_halt("verify_c_deficit");
    } else {
        rl_nmf_halt("verify_bad");
    }
    int post = rl_relay_ok();
    if (!post) { s_nmf_relay_lost_n++; rl_nmf_halt("verify_relay_lost"); }
    if (post_relay) *post_relay = post;
    if (s_nmf_v_lines < FU_AR_LOG_LIMIT) {
        s_nmf_v_lines++;
        debug_log(P1067_TAG " verify f=%d verify=%s %s ushd=0x%02x n=%u rids=%s r_mask=0x%02x r_cover=%u/8"
                  " c_total=%u cc_total=%u c_deficit=%u late_c_absorbed=%u late_cc_absorbed=%u c_deficit_final=%u"
                  " gen_after=%u rx_overflow=%d bad_after=%s post_relay=%d halt_after=%d chain_ok_after=%d tracks_ok=%u"
                  " runs=%u ok=%u bad=%u skipped=%u us=%lld\n",
                  g_mx68k_frame_num, good ? "ok" : "bad", cond, (unsigned)ushd, (unsigned)done, all, (unsigned)rmask,
                  (unsigned)cover, (unsigned)c_total, (unsigned)cc_total, (unsigned)c_deficit, (unsigned)late_c,
                  (unsigned)late_cc, (unsigned)c_deficit_final, (unsigned)gen_after, rx_ovf, bad_after, post, s_nmf_halt,
                  s_nmf_chain_ok, (unsigned)s_nmf_tracks_ok, (unsigned)s_nmf_verify_runs, (unsigned)s_nmf_verify_ok,
                  (unsigned)s_nmf_verify_bad, (unsigned)s_nmf_verify_skipped, (long long)(rl_now_us() - t0));
        if (completed)
            debug_log(P1067_TAG " run f=%d end=complete unit=%u tracks=%u runs_started=%u runs_completed=%u\n",
                      g_mx68k_frame_num, (unsigned)us, (unsigned)(FU_NMF_LAST_POS + 1u), (unsigned)s_nmf_runs_started,
                      (unsigned)s_nmf_runs_completed);
    }
    return pass && post;
}

/* 拒否: NW を合成する(CHRN 非エコー、P1064 F-6)。走行中の同ユニットの拒否は、ディスクが既に一部初期化された状態での
 * 中断なので停止(partial)へ倒す。走行外・別ユニットの拒否は送信0・ラッチなし(ディスク無変更) */
static void rl_nmf_refuse(int ref, int in_run_same)
{
    s_nmf_ref[ref]++;
    if (in_run_same) rl_nmf_halt("partial_refused");
    rl_synth_err(0x02u, 0);
    rl_raise_int();
}

/* P1070: C77H0 判定の生値(Phase 0 拒否時は Phase A の値が未計算なので -1) */
typedef struct {
    int phase0, ld, cand, ld_match, run_act, a1, relay, unch, a2, a3, a4, a5, a6, a7, a8, a10, pcn_valid, fence, latched;
    uint8_t us, c, hd, pcn;
    uint32_t n, gen_now;
    const uint8_t* res;   /* 吸収時の合成リザルト(不成立なら NULL) */
    const char* decision;
} rl_nmf_c77_log_t;

static void rl_nmf_c77_log(const rl_nmf_c77_log_t* g)
{
    if (s_nmf_c77_lines >= FU_NMF_C77_LOG_LIMIT) return;
    s_nmf_c77_lines++;
    char rh[3 * FU_RESULT_LEN + 4];
    if (g->res) snprintf(rh, sizeof(rh), "%02X_%02X_%02X_%02X_%02X_%02X_%02X", g->res[0], g->res[1], g->res[2], g->res[3],
                         g->res[4], g->res[5], g->res[6]);
    else        snprintf(rh, sizeof(rh), "-");
    char pc[8];
    if (g->pcn_valid > 0) snprintf(pc, sizeof(pc), "%u", (unsigned)g->pcn);
    else                  snprintf(pc, sizeof(pc), "-");
    debug_log(P1070_TAG " c77 f=%d phase=%s cmd=0x%02x hd_us=0x%02x unit=%u track=%u,%u n=%u"
              " ld=%d ld_us=%u ld_ue=%u ld_gen=%u ld_f=%d ld_runs=%u ue_now=%u gen_now=%u gen_begin=%u ue_begin=%u st=%s"
              " runs_completed=%u run_active=%d cand=%d ld_match=%d"
              " a1=%d relay=%d unch=%d a2=%d a3=%d a4=%d fence_open=%d a5=%d a6=%d a7=%d pcn=%s pcn_valid=%d a8=%d a10=%d"
              " halt=%d latched=%d res=%s c77_seen=%u c77_cand=%u absorbed_c77=%u decision=%s\n",
              g_mx68k_frame_num, g->phase0 ? "0" : "A", s_rl_cmd[0], s_rl_cmd[1], (unsigned)g->us, (unsigned)g->c,
              (unsigned)g->hd, (unsigned)g->n,
              g->ld, (unsigned)s_nmf_ld_us, (unsigned)s_nmf_ld_ue, (unsigned)s_nmf_ld_gen, s_nmf_ld_f, (unsigned)s_nmf_ld_runs,
              (unsigned)s_nm_ue[g->us & 3u], (unsigned)g->gen_now, (unsigned)s_rl_wr_gen_begin,
              (unsigned)s_rl_wr_ue_begin[g->us & 3u], NM_ST_NAME[s_nm_st],
              (unsigned)s_nmf_runs_completed, g->run_act, g->cand, g->ld_match,
              g->a1, g->relay, g->unch, g->a2, g->a3, g->a4, g->fence, g->a5, g->a6, g->a7, pc, g->pcn_valid, g->a8, g->a10,
              s_nmf_halt, g->latched, rh, (unsigned)s_nmf_c77_seen, (unsigned)s_nmf_c77_cand, (unsigned)s_nmf_absorbed,
              g->decision);
}

/* 収集完了時(rl_collect_done 経由、Core DMA ch0 の最終バイト書込みコールバック内で同期実行)。
 * Phase 0(検証予約・停止・連鎖)→Phase A(装置へ触れない検査)→Phase B(非キャッシュ SDS・同期 READ ID)→C0(再検査)→送信→
 * 同期'W'待ち。判定に使った生値は1 FORMAT につき1行の precheck 行へ全て出す(送信の有無に関わらず)。
 * ヘッドを動かすコマンドは挿入しない(rl_wr_sync_seek は呼ばない)。位置はゲスト自身の SEEK の結果(PCN 記録)で照合する */
static void rl_nmf_try_send(void)
{
    const uint8_t* p = s_rl_cmd;   /* [0]cmd [1]HD/US(装置座標) [2]N [3]SC [4]GPL [5]D */
    const uint8_t* ids = s_rl_wrcollect_buf;
    uint32_t n = s_rl_wrcollect_pos;
    int64_t t0 = rl_now_us();
    uint8_t us = p[1] & 3u;
    uint8_t hd = (uint8_t)((p[1] >> 2) & 1u);
    uint8_t c = n >= 1u ? ids[0] : 0xFFu;
    uint32_t k = 2u * (uint32_t)c + hd;   /* 走行内の位置(C0H0=0 … C76H1=153) */
    char idh[2 * 32 + 8];
    rl_hex(idh, sizeof(idh), ids, n, 32);
    /* P1070: 入口での C77H0 の計数(Phase 0 拒否も含む分母) */
    int c77_in = (n >= 4u && c == FU_NMF_ABSORB_CYL && hd == 0u);
    if (c77_in) s_nmf_c77_seen++;

    /* Phase 0: 検証予約があれば先に検証する(ユニットを問わない)。停止・連鎖不成立なら装置へ触れずに拒否 */
    int v0 = -1;
    if (s_nmf_verify_pending) v0 = rl_nmf_verify(NMF_VSRC_FMT, NULL);
    /* P1070 X5: 完走記録はこの FORMAT で消費する(検証の後に読む: この検証で完走した記録も取りこぼさない。吸収の成否によらない) */
    int ld = s_nmf_ld_valid;
    s_nmf_ld_valid = 0;
    int in_run_same = s_nmf_run_active && us == s_nmf_run_us;
    int z = -1;
    if (s_nmf_halt)                        z = NMF_REF_HALT;
    else if (in_run_same && !s_nmf_chain_ok) z = NMF_REF_CHAIN;
    if (z >= 0) {
        if (ld) s_nmf_ld_exp_fmt++;
        if (c77_in) {
            rl_nmf_c77_log_t g = { 1, ld, 0, 0, s_nmf_run_active, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                   s_nmf_pcn_valid[us], -1, s_rl_wr_latched_off, us, c, hd, s_nmf_pcn[us], n,
                                   mx68k_floppyu_usb_generation(), NULL, "not_absorbed_phase0" };
            rl_nmf_c77_log(&g);
        }
        if (s_nmf_pre_lines < FU_AR_LOG_LIMIT) {
            s_nmf_pre_lines++;
            debug_log(P1067_TAG " precheck f=%d phase=0 cmd=0x%02x hd_us=0x%02x us_mx=0x%02x unit=%u hd=%u n=%u ids=%s"
                      " track=%u,%u k=%d v0=%d halt=%d halt_why=%s partial=%d chain_ok=%d run_active=%d run_us=%u run_pos=%u"
                      " latched=%d decision=%s\n",
                      g_mx68k_frame_num, p[0], p[1], (unsigned)s_rl_us_mx, (unsigned)us, (unsigned)hd, (unsigned)n, idh,
                      (unsigned)c, (unsigned)hd, (int)k, v0, s_nmf_halt, s_nmf_halt_why, s_nmf_partial, s_nmf_chain_ok,
                      s_nmf_run_active, (unsigned)s_nmf_run_us, (unsigned)s_nmf_run_pos, s_rl_wr_latched_off, NMF_REF_NAME[z]);
        }
        rl_nmf_refuse(z, in_run_same);
        return;
    }

    /* Phase A: 装置へ一切触れない検査 */
    int a1 = (n == s_rl_wrcollect_n) && (n == (uint32_t)p[3] * 4u) && (n == (uint32_t)s_rl_wr_mtc_begin) &&
             (s_rl_wrcollect_mar0_valid == 1) && ((s_rl_wrcollect_ocr0 & 0x80u) == 0u);
    int relay = rl_relay_ok();
    uint32_t gen_now = mx68k_floppyu_usb_generation();
    int unch = rl_nm_unit_unchanged(us);   /* 右辺は R1 で採取したこの FORMAT の収集開始時の ue */
    int a2 = relay && gen_now == s_rl_wr_gen_begin && unch;
    int latched = s_rl_wr_latched_off;
    int a3 = !latched && !s_nmf_halt;
    int a4 = s_nm_unit_state == 0 || (s_nm_unit_state == 1 && (int)us == s_nm_unit);
    int fence = rl_nm_fence_open(us);
    int a5 = fence;
    int a6 = (s_rl_cmd_n == 6u) && p[0] == 0x4Du && p[2] == 3u && p[3] == 8u && p[4] == 0x74u && p[5] == 0xE5u;
    int a7 = (n == 32u);
    for (uint32_t i = 0; a7 && i < 8u; i++)
        if (ids[i * 4u] != c || ids[i * 4u + 1u] != hd || ids[i * 4u + 2u] != (uint8_t)(i + 1u) || ids[i * 4u + 3u] != 3u) a7 = 0;
    int a7c = a7 && c <= FU_NMF_MAX_CYL;
    int pcn_v = s_nmf_pcn_valid[us] && s_nmf_pcn_ue[us] == s_nm_ue[us] && s_nmf_pcn_gen[us] == gen_now;
    int a8 = pcn_v && s_nmf_pcn[us] == c;
    int run_act = s_nmf_run_active;
    int other = run_act && us != s_nmf_run_us;
    uint32_t run_n = (s_nmf_ep_valid[us] && s_nmf_ep_ue[us] == s_nm_ue[us]) ? s_nmf_run_n[us] : 0u;
    int start_ok = !run_act && k == 0u && run_n < FU_NMF_RUNS_PER_EPOCH;
    int order_ok = run_act && !other && k == s_nmf_run_pos;
    int a9 = start_ok || order_ok;
    int a10 = !s_rl_in_poll;
    int phase_a = a1 && a2 && a3 && a4 && a5 && a6 && a7c && a8 && a9 && a10;

    /* P1070(V-4): 直前に完走した走行の直後の C77H0 を、装置へ一切送らず正常終了で返す(XEiJ/XM6 と同じ扱い)。
     * SDS・READ ID・WRITE ID のいずれも送らない。走行状態・連鎖・送信記録・検証予約・PCN 記録・照合表は変えない
     * (rl_ar_idx(77,…) は照合表 C0..C76 の範囲外なので呼ばない)。rl_synth_err を呼ばないので EJGUARD 用記録も立たない */
    int cand = a7 && c == FU_NMF_ABSORB_CYL && hd == 0u;
    if (cand) {
        s_nmf_c77_cand++;
        int ld_match = ld && us == s_nmf_ld_us && s_nm_ue[us] == s_nmf_ld_ue && gen_now == s_nmf_ld_gen;
        int absorb = ld_match && !run_act && a1 && a2 && a3 && a4 && a5 && a6 && a8 && a10;
        uint8_t res[FU_RESULT_LEN] = { 0 };   /* rl_fmt_dryrun_complete と同形(IC=00、C=H=R=0、N=コマンドの N) */
        res[0] = p955_st_dev_to_mx((uint8_t)(p[1] & 0x07u));
        res[6] = p[2];
        if (absorb) s_nmf_absorbed++;
        rl_nmf_c77_log_t g = { 0, ld, cand, ld_match, run_act, a1, relay, unch, a2, a3, a4, a5, a6, a7, a8, a10,
                               s_nmf_pcn_valid[us], fence, latched, us, c, hd, s_nmf_pcn[us], n, gen_now,
                               absorb ? res : NULL, absorb ? "absorbed" : "not_absorbed" };
        rl_nmf_c77_log(&g);
        if (absorb) {
            rl_set_result(res, FU_RESULT_LEN);
            rl_raise_int();
            return;
        }
    }
    if (ld) s_nmf_ld_exp_fmt++;   /* X5 で消費したが吸収されなかった */

    /* Phase B: 装置への読取り専用の問合せ(Phase A 全成立時のみ) */
    rl_wr_sds_log_t sl;
    memset(&sl, 0, sizeof(sl));
    sl.ret_tx = -9; sl.ret_st3 = -9; sl.ret_post = -9; sl.fail = "skipped";
    rl_rid_fresh_log_t rg;
    memset(&rg, 0, sizeof(rg));
    rg.ret_tx = -9; rg.ret_post = -9; rg.fail = "skipped";
    uint8_t st3 = 0;
    uint8_t want = (uint8_t)(0x20u | (p[1] & 7u));   /* FT=0・WP=0・RY=1・US/HD エコー一致 */
    int sds_ok = 0, st3_eq = 0, t0ok = 0, b1 = 0, rid_ok = 0, rid_ic_ok = 0, rid_chs = 0, rid_n = 0, b2 = 0, c0 = 0;
    int ry0_close = 0;
    if (phase_a) {
        sds_ok = rl_wr_sds_fresh(&st3, &sl);
        st3_eq = sds_ok && ((st3 & 0xE7u) == want);
        /* T0 整合: C=0 では T0=1、C≥1 では T0=0(位置の特定にはならないが矛盾を検出する) */
        t0ok = sds_ok && (((st3 & ST3_T0) != 0u) == (c == 0u));
        b1 = st3_eq && t0ok;
        if (sds_ok && !(st3 & ST3_RY)) {   /* WRITE DATA 側(rl_wr_try_send)と同じ能動閉鎖。経路間で非対称を作らない */
            uint8_t was = rl_nm_open_mask();
            rl_nm_fence_close(us, NM_CL_RY0, 1, 1, was, NULL);
            ry0_close = 1;
        }
        if (b1) {
            rid_ok = rl_fmt1_readid_fresh(p[1], &rg);   /* ゲストの FORMAT の US/HD バイトそのもの */
            const uint8_t* r = rg.res;
            rid_ic_ok = rg.res_valid && r[0] == (p[1] & 7u) && r[1] == 0u && r[2] == 0u;
            rid_chs = rg.res_valid && r[3] == c && r[4] == hd;   /* C≥1 で唯一の物理位置の証拠 */
            rid_n = rg.res_valid && r[6] == 3u;                  /* 既に 1024B でフォーマット済み(他物理形式・ブランクは拒否) */
            b2 = rid_ok && rid_ic_ok && rid_chs && rid_n;
        }
        if (b1 && b2) {
            /* C0: Phase B 中の rl_poll() で 'D'/'d'・世代変化・ラッチ・状態変化・PCN 無効化を読んだ可能性があるので再評価する */
            int relay2 = rl_relay_ok();
            uint32_t gen2 = mx68k_floppyu_usb_generation();
            int pcn2 = s_nmf_pcn_valid[us] && s_nmf_pcn_ue[us] == s_nm_ue[us] && s_nmf_pcn_gen[us] == gen2 && s_nmf_pcn[us] == c;
            int run2 = !s_nmf_run_active ? (k == 0u) : (us == s_nmf_run_us && k == s_nmf_run_pos && s_nmf_chain_ok);
            c0 = relay2 && gen2 == s_rl_wr_gen_begin && rl_nm_unit_unchanged(us) && rl_nm_fence_open(us) &&
                 !s_rl_wr_latched_off && !s_nmf_halt && !s_nmf_verify_pending && run2 && s_rl_state == RL_WR_COLLECT && pcn2;
        }
    }
    int send = phase_a && b1 && b2 && c0;

    /* 拒否理由(優先順で1つ。ログ・計数用で、送信判断には使わない) */
    int ref = -1;
    if (!send) {
        int rid_proc_fail = strcmp(rg.fail, "none") != 0 && strcmp(rg.fail, "c_without_cc") != 0 &&
                            strcmp(rg.fail, "no_c") != 0;
        if (latched || s_rl_wr_latched_off)         ref = NMF_REF_LATCHED;
        else if (s_nmf_halt)                        ref = NMF_REF_HALT;
        else if (!a4)                               ref = NMF_REF_UNIT;
        else if (!a5)                               ref = NMF_REF_FENCE;
        else if (!a6)                               ref = NMF_REF_PARAM;
        else if (!a7)                               ref = NMF_REF_IDS;
        else if (!a7c)                              ref = NMF_REF_CYL;
        else if (other)                             ref = NMF_REF_RUN_OTHER_UNIT;
        else if (!run_act && k != 0u)               ref = NMF_REF_RUN_START;
        else if (!run_act && !start_ok)             ref = NMF_REF_RUN_MAX;
        else if (run_act && !order_ok)              ref = NMF_REF_ORDER;
        else if (!a1)                               ref = NMF_REF_COLLECT;
        else if (!a2)                               ref = NMF_REF_LINK;
        else if (!pcn_v)                            ref = NMF_REF_NO_PCN;
        else if (!a8)                               ref = NMF_REF_PCN;
        else if (!a10)                              ref = NMF_REF_REENTRY;
        else if (!sds_ok)                           ref = NMF_REF_SDS_FAIL;
        else if (!(st3 & ST3_RY))                   ref = NMF_REF_NO_MEDIA;
        else if (st3 & ST3_WP)                      ref = NMF_REF_WP;
        else if (!t0ok)                             ref = NMF_REF_T0;
        else if (!b1)                               ref = NMF_REF_ST3_OTHER;
        else if (rid_proc_fail)                     ref = NMF_REF_RID_TIMEOUT;
        else if (!rid_ic_ok)                        ref = NMF_REF_RID_IC;
        else if (strcmp(rg.fail, "c_without_cc") == 0) ref = NMF_REF_RID_C_WITHOUT_CC;
        else if (strcmp(rg.fail, "no_c") == 0)      ref = NMF_REF_RID_NO_C;
        else if (!rid_chs)                          ref = NMF_REF_RID_CHS;
        else if (!rid_n)                            ref = NMF_REF_RID_N;
        else                                        ref = NMF_REF_RECHECK;
    }

    if (s_nmf_pre_lines < FU_AR_LOG_LIMIT) {
        char sh[2 * FU_WR_SDS_RQM_POLL_MAX + 4], rh[2 * 8 + 4], rmh[2 * 16 + 4], ph[8];
        s_nmf_pre_lines++;
        rl_hex(sh, sizeof(sh), sl.msr_pre, sl.polls, FU_WR_SDS_RQM_POLL_MAX);
        rl_fmt1_rid_str(&rg, rh, sizeof(rh));
        rl_hex(rmh, sizeof(rmh), rg.msr_seq, rg.msr_n, 16);
        if (s_nmf_pcn_valid[us]) snprintf(ph, sizeof(ph), "%u", (unsigned)s_nmf_pcn[us]);
        else                     snprintf(ph, sizeof(ph), "-");
        debug_log(P1067_TAG " precheck f=%d phase=%s cmd=0x%02x hd_us=0x%02x us_mx=0x%02x unit=%u hd=%u N=%u SC=%u GPL=0x%02x"
                  " D=0x%02x cmd_n=%u n=%u collect_n=%u mtc_begin=0x%04x mar0_valid=%d ocr0=0x%02x ids=%s track=%u,%u k=%u"
                  " a1=%d relay=%d gen_begin=%u gen_now=%u ue_begin=%u ue_now=%u st=%s unch=%d a2=%d latched=%d halt=%d a3=%d"
                  " allow_unit_state=%d allow_unit=%d a4=%d fo_valid=%d fo_ue=%u fo_gen=%u fence_open=%d a5=%d a6=%d a7=%d a7c=%d"
                  " pcn=%s pcn_valid=%d pcn_f=%d pcn_ue=%u pcn_gen=%u st0_seek=0x%02x pcn_ok=%d a8=%d"
                  " run_active=%d run_us=%u run_pos=%u run_n=%u run_ue=%u chain_ok=%d start_ok=%d order_ok=%d a9=%d a10=%d"
                  " sds_ret_tx=%d sds_msr_pre=%s sds_polls=%u sds_ok=%d sds_fail=%s st3_dev=0x%02x want=0x%02x st3_masked=0x%02x"
                  " FT=%d WP=%d RY=%d T0=%d t0ok=%d b1=%d ry0_close=%d"
                  " rid_ushd=0x%02x rid_ret_tx=%d rid_polls=%u rid_msr_seq=%s rid_res=%s rid_c=%u rid_cc=%u rid_us=%lld"
                  " rid_fail=%s rid_ic_ok=%d rid_chs=%d rid_n=%d b2=%d c0=%d pos_ok=%d state=%s decision=%s us=%lld\n",
                  g_mx68k_frame_num, phase_a ? "B" : "A", p[0], p[1], (unsigned)s_rl_us_mx, (unsigned)us, (unsigned)hd,
                  p[2], p[3], p[4], p[5], (unsigned)s_rl_cmd_n, (unsigned)n, (unsigned)s_rl_wrcollect_n,
                  (unsigned)s_rl_wr_mtc_begin, s_rl_wrcollect_mar0_valid, (unsigned)s_rl_wrcollect_ocr0, idh,
                  (unsigned)c, (unsigned)hd, (unsigned)k,
                  a1, relay, (unsigned)s_rl_wr_gen_begin, (unsigned)gen_now, (unsigned)s_rl_wr_ue_begin[us],
                  (unsigned)s_nm_ue[us], NM_ST_NAME[s_nm_st], unch, a2, latched, s_nmf_halt, a3,
                  s_nm_unit_state, s_nm_unit, a4, s_nm_fo_valid[us], (unsigned)s_nm_fo_ue[us], (unsigned)s_nm_fo_gen[us],
                  fence, a5, a6, a7, a7c,
                  ph, s_nmf_pcn_valid[us], s_nmf_pcn_f[us], (unsigned)s_nmf_pcn_ue[us], (unsigned)s_nmf_pcn_gen[us],
                  (unsigned)s_nmf_pcn_st0[us], pcn_v, a8,
                  run_act, (unsigned)s_nmf_run_us, (unsigned)s_nmf_run_pos, (unsigned)run_n, (unsigned)s_nmf_run_ue,
                  s_nmf_chain_ok, start_ok, order_ok, a9, a10,
                  sl.ret_tx, sl.polls ? sh : "-", (unsigned)sl.polls, sds_ok, sl.fail, (unsigned)st3, (unsigned)want,
                  (unsigned)(st3 & 0xE7u), (st3 & ST3_FT) ? 1 : 0, (st3 & ST3_WP) ? 1 : 0, (st3 & ST3_RY) ? 1 : 0,
                  (st3 & ST3_T0) ? 1 : 0, t0ok, b1, ry0_close,
                  (unsigned)p[1], rg.ret_tx, (unsigned)rg.polls, rg.msr_n ? rmh : "-", rh, (unsigned)rg.c, (unsigned)rg.cc,
                  (long long)rg.us, rg.fail, rid_ic_ok, rid_chs, rid_n, b2, c0, (a8 && b1 && b2) ? 1 : 0,
                  RL_STATE_NAME[s_rl_state], send ? "sent" : NMF_REF_NAME[ref], (long long)(rl_now_us() - t0));
    }
    if (!send) {
        rl_nmf_refuse(ref, s_nmf_run_active && us == s_nmf_run_us);
        return;
    }

    /* 送信。送信関数を呼ぶ前に走行状態を進め、連鎖を0にし、飛行中フラグを立て、照合表のそのトラックを無効化する
     * (送信失敗・後続の例外経路に関わらず、次のトラックは検証 ok まで送れない) */
    if (!s_nmf_run_active) {
        if (!s_nmf_ep_valid[us] || s_nmf_ep_ue[us] != s_nm_ue[us]) {   /* ディスク交換等でエポックが変われば数え直す */
            s_nmf_run_n[us] = 0;
            s_nmf_ep_ue[us] = s_nm_ue[us];
            s_nmf_ep_valid[us] = 1;
        }
        s_nmf_run_active = 1;
        s_nmf_run_us = us;
        s_nmf_run_n[us]++;
        s_nmf_run_ue = s_nm_ue[us];
        s_nmf_runs_started++;
        debug_log(P1067_TAG " run f=%d start unit=%u run_n=%u/%u ue=%u gen=%u runs_started=%u\n",
                  g_mx68k_frame_num, (unsigned)us, (unsigned)s_nmf_run_n[us], (unsigned)FU_NMF_RUNS_PER_EPOCH,
                  (unsigned)s_nm_ue[us], (unsigned)gen_now, (unsigned)s_nmf_runs_started);
    }
    s_nmf_run_pos = k + 1u;
    s_nmf_chain_ok = 0;
    s_nmf_inflight = 1;
    s_nmf_sent_us = us; s_nmf_sent_c = c; s_nmf_sent_h = hd; s_nmf_sent_k = k;
    s_nmf_sent_ue = s_rl_wr_ue_begin[us];   /* = この FORMAT の収集開始時の ue(c0 で現在値と一致を確認済み) */
    s_nmf_sent_gen = s_rl_wr_gen_begin;
    /* V-5: このトラックの読み戻し照合表を無効化する(FORMAT の成否に関わらず旧内容を照合に使わない。uncertain は変えない) */
    for (uint8_t rr = 1; rr <= FU_AR_NSEC; rr++) {
        rl_nm_sec_t* e = &s_nm_map[us][rl_ar_idx(c, hd, rr)];
        e->written = 0;
        e->pending = 0;
    }
    s_nmf_map_inval++;
    s_rl_wr_wp_at_send = 0;   /* ログ整合用(FORMAT のリザルト判定は rl_fmt_result_classify が行う) */
    s_nmf_sends++;
    int r;
#ifdef MX68K_FLOPPYU_USB_SELFTEST
    if (s_st_nmf_force_ret != 0) { r = s_st_nmf_force_ret; s_st_nmf_force_ret = 0; }
    else
#endif
    r = rl_send_write_cmd(n);   /* B0 <n> B1 <n>>8> 83 4D 83 <HD/US> 83 N 83 SC 83 GPL 83 D(16B) */
    if (r != -1) s_rl_last_io_frame[s_rl_us_mx & 3u] = g_mx68k_frame_num;   /* P1092: 表示用 */
    if (s_nmf_lines < FU_AR_LOG_LIMIT) {
        s_nmf_lines++;
        debug_log(P1067_TAG " sent f=%d ret=%d n=%u unit=%u C=%u H=%u k=%u run_pos_after=%u sends=%u inflight=%d\n",
                  g_mx68k_frame_num, r, (unsigned)n, (unsigned)us, (unsigned)c, (unsigned)hd, (unsigned)k,
                  (unsigned)s_nmf_run_pos, (unsigned)s_nmf_sends, s_nmf_inflight);
    }
    if (r == -1) {   /* 未接続: 何も届いていないが走行は進めたので停止(半端な走行のまま書込みを続けない) */
        s_nmf_inflight = 0;
        rl_nmf_halt("send_not_connected");
        rl_synth_err(0x02u, 0);
        rl_raise_int();
        return;
    }
    if (r < 0) {     /* -2/-3: 送信途中で失敗し得る(装置状態不明 → ラッチ。rl_wr_fail_unknown が停止も立てる) */
        s_rl_wr_sends++;
        rl_wr_fail_unknown("unknown_p1");
        return;
    }
    s_rl_wr_sends++;
    rl_wr_enter_wait_w(n);

    /* 送信直後の同期'W'待ち(P1064 C-2 と同じ)。'W' は既存の 'W' ハンドラ(受理ゲート)がそのまま rl_wr_send_data() を呼ぶ */
    int64_t ts = rl_now_us(), wdl = ts + (int64_t)FU_FMT1_W_WAIT_MS * 1000;
    while (s_rl_state == RL_WR_WAIT_W && !s_rl_wr_data_sent && rl_now_us() < wdl) {
        rl_poll();
        if (s_rl_state != RL_WR_WAIT_W || s_rl_wr_data_sent) break;
        rl_sync_sleep();
    }
    int64_t te = rl_now_us();
    const char* ws = s_rl_wr_data_sent ? "hit" : (s_rl_state != RL_WR_WAIT_W ? "state_changed" : "timeout");
    if (s_nmf_lines < FU_AR_LOG_LIMIT) {
        char ev[FU_WR_EV_MAX * 24 + 4];
        s_nmf_lines++;
        rl_wr_events_str(ev, sizeof(ev));
        debug_log(P1067_TAG " wsync f=%d w_sync=%s w_wait_us=%lld data_sent=%d state=%s events=%s\n",
                  g_mx68k_frame_num, ws, (long long)(te - ts), s_rl_wr_data_sent, RL_STATE_NAME[s_rl_state], ev);
    }
}

/* FORMAT 専用のリザルト判定(Inside X68000 p.399-401/409)。C/H/R(r[3..5])は無意味なので判定に使わない。
 * 採用なら outcome、不採用なら NULL と *why。P1067: fmt1 と normal FORMAT の共通ヘルパー。inflight は呼出し側が
 * 渡す飛行中フラグ(fmt1 は s_rl_fmt1_inflight、normal は s_nmf_inflight)。判定本体は P1064 から不変 */
static const char* rl_fmt_result_classify(const uint8_t* r, const char** why, int inflight)
{
    const uint8_t* p = s_rl_cmd;
    uint8_t us = p[1] & 7u;
    int all_same = 1;
    for (uint32_t i = 1; i < FU_RESULT_LEN; i++) if (r[i] != r[0]) all_same = 0;
    *why = "-";
    if (!inflight)                    { *why = "fmt_without_send"; return NULL; }
    if (all_same)                     { *why = "all_same";         return NULL; }
    if ((r[0] & 0x07u) != us)         { *why = "us_mismatch";      return NULL; }
    if ((r[0] & 0xC0u) == 0x00u) {
        if (r[0] == us && r[1] == 0x00u && r[2] == 0x00u)
            return (r[6] == p[2]) ? "ok_formatted" : "ok_fmt_n_unexpected";   /* 後者は装置値をゲストへ返しラッチ */
        *why = "ic00_not_clean";      /* IC=00 で SE/EC/NR が立つ、または ST1/ST2 非0 */
        return NULL;
    }
    if (r[0] == (uint8_t)(0x40u | us) && r[1] == 0x02u && r[2] == 0x00u) return "device_nw_fmt";   /* FDC が WP を検出 */
    *why = "fmt_abnormal";            /* OR・NR・EC・MA・IC=10/11 等 */
    return NULL;
}

/* fmt1 の result 行(採用/不採用とも7B生値を出す) */
static void rl_fmt1_log_result(const uint8_t* res, const char* outcome, const char* why, int64_t now)
{
    if (s_rl_fmt1_lines >= FU_AR_LOG_LIMIT) return;
    char ev[FU_WR_EV_MAX * 24 + 4], rh[2 * 8 + 4];
    s_rl_fmt1_lines++;
    rl_wr_events_str(ev, sizeof(ev));
    rl_hex(rh, sizeof(rh), res, FU_RESULT_LEN, FU_RESULT_LEN);
    debug_log(P1064_TAG " result f=%d result_dev=%s st0=0x%02x st1=0x%02x st2=0x%02x n_res=%u n_cmd=%u outcome=%s why=%s"
              " since_send_us=%lld events=%s data_sent=%d latched=%d inflight=%d verify_pending=%d ok=%u nw=%u"
              " n_unexp=%u invalid=%u\n",
              g_mx68k_frame_num, rh, res[0], res[1], res[2], res[6], s_rl_cmd[2], outcome, why,
              (long long)(now - s_rl_wr_send_t_us), ev, s_rl_wr_data_sent, s_rl_wr_latched_off, s_rl_fmt1_inflight,
              s_rl_fmt1_verify_pending, (unsigned)s_rl_fmt1_ok, (unsigned)s_rl_fmt1_nw, (unsigned)s_rl_fmt1_n_unexp,
              (unsigned)s_rl_fmt1_invalid);
}

/* P1051: 書込みリザルトの CHRN の形(ログ・集計用)。a=形A(C+1,H,01,N、同期経路の実FDC)、c=形C(C,H,R+1,N)、
 * 4=第4形(R>8 で C,H,01,N。形C・第4形はキャッシュ経路の合成応答、app.c@1518-1528)、other=それ以外(形B を含む) */
static const char* rl_wr_form(const uint8_t* r)
{
    const uint8_t* p = s_rl_cmd;
    if (r[3] == (uint8_t)(p[2] + 1u) && r[4] == p[3] && r[5] == 0x01u && r[6] == p[5]) return "a";
    uint8_t rn = (uint8_t)(p[4] + 1u);
    if (r[3] == p[2] && r[4] == p[3] && r[6] == p[5]) {
        if (rn > 8u) { if (r[5] == 0x01u) return "4"; }
        else if (r[5] == rn) return "c";
    }
    return "other";
}

/* P1035: mode1・WP=0 観測時の装置リザルト7B検査。異常終了は全て不採用。
 * P1051: 正常終了は形A(同期経路の実FDCリザルト)だけを ok_written とし、形B・形C(P1039 の受理を撤回)・第4形は
 * ok_chrn_unexpected(装置値をゲストへ返しラッチ)。形C・第4形は装置が物理書込み前に返す合成応答のため */
static const char* rl_wr_result_classify_write(const uint8_t* r, const char** why)
{
    const uint8_t* p = s_rl_cmd;
    uint8_t us_dev = p[1] & 7u;
    int all_same = 1;
    for (uint32_t i = 1; i < FU_RESULT_LEN; i++) if (r[i] != r[0]) all_same = 0;
    *why = "-";
    if (all_same)                       { *why = "all_same";        return NULL; }
    if ((r[0] & 0x07u) != us_dev)       { *why = "us_mismatch";     return NULL; }
    if ((r[0] & 0xC0u) != 0x00u)        { *why = "ic_not_00_write"; return NULL; }   /* NW/ND/DE/OR 等は段階1では全て不採用 */
    if (r[0] != us_dev)                 { *why = "st0_not_exact";   return NULL; }   /* IC=00 で SE/EC/NR が立つ不定値 */
    if (r[1] != 0x00u || r[2] != 0x00u) { *why = "ic00_st_nonzero"; return NULL; }
    const char* fm = rl_wr_form(r);
    if (strcmp(fm, "a") == 0) return "ok_written";
    /* P1089(B-4): normal の高速モードに限り、装置キャッシュ経路の合成応答(形C・第4形)を ok_cached として受理する。
     * ★この関数は mode1・armed からも呼ばれる共有関数なので、モード条件(WRM_NORMAL)を必ず含めること */
    if (s_rl_write_mode == WRM_NORMAL && s_nm_fast && (strcmp(fm, "c") == 0 || strcmp(fm, "4") == 0)) return "ok_cached";
    return "ok_chrn_unexpected";   /* 装置値をゲストへ返しラッチ(P1032 U-5 と同型の保守策、P1035 U-5) */
}

/* P1087(D-126a): normal の第2段判定。第1段(rl_wr_result_classify_write)で ic_not_00_write になった装置リザルトだけを
 * 再判定し、「物理的な書込み失敗として説明がつく整った異常終了」かを調べる。
 * 戻り値: 1=素通し可(装置の本物の7Bをゲストへ返し、ラッチしない)
 *         0=素通し不可(*nmerr に最初に落ちた理由。呼出し元は従来どおり invalid_result=ラッチ+bringup無効化+合成 OR)
 *         2=上限到達(E1〜E6 は全て満たすが素通し総数が FU_NM_ERR_MAX に達した。*nmerr="limit"。呼出し元は
 *           本物の7Bをゲストへ返した上でラッチのみ行う。bringup 無効化・合成 OR はしない)
 * 条件(上から順に評価): E1 ST0=0x40|送信US/HD(IC=01 で NR/EC/SE なし) / E2 ST1 非0・NW なし・許可集合内 / E3 ST2=0 /
 * E4 CHRN=送信 CHRN のエコー / E5 7B読出し直後の装置 MSR=0x80(残留結果フェーズなし) / E6 送信記録あり。
 * 根拠・各値の意味は .mx68k_cycles/P1087_plan.md 記号表 */
static int rl_nm_err_classify(const uint8_t* r, uint8_t msr_post, int msr_post_ret, const char** nmerr)
{
    const uint8_t* p = s_rl_cmd;
    if (r[0] != (uint8_t)(0x40u | (p[1] & 7u)))           { *nmerr = "st0";         return 0; }   /* E1 */
    if (r[1] == 0x00u)                                     { *nmerr = "st1_zero";    return 0; }   /* E2a */
    if (r[1] & 0x02u)                                      { *nmerr = "st1_nw";      return 0; }   /* E2b */
    if (r[1] & (uint8_t)~FU_NM_ERR_ST1_OK)                 { *nmerr = "st1_unknown"; return 0; }   /* E2c */
    if (r[2] != 0x00u)                                     { *nmerr = "st2";         return 0; }   /* E3 */
    if (r[3] != p[2] || r[4] != p[3] || r[5] != p[4] || r[6] != p[5]) { *nmerr = "chrn"; return 0; }   /* E4 */
    if (msr_post_ret < 1 || msr_post != MSR_RQM)           { *nmerr = "msr_post";    return 0; }   /* E5 */
    if (!s_nm_cur_valid)                                   { *nmerr = "no_send";     return 0; }   /* E6 */
    if (s_nm_err_n >= FU_NM_ERR_MAX)                       { *nmerr = "limit";       return 2; }   /* 上限 */
    *nmerr = "pass";
    return 1;
}

/* P1034: wponly('w'/'C'後)の装置リザルト7Bの妥当性検査。採用ならoutcome、不採用ならNULLと*why。
 * ★wponly(WP確認済みディスクへの送信)専用の期待値検査であり、書込み可能ディスク(モード1)には流用不可。
 * P1035: mode1 で WP=0 を観測して送った場合だけ先頭で rl_wr_result_classify_write へ分岐する(本体は不変)。
 * P1037: armed も同じ分岐へ入る(armed は WP=1 を送らないので常にこちら) */
static const char* rl_wr_result_classify(const uint8_t* r, const char** why)
{
    /* P1064: FORMAT のリザルトは wponly/WRITE DATA 用判定へ落とさない。P1067: 「FORMAT のリザルトか」はモードではなく
     * モード非依存の飛行中フラグで判別する(normal FORMAT は WRM_NORMAL のままこの経路を通るため) */
    if (s_rl_fmt1_inflight || s_nmf_inflight)
        return rl_fmt_result_classify(r, why, s_rl_fmt1_inflight ? s_rl_fmt1_inflight : s_nmf_inflight);
    if (s_rl_write_mode == WRM_FMT1) return rl_fmt_result_classify(r, why, 0);   /* fmt1 で飛行中でない→fmt_without_send(従来同値) */
    if ((s_rl_write_mode == WRM_WRITE || s_rl_write_mode == WRM_ARMED || s_rl_write_mode == WRM_NORMAL) && !s_rl_wr_wp_at_send)
        return rl_wr_result_classify_write(r, why);   /* P1053: normal は WP=1 を送らないので常にこちら */
    uint8_t us_dev = s_rl_cmd[1] & 7u;   /* 送信したUS/HD(装置番号へ変換済み) */
    int all_same = 1;
    for (uint32_t i = 1; i < FU_RESULT_LEN; i++) if (r[i] != r[0]) all_same = 0;
    *why = "-";
    if (all_same)                     { *why = "all_same";    return NULL; }   /* 0x80/0xFF連続等の不定値 */
    if ((r[0] & 0x07u) != us_dev)     { *why = "us_mismatch"; return NULL; }
    if ((r[0] & 0xC0u) == 0x00u) {
        /* 正常終了ならST1/ST2は0のはず(Fable5 F-1)。満たさなければ不定値として不採用 */
        if (r[1] != 0x00u || r[2] != 0x00u) { *why = "ic00_st_nonzero"; return NULL; }
        return "ok_UNEXPECTED_in_wponly";   /* WPなのに正常終了: 装置値をゲストへ返しラッチ(P1032 U-5) */
    }
    if ((r[0] & 0xC0u) != 0x40u)      { *why = "ic_10_11";    return NULL; }
    if (r[0] == (uint8_t)(0x40u | us_dev) && r[1] == 0x02u && r[2] == 0x00u &&
        r[3] == s_rl_cmd[2] && r[4] == s_rl_cmd[3] && r[5] == s_rl_cmd[4] && r[6] == s_rl_cmd[5])
        return "device_nw";   /* 期待値 40|US 02 00 C H R N 完全一致のみ受理 */
    *why = "ic40_not_nw";     /* NW以外の異常終了・CHRN不一致等(P1032の device_err_other はここへ統合) */
    return NULL;
}

/* P1037: armed の採用リザルトの後処理。形A(C+1,H,01,N、P1035 H-3 実機 result_dev=0400004D010103)のみ受理し、
 * それ以外は ok_chrn_unexpected(装置値はゲストへ返し、永久停止)。P1051: P1039 で追加した形C の受理は撤回
 * (形C は装置のキャッシュ経路の合成応答で、物理書込み前に返る)。照合表を更新して outcome を返す */
static const char* rl_ar_on_result(const uint8_t* res, const char* outcome)
{
    const uint8_t* p = s_rl_cmd;
    int form_a = res[3] == (uint8_t)(p[2] + 1u) && res[4] == p[3] && res[5] == 0x01u && res[6] == p[5];
    int form_c = res[3] == p[2] && res[4] == p[3] && res[5] == (uint8_t)(p[4] + 1u) && res[6] == p[5];   /* ログ用 */
    if (strcmp(outcome, "ok_written") == 0 && !form_a) outcome = "ok_chrn_unexpected";
    if (!s_rl_ar_cur_valid) { rl_ar_disarm("result_without_send"); return outcome; }   /* 送信記録が無い(想定外)→停止 */
    rl_ar_sec_t* e = &s_rl_ar_map[s_rl_ar_cur_idx];   /* §d で範囲検査済みのコマンドのみ */
    s_rl_ar_cur_valid = 0;
    if (strcmp(outcome, "ok_written") == 0) {
        e->written = 1; e->pending = 1; e->uncertain = 0; e->writes++;
        e->verified = 0;   /* 新しい書込みは未照合に戻す(前回内容の照合結果で D-8-wv を過大計上しない、Fable5 R1) */
        e->last_fnv = rl_fnv(s_rl_wrcollect_buf, s_rl_wr_n);
        s_rl_ar_ok_written++;
    } else {
        e->uncertain = 1; s_rl_ar_ok_unexp++; rl_ar_disarm("chrn_unexpected");
    }
    rl_ar_log_r(res, outcome, form_a, form_c, rl_fnv(s_rl_wrcollect_buf, s_rl_wr_n), e->writes);
    return outcome;
}

/* P1053: normal の採用リザルトの後処理。形A(ok_written)のみ照合表へ記録し、それ以外(ok_chrn_unexpected)は
 * 当該セクタを照合対象外にしてラッチする(呼出し元でも ok_chrn_unexpected はラッチされる)。
 * P1087: device_err_nm(装置の整った異常終了の素通し)は当該セクタを照合対象外にするだけでラッチしない。
 * device_err_nm_limit(素通し総数の上限到達)は同じく照合対象外にした上でラッチする。
 * ★この2分岐は、非 ok_written を全てラッチする末尾の else より必ず前に置くこと */
static const char* rl_nm_on_result(const char* outcome, uint8_t st1)
{
    if (!s_nm_cur_valid) { rl_nm_latch("result_without_send"); return outcome; }
    s_nm_cur_valid = 0;
    if (strcmp(outcome, "device_err_nm") == 0 || strcmp(outcome, "device_err_nm_limit") == 0) {
        int is_limit = strcmp(outcome, "device_err_nm_limit") == 0;
        if (s_nm_cur_mapped) s_nm_map[s_nm_cur_us][s_nm_cur_idx].uncertain = 1;   /* written/pending/last_fnv は触らない */
        if (is_limit) {
            s_nm_err_limit++;
            rl_nm_latch("err_limit");
        } else {
            s_nm_err_n++;
            if (st1 & 0x04u) s_nm_err_nd++;
            if (st1 & 0x20u) s_nm_err_de++;
            if (st1 & 0x10u) s_nm_err_or++;
            if (st1 & 0x01u) s_nm_err_ma++;
        }
        return outcome;
    }
    /* P1089(B-5): ok_cached(高速モードの装置合成応答)も照合表へ記録し cached=1 とする。計数は ok_written と分ける
     * (D-8-w の「確定」書込み数は形A のみ)。形A の受理は同期経路=装置キャッシュ flush 後なので dirty を落とす */
    int is_cached = strcmp(outcome, "ok_cached") == 0;
    if (strcmp(outcome, "ok_written") == 0 || is_cached) {
        if (is_cached) {
            s_nm_ok_cached++;
            rl_wc_on_cached(s_rl_cmd[1], s_nm_cur_mapped, s_nm_cur_us, s_nm_cur_idx);
        } else {
            s_nm_ok_written++;
            if (s_fu_wc_dirty) rl_wc_clear(WCC_FORMA);
        }
        if (!s_nm_cur_mapped) { s_nm_v_unmapped++; return outcome; }
        rl_nm_sec_t* e = &s_nm_map[s_nm_cur_us][s_nm_cur_idx];
        e->written = 1; e->pending = 1; e->uncertain = 0; e->writes++;
        e->cached = (uint8_t)is_cached;
        e->last_fnv = rl_fnv(s_rl_wrcollect_buf, s_rl_wr_n);
        e->len = s_rl_wr_n;
        e->epoch_ue = s_nm_cur_ue;
        e->epoch_gen = s_nm_cur_gen;
    } else {
        s_nm_ok_unexp++;
        if (s_nm_cur_mapped) s_nm_map[s_nm_cur_us][s_nm_cur_idx].uncertain = 1;
        rl_nm_latch("chrn_unexpected");
    }
    return outcome;
}

/* P1034: result 行の出力(採用/不採用とも result_dev に7B生値を出す) */
static void rl_wr_log_result(const char* via, const uint8_t* res, const char* outcome, const char* why,
                             uint8_t msr_post, int msr_post_ret, int64_t now, const char* nmerr)
{
    /* P1057: invalid_result は稀・診断上重要なため予算対象外で必ず出す(他は従来どおり予算共有)。
     * P1087(Fable5 M1): device_err_nm(素通し)・device_err_nm_limit(上限到達)も稀・診断上重要なため予算対象外
     * (素通し・上限到達は合計 FU_NM_ERR_MAX+1 件で有界。長時間の書込みで ok_written が予算を使い切っても必ず出す) */
    int no_budget = outcome && (strcmp(outcome, "invalid_result") == 0 || strcmp(outcome, "device_err_nm") == 0 ||
                                strcmp(outcome, "device_err_nm_limit") == 0);
    if (!no_budget && s_rl_wr_lines >= FU_LOG_LIMIT) return;
    char ev[FU_WR_EV_MAX * 24 + 4], mh[2 * 16 + 4], rh[2 * 8 + 4];
    if (!no_budget) s_rl_wr_lines++;
    rl_wr_events_str(ev, sizeof(ev));
    rl_hex(mh, sizeof(mh), s_rl_wr_res_msr, s_rl_wr_res_msr_n, 16);
    rl_hex(rh, sizeof(rh), res, FU_RESULT_LEN, FU_RESULT_LEN);
    debug_log(P1032_TAG " result f=%d gen=%u via=%s result_dev=%s st0_mx=0x%02x write_outcome=%s invalid_why=%s"
              " msr_post=0x%02x msr_post_ret=%d events=%s"
              " data_sent=%d polls=%u msr_seq=%s since_send_us=%lld latched=%d sends=%u form=%s nmerr=%s\n",
              g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), via, rh,
              (unsigned)p955_st_dev_to_mx(res[0]), outcome, why, (unsigned)msr_post, msr_post_ret, ev,
              s_rl_wr_data_sent, (unsigned)s_rl_wr_res_polls,
              s_rl_wr_res_msr_n ? mh : "-", (long long)(now - s_rl_wr_send_t_us), s_rl_wr_latched_off,
              (unsigned)s_rl_wr_sends, rl_wr_form(res), nmerr ? nmerr : "-");
}

/* P1032/P1034: 結果7Bを1Bずつ同期読出しして検査し、ゲストへ返す。取得(採用/不採用とも)したら1。
 * require_msr=1('C'経由・tick/MSRポーリング・timeout_probe): 装置MSRがRQM|DIOのときだけ読む(装置FDCに結果フェーズが残る経路)。
 * require_msr=0('w'経由): 装置MSRを見ずに読む(XEiJ FUK.java:992-999/533-538、XM6t fdc_bridger.h:964-971 と同じ。
 *   P1033実機で'w'後の装置MSRは0x80固定だった)。
 * 読出しはリトライしない(装置ファームのバッファを読み進める可能性があるため) */
static int rl_wr_result_fetch(const char* via, int require_msr)
{
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    if (s_rl_state != RL_WR_RESULT) return 0;
    s_rl_wr_res_last_poll_us = rl_now_us();
    s_rl_wr_res_polls++;
    if (require_msr) {
        uint8_t msr = 0;
        if (rl_p1("wr_msr", TX_MSR, 1, &msr, 1) < 1) return 0;   /* 故障停止なら rl_fault 内で打切り済み */
        if (s_rl_state != RL_WR_RESULT) return 0;
        if (s_rl_wr_res_msr_n < sizeof(s_rl_wr_res_msr)) s_rl_wr_res_msr[s_rl_wr_res_msr_n++] = msr;
        if ((msr & 0xC0u) != 0xC0u) return 0;
    }
    uint8_t res[FU_RESULT_LEN];
    for (uint32_t i = 0; i < FU_RESULT_LEN; i++) {
        if (rl_p1("wr_result", TX_DATA, 1, &res[i], 1) < 1) return 0;   /* -2/-3 は rl_fault→rl_abort_op でラッチ済み */
        if (s_rl_state != RL_WR_RESULT) return 0;
    }
    /* 故障停止へ遷移しない rl_p1_nofault で取得する(取得失敗でも故障停止にしない、据置)。
     * P1087: 従来はログ専用だったが、normal の第2段判定(E5)の入力にも使う。取得に失敗した/0x80 以外なら素通しせず
     * 従来どおり invalid_result(=素通しを禁じる方向にしか働かない)。第1段判定・他モードには影響させない */
    uint8_t msr_post = 0;
    int msr_post_ret = rl_p1_nofault(TX_MSR, 1, &msr_post, 1);
    if (msr_post_ret < 1) msr_post = 0;   /* ログでは msr_post_ret<1 で判別する(0x00 は正常応答としてもあり得るため値単独で判別しない) */
    const char* why;
    const char* outcome = rl_wr_result_classify(res, &why);
    int64_t now = rl_now_us();
    /* P1087(D-126a): normal 限定の第2段判定。第1段で ic_not_00_write になったものだけを再判定し、整った異常終了
     * (ND 等)なら装置の本物の7Bをゲストへ素通しする(XEiJ+t FUK.java:533-539・XM6t fdc_bridger.h:351-356 の透過へ寄せる)。
     * mode1・armed・wponly・fmt1・normal FORMAT は判定式に入らない(従来どおり) */
    const char* nmerr = "-";
    if (!outcome && strcmp(why, "ic_not_00_write") == 0 && s_rl_write_mode == WRM_NORMAL &&
        !s_rl_wr_wp_at_send && !s_nmf_inflight && !s_rl_fmt1_inflight) {
        int k = rl_nm_err_classify(res, msr_post, msr_post_ret, &nmerr);
        if (k == 1)      { outcome = "device_err_nm";       why = "-"; }
        else if (k == 2) { outcome = "device_err_nm_limit"; why = "-"; }   /* nmerr="limit" */
        else             s_nm_err_rej++;
    }
    if (!outcome) {
        /* 不採用: M1と同じラッチ経路(ラッチ+bringup無効化+合成エラー+割込み)。ラッチ解除経路は作らない */
        s_rl_wr_invalid++;
        s_rl_wr_latched_off = 1;
        s_rl_bringup_valid = 0;
        rl_rebringup_arm("invalid_result");   /* P1087(D) */
        rl_wr_log_result(via, res, "invalid_result", why, msr_post, msr_post_ret, now, nmerr);
        rl_ar_after_unknown("invalid_result", res);   /* P1037: armed のみ */
        if (s_rl_fmt1_inflight) {                     /* P1064: spent はそのまま。P1067: モード判定→飛行中フラグ(fmt1 では同値) */
            s_rl_fmt1_inflight = 0;
            s_rl_fmt1_invalid++;
            rl_fmt1_log_result(res, "invalid_result", why, now);
        }
        if (s_nmf_inflight) {                         /* P1067 */
            s_nmf_inflight = 0;
            s_nmf_invalid++;
            rl_nmf_halt("invalid_result");
        }
        rl_synth_err(ST1_OR, 1);
        rl_raise_int();
        return 1;
    }
    if (s_rl_write_mode == WRM_ARMED) outcome = rl_ar_on_result(res, outcome);   /* P1037: 形A判定・照合表更新 */
    /* P1053: 照合表更新。P1067: normal FORMAT の飛行中は呼ばない(WRITE DATA の照合カーソルを持たないため result_without_send になる) */
    if (s_rl_write_mode == WRM_NORMAL && !s_nmf_inflight) outcome = rl_nm_on_result(outcome, res[1]);
    /* P1087: 装置の本物の異常終了を素通しする場合(device_err_nm 系)。form_* 統計には数えず nm_err 側で数える */
    int is_nm_err = strcmp(outcome, "device_err_nm") == 0 || strcmp(outcome, "device_err_nm_limit") == 0;
    /* P1064: ファームは WRITE ID を書込みキャッシュ対象にしない(app.c isWriteCommandGroup@1284)ので反事実モデルを更新しない。
     * P1067: モード判定→飛行中フラグ(normal FORMAT も対象外) */
    if ((res[0] & 0xC0u) == 0x00u && !s_rl_fmt1_inflight && !s_nmf_inflight) {
        /* P1051: 反事実モデル。正常終了の書込みを装置が受理した後は(同期・キャッシュ経路とも)次の同コマンドがキャッシュ対象 */
        s_rl_cf_first = 0;
        s_rl_cf_cmd = s_rl_cmd[0];
    }
    if ((s_rl_write_mode == WRM_WRITE || s_rl_write_mode == WRM_ARMED || s_rl_write_mode == WRM_NORMAL) && !s_rl_wr_wp_at_send &&
        !s_nmf_inflight && !is_nm_err) {   /* P1067: FORMAT のリザルトは書込み形の統計に数えない。P1087: device_err_nm 系も */
        const char* fm = rl_wr_form(res);
        if      (strcmp(fm, "a") == 0) s_rl_sy_form_a++;
        else if (strcmp(fm, "c") == 0) s_rl_sy_form_c++;
        else if (strcmp(fm, "4") == 0) s_rl_sy_form4++;
        else                           s_rl_sy_form_other++;
    }
    if (strcmp(outcome, "ok_UNEXPECTED_in_wponly") == 0 || strcmp(outcome, "ok_chrn_unexpected") == 0 ||
        strcmp(outcome, "ok_fmt_n_unexpected") == 0)
        s_rl_wr_latched_off = 1;   /* P1032 U-5 / P1035 U-5。ok_written はラッチしない(以後は送信上限1回で止まる) */
    if (s_rl_fmt1_inflight) {   /* P1067: モード判定→飛行中フラグ(fmt1 では同値) */
        /* P1064: ok_formatted なら検証 READ ID を予約する(送信時の世代・'D'/'d'受信数・SEEK数を基準にする)。
         * device_nw_fmt はラッチしない(装置へ書込みは届いたがFDCが WP で拒否した) */
        s_rl_fmt1_inflight = 0;
        if (strcmp(outcome, "ok_formatted") == 0) {
            s_rl_fmt1_ok++;
            s_rl_fmt1_verify_pending = 1;
            s_rl_fmt1_v_gen = s_rl_op_gen;
            s_rl_fmt1_v_d = s_rl_wr_d_begin;
            s_rl_fmt1_v_seek = s_rl_fmt1_send_seek_n;
            s_rl_fmt1_v_h = s_rl_fmt1_sent_h;   /* P1066: 検証対象の面 */
        } else if (strcmp(outcome, "device_nw_fmt") == 0) {
            s_rl_fmt1_nw++;
        } else if (strcmp(outcome, "ok_fmt_n_unexpected") == 0) {
            s_rl_fmt1_n_unexp++;
        }
        rl_fmt1_log_result(res, outcome, why, now);
    } else if (s_nmf_inflight) {
        /* P1067: ok_formatted なら検証を予約する(基準はこの FORMAT の収集開始時の ue・送信時の世代)。それ以外は停止
         * (device_nw_fmt は送信前 SDS で WP=0 を見ていたのに FDC が WP で拒否した不整合。ok_fmt_n_unexpected は上でラッチ済み) */
        s_nmf_inflight = 0;
        if (strcmp(outcome, "ok_formatted") == 0) {
            s_nmf_ok++;
            s_nmf_verify_pending = 1;
            s_nmf_v_us = s_nmf_sent_us; s_nmf_v_c = s_nmf_sent_c; s_nmf_v_h = s_nmf_sent_h; s_nmf_v_k = s_nmf_sent_k;
            s_nmf_v_ue = s_nmf_sent_ue;
            s_nmf_v_gen = s_nmf_sent_gen;
        } else if (strcmp(outcome, "device_nw_fmt") == 0) {
            s_nmf_nw++;
            rl_nmf_halt("device_nw");
        } else {
            s_nmf_n_unexp++;
            rl_nmf_halt("fmt_n_unexpected");
        }
        if (s_nmf_res_lines < FU_AR_LOG_LIMIT) {
            char rh[2 * 8 + 4], ev[FU_WR_EV_MAX * 24 + 4];
            s_nmf_res_lines++;
            rl_hex(rh, sizeof(rh), res, FU_RESULT_LEN, FU_RESULT_LEN);
            rl_wr_events_str(ev, sizeof(ev));
            debug_log(P1067_TAG " result f=%d unit=%u C=%u H=%u k=%u result_dev=%s outcome=%s why=%s since_send_us=%lld"
                      " events=%s data_sent=%d latched=%d verify_pending=%d halt=%d ok=%u nw=%u n_unexp=%u invalid=%u\n",
                      g_mx68k_frame_num, (unsigned)s_nmf_sent_us, (unsigned)s_nmf_sent_c, (unsigned)s_nmf_sent_h,
                      (unsigned)s_nmf_sent_k, rh, outcome, why, (long long)(now - s_rl_wr_send_t_us), ev, s_rl_wr_data_sent,
                      s_rl_wr_latched_off, s_nmf_verify_pending, s_nmf_halt, (unsigned)s_nmf_ok, (unsigned)s_nmf_nw,
                      (unsigned)s_nmf_n_unexp, (unsigned)s_nmf_invalid);
        }
    }
    if (s_rl_write_mode == WRM_WRITE && !s_rl_wr_wp_at_send) {
        /* P1035: mode1・WP=0 の採用結果を集計し、読み戻し照合を予約する */
        if (strcmp(outcome, "ok_written") == 0) s_rl_m1_ok_written++; else s_rl_m1_ok_chrn_unexp++;
        s_rl_wv_pending = 1;
        s_rl_wv_us = s_rl_cmd[1] & 3u;
        memcpy(s_rl_wv_chrn, s_rl_cmd + 2, 4);
        s_rl_wv_fnv = rl_fnv(s_rl_wrcollect_buf, s_rl_wr_n);
        s_rl_wv_outcome = outcome;
    }
    s_rl_wr_results++;
    s_rl_wr_result_t_us = (s_rl_wr_c_seen == 0u) ? now : 0;
    s_rl_wr_result_f = g_mx68k_frame_num;   /* P1099: 分岐の結果に関わらず記録(診断用) */
    rl_wr_log_result(via, res, outcome, why, msr_post, msr_post_ret, now, nmerr);
    /* P1087(C'-E): 本物の書込みエラーをゲストへ返す場合も、合成エラーと同じ排出抑止窓を起点させる */
    if (is_nm_err) rl_se_note(res[1]);
    res[0] = p955_st_dev_to_mx(res[0]);
    rl_set_result(res, FU_RESULT_LEN);
    rl_raise_int();
    return 1;
}

/* P1032: RL_WR_RESULT 専用の結果ポーリング(rl_rid_poll と同形、最小間隔 FU_RID_POLL_MIN_US) */
static void rl_wr_result_poll(int force, const char* via)
{
    if (s_rl_state != RL_WR_RESULT) return;
    int64_t now = rl_now_us();
    if (!force && s_rl_wr_res_last_poll_us != 0 && now - s_rl_wr_res_last_poll_us < FU_RID_POLL_MIN_US) return;
    (void)rl_wr_result_fetch(via, 1);   /* P1034: tick/MSRポーリング・'C'経路は従来どおりMSR確認あり */
}

/* P1032/P1034: 'w'/'C' を受けて結果取得へ。'w' は MSR を見ずに即取得、'C' は MSR 確認つきで取得を試みる */
static void rl_wr_enter_result(const char* via, int require_msr)
{
    if (s_rl_state == RL_WR_WAIT_W || s_rl_state == RL_WR_WAIT_DONE) {
        s_rl_state = RL_WR_RESULT;
        s_rl_wr_state_t_us = rl_now_us();
    }
    if (s_rl_state != RL_WR_RESULT) return;
    if (require_msr) { rl_wr_result_poll(1, via); return; }
    if (!rl_wr_result_fetch(via, 0) && s_rl_state == RL_WR_RESULT)
        rl_abort_op(RE_FAULT);   /* -1(未接続)等で故障停止に入らず取得できなかった場合も、部分消費のまま待たずにラッチ */
}

/* P1032: 書込み待ち中に受けた port2 イベントを記録する */
static void rl_wr_ev_add(uint8_t b)
{
    switch (b) {
    case 0x57u: s_rl_wr_ev_W++; break;
    case 0x77u: s_rl_wr_ev_w++; break;
    case 0x43u: s_rl_wr_ev_C++; s_rl_wr_c_seen++; break;
    case 0x74u: s_rl_wr_ev_t++; break;
    case 0x44u: case 0x64u: s_rl_wr_ev_D++; break;
    default: break;
    }
    if (s_rl_wr_ev_n < FU_WR_EV_MAX) {
        s_rl_wr_ev_c[s_rl_wr_ev_n] = (b >= 0x20u && b < 0x7Fu) ? (char)b : '?';
        s_rl_wr_ev_us[s_rl_wr_ev_n] = rl_now_us() - s_rl_wr_send_t_us;
        s_rl_wr_ev_n++;
    }
}

/* P1031: 収集完了。記録してから従来どおり NW エラー(rl_refuse と同じ ST1)。
 * 状態は rl_synth_err→rl_set_result が RL_RESULT へ遷移させるので、ここでは変更しない。
 * P1032: wponly モードでは rl_wr_try_send() が送信前検査を行い、不成立なら同じ NW を合成する */
static void rl_collect_done(void)
{
    s_p1100_fn = g_mx68k_frame_num;   /* P1100: 最後の1バイトを受けた時点(FORMAT 分岐より前で記録) */
    s_p1100_vln = VLINE;
    /* P1063: FORMAT の ID 表は WRITE DATA 用の経路(rl_wr_try_send 等)へ一切入れない */
    if (s_rl_wr_is_format) {
        s_rl_wr_is_format = 0;
        if (s_rl_write_mode == WRM_FMT1)        rl_fmt1_try_send();         /* P1064 */
        else if (s_rl_write_mode == WRM_DRYRUN) rl_fmt_dryrun_complete();
        else if (s_rl_write_mode == WRM_NORMAL && s_nmf_on) rl_nmf_try_send();   /* P1067 */
        else { rl_synth_err(0x02u, 0); rl_raise_int(); }   /* 到達しない設計(case 13 で弾く)。到達時は送らず NW(CHRN非エコー、F-6) */
        return;
    }
    uint32_t n = s_rl_wrcollect_pos;
    const uint8_t* p = s_rl_cmd;
    s_rl_wrcollect_done++;
    if (s_rl_wrcollect_lines < FU_LOG_LIMIT) {
        uint8_t tx[20];
        size_t tn = 0;
        char dh[2 * 32 + 8], th[2 * 20 + 4];
        s_rl_wrcollect_lines++;
        /* 次段の実送信で port1 へ送る予定のコマンド列(rl_send_cmd と同形)。ここでは組み立てて記録するだけ */
        for (uint32_t i = 0; i < s_rl_cmd_n && tn + 2 <= sizeof(tx); i++) {
            tx[tn++] = (uint8_t)(FU_WR | FU_REG_DATA);
            tx[tn++] = s_rl_cmd[i];
        }
        rl_hex(th, sizeof(th), tx, tn, 20);
        rl_hex(dh, sizeof(dh), s_rl_wrcollect_buf, n, 32);
        debug_log(P1031_TAG " done f=%d C=%u H=%u R=%u N=%u got=%u expect=%u frames=%d"
                  " MAR0_valid=%d MAR0=0x%06x MTC0=0x%04x OCR0=0x%02x CSR0=0x%02x MAR_now=0x%06x MTC_now=0x%04x"
                  " data_fnv=0x%08x head=%s planned_p1_cmd=%s planned_data_len=%u sent=0 done=%u/%u\n",
                  g_mx68k_frame_num, p[2], p[3], p[4], p[5], (unsigned)n, (unsigned)s_rl_wrcollect_n,
                  g_mx68k_frame_num - s_rl_wrcollect_f0,
                  s_rl_wrcollect_mar0_valid, (unsigned)s_rl_wrcollect_mar0, (unsigned)s_rl_wrcollect_mtc0,
                  (unsigned)s_rl_wrcollect_ocr0, (unsigned)s_rl_wrcollect_csr0,
                  (unsigned)(DMA[0].MAR & 0xFFFFFFu), (unsigned)DMA[0].MTC,
                  (unsigned)rl_fnv(s_rl_wrcollect_buf, n), dh, th, (unsigned)n,
                  (unsigned)s_rl_wrcollect_done, (unsigned)s_rl_wrcollect_begin);
    }
    if (s_rl_write_mode == WRM_WPONLY || s_rl_write_mode == WRM_WRITE || s_rl_write_mode == WRM_ARMED ||
        s_rl_write_mode == WRM_NORMAL) { rl_wr_try_send(); return; }
    rl_synth_err(0x02u, 1);
    rl_raise_int();
}

/* P1031: WR_COLLECT 中の $E94003 書込み(DMA経由・CPU書込みとも)を1B受け取る */
static void rl_collect_byte(uint8_t data)
{
    if (s_rl_wrcollect_pos == 0) {
        s_rl_wrcollect_mar0_valid = (DMA[0].CSR & 0x08u) ? 1 : 0;
        s_rl_wrcollect_mar0 = DMA[0].MAR & 0xFFFFFFu;
        s_rl_wrcollect_mtc0 = DMA[0].MTC;
        s_rl_wrcollect_ocr0 = DMA[0].OCR;
        s_rl_wrcollect_csr0 = DMA[0].CSR;
        s_p1100_f1 = g_mx68k_frame_num;   /* P1100 */
        s_p1100_vl1 = VLINE;
    }
    if (s_rl_wrcollect_pos < s_rl_wrcollect_n) s_rl_wrcollect_buf[s_rl_wrcollect_pos++] = data;
    if (s_rl_wrcollect_pos >= s_rl_wrcollect_n) rl_collect_done();
}

static void rl_rid_begin(void)
{
    s_rl_rid_begin++;
    s_rl_rp_total++;
    s_rl_op_gen = mx68k_floppyu_usb_generation();
    s_rl_op_t0_us = rl_now_us();
    s_rl_op_f0 = g_mx68k_frame_num;
    s_rl_rid_msr_n = 0;
    s_rl_rid_polls = 0;
    s_rl_rid_last_poll_us = 0;
    debug_log(P1009_TAG " rid begin f=%d gen=%u us_mx=0x%02x us_dev=0x%02x\n",
              g_mx68k_frame_num, (unsigned)s_rl_op_gen, (unsigned)s_rl_us_mx, (unsigned)(s_rl_cmd[1] & 7u));
    if (rl_send_cmd("read_id") < 0) {
        s_rl_rid_err++;
        rl_log_rid_end(RE_NAME[RE_FAULT], NULL);
        rl_synth_err(0, 0);
        rl_raise_int();
        return;
    }
    s_rl_state = RL_RID_WAIT;
}

/* RID_WAIT: 装置MSRを(最小間隔付きで)問い合わせ、RQM|DIO なら結果7Bを1Bずつ同期読出し */
static void rl_rid_poll(int force)
{
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    if (s_rl_state != RL_RID_WAIT) return;
    int64_t now = rl_now_us();
    if (!force && s_rl_rid_last_poll_us != 0 && now - s_rl_rid_last_poll_us < FU_RID_POLL_MIN_US) return;
    s_rl_rid_last_poll_us = now;
    uint8_t msr = 0;
    s_rl_rid_polls++;
    if (rl_p1("rid_msr", TX_MSR, 1, &msr, 1) < 1) return;   /* 故障停止なら rl_fault 内で打切り済み */
    if (s_rl_state != RL_RID_WAIT) return;
    if (s_rl_rid_msr_n < sizeof(s_rl_rid_msr)) s_rl_rid_msr[s_rl_rid_msr_n++] = msr;
    if ((msr & 0xC0u) != 0xC0u) return;
    uint8_t res[FU_RESULT_LEN];
    for (uint32_t i = 0; i < FU_RESULT_LEN; i++) {
        if (rl_p1("rid_result", TX_DATA, 1, &res[i], 1) < 1) return;
        if (s_rl_state != RL_RID_WAIT) return;
    }
    s_rl_rid_ok++;
    rl_log_rid_end("ok", res);
    res[0] = p955_st_dev_to_mx(res[0]);
    rl_set_result(res, FU_RESULT_LEN);
    rl_raise_int();
}

static void rl_sds(void)
{
    uint8_t key = s_rl_cmd[1] & 7u;
    int64_t now = rl_now_us();
    uint8_t st3 = 0;
    int from_cache = 0, r = 1;
    if (s_rl_sds_valid[key] && now - s_rl_sds_t[key] < (int64_t)FU_SDS_CACHE_MS * 1000) {
        st3 = s_rl_sds_val[key];
        from_cache = 1;
        s_rl_sds_cache++;
    } else {
        uint8_t tx[5] = { FU_WR | FU_REG_DATA, 0x04, FU_WR | FU_REG_DATA, s_rl_cmd[1], FU_REG_DATA };
        rl_wc_note_trigger(WCF_SDS);   /* P1089: キャッシュ失効時の装置 SDS は flush 契機(同じ往復の受信成功で dirty を落とす) */
        r = rl_p1("sds", tx, sizeof(tx), &st3, 1);
        if (r >= 1) {
            s_rl_sds_valid[key] = 1;
            s_rl_sds_val[key] = st3;
            s_rl_sds_t[key] = now;
            s_rl_sds_usb++;
            /* P1037: armed・許可ユニットのゲスト SDS で RY=0 を見た回数(計数のみ。誤停止を避けるためラッチしない、H-4-0 で観測) */
            if (s_rl_write_mode == WRM_ARMED && s_rl_ar_env_ok && (int)(key & 3u) == s_rl_ar_unit && !(st3 & ST3_RY))
                s_rl_ar_ry0_sds++;
        }
    }
    uint8_t st3_mx = (r >= 1) ? rl_st3_dev_to_mx(st3) : (uint8_t)(s_rl_us_mx & 7u);   /* 失敗時は Ready 無し */
    if (s_rl_sds_lines < FU_SDS_LOG_LIMIT) {
        s_rl_sds_lines++;
        debug_log(P1009_TAG " sds f=%d gen=%u us_mx=0x%02x us_dev=0x%02x cache=%d ret=%d st3_dev=0x%02x st3_mx=0x%02x\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)s_rl_us_mx,
                  (unsigned)key, from_cache, r, (unsigned)st3, (unsigned)st3_mx);
    }
    rl_set_result(&st3_mx, 1);   /* 割込み無し(Core:fdc.c:217-223 と同じ) */
}

static void rl_seek(void)
{
    int relay_after = 1;
    /* P1065: ヘッドを動かすコマンドを装置へ送る前に、予約中の検証 READ ID を行う(seek_n++ より前に呼ぶこと) */
    if (s_rl_fmt1_verify_pending) relay_after = rl_fmt1_verify(1);
    /* P1067: normal FORMAT の検証予約は「検証対象と同じユニット」への SEEK/RECAL のときだけ実行する。別ユニットの SEEK では
     * 検証対象のヘッドは動かないので予約を保持する(fmt1 の skip=unit→停止は流用しない) */
    if (s_nmf_verify_pending && (s_rl_cmd[1] & 3u) == s_nmf_v_us) {
        int post = 1;
        (void)rl_nmf_verify(NMF_VSRC_SEEK, &post);
        relay_after = relay_after && post;
    }
    /* P1070 X4: 完走したユニットへの RECAL、または目標≠77 の SEEK で完走記録を失効(検証の後に置く: その検証で完走した
     * 記録も消す)。s_rl_cmd[2](NCN)はゲストの書いた値のまま(rl_start が装置座標へ変換するのは [1] だけ) */
    if (s_nmf_ld_valid && (s_rl_cmd[1] & 3u) == s_nmf_ld_us &&
        ((s_rl_cmd[0] & 0x1Fu) == 7u || s_rl_cmd[2] != FU_NMF_ABSORB_CYL)) {
        s_nmf_ld_valid = 0;
        s_nmf_ld_exp_seek++;
    }
    const char* name = (s_rl_cmd[0] & 0x1Fu) == 7u ? "recal" : "seek";
    s_rl_fmt1_seek_n++;   /* P1064: 送信後に SEEK/RECAL が1回でもあれば検証 READ ID を行わない(送信の成否によらず数える) */
    /* P1065(C-2): 検証中に中継不可になったら装置へ送らず、既存の送信失敗経路でスタブへ。
     * スタブがNot Readyの割込みを出すのでDnBは立てない */
    if (!relay_after || rl_send_cmd(name) < 0) {
        rl_fallback_stub();
        rl_c_irq_flush("seek_fallback");
        return;
    }
    s_rl_cf_first = 1;   /* P1051: ゲストの RECAL/SEEK は装置の同期フラグを立てる(app.c@2166/2170) */
    uint8_t u = s_rl_cmd[1] & 3u;
    /* P1067: 装置へ実際に中継した後だけ PCN 記録を無効化する(次の SENSE INT まで位置は未確定)。送信できずスタブへ
     * 回した場合(上の rl_fallback_stub)は無効化しない——装置のヘッドは動いておらず、古い PCN が新しい ID 表の C と
     * 食い違うことが PCN 一致検査の拒否根拠になるため */
    rl_nmf_pcn_invalidate(u, &s_nmf_pcn_inval_seek);
    int64_t now = rl_now_us();
    s_rl_seek_t_us[u] = now ? now : 1;
    s_rl_seek_seq[u] = s_rl_c_seq;
    s_rl_state = RL_IDLE;   /* 結果フェーズ無し。MSRは0x80にDnBを重ねる(P1010) */
    s_rl_rdnum = 0;
    /* P1010: ビットはゲスト側番号(変換前)で立てる。装置側番号だと割当て23のときゲストの見るビットとずれる */
    rl_dnb_set(s_rl_us_mx & 3u, u, name);
    rl_c_irq_flush("seek");   /* P1028: リザルト無しコマンドは受付完了で再発行 */
}

static void rl_exec(void)
{
    uint8_t cmd = s_rl_cmd[0] & 0x1Fu;
    switch (cmd) {
    case 6: {   /* READ DATA: 単一セクタ(MT=0 かつ R==EOT)かつ (128<<N)+7 <= FU_FRAME_CAP のみ中継 */
        int mt = (s_rl_cmd[0] >> 7) & 1;
        uint8_t r = s_rl_cmd[4], n = s_rl_cmd[5], eot = s_rl_cmd[6];
        if (mt || r != eot || n > 6u) { rl_refuse(0, mt ? "mt" : (r != eot ? "multi_sector" : "n_too_large")); return; }
        rl_read_begin();
        return;
    }
    case 10: rl_rid_begin(); return;
    case 4:  rl_sds(); return;
    case 7: case 15: rl_seek(); return;
    case 5: {   /* WRITE DATA: dryrun/wponly/write(1)/armed/normal 時のみ単一セクタ(MT=0 かつ R==EOT)かつ N<=3 を収集。
                 * 装置へ送るのは wponly/write(1)/armed で送信前検査(write(1)・armed は柵を含む)が全て成立した場合だけ
                 * (rl_wr_try_send)。受付条件は P1037 でも変更しない */
        if (s_rl_write_mode == WRM_OFF) { rl_refuse(1, "write"); return; }
        int mt = (s_rl_cmd[0] >> 7) & 1;
        uint8_t r = s_rl_cmd[4], n = s_rl_cmd[5], eot = s_rl_cmd[6];
        if (mt || r != eot || n > 3u) {
            s_rl_wrcollect_refused++;
            if (mt) s_nmf_wr_mt++; else if (r != eot) s_nmf_wr_multi++; else s_nmf_wr_nbig++;   /* P1067: 内訳(計数のみ) */
            rl_refuse(1, mt ? "wr_mt" : (r != eot ? "wr_multi_sector" : "wr_n_too_large"));
            return;
        }
        rl_collect_begin();
        return;
    }
    case 9: rl_refuse(1, "write"); return;
    case 13:   /* P1063: FORMAT は dryrun だけ観測(装置へ送らず成功を合成)。P1064: fmt1 は柵つきで単一トラックを送る。
                * P1067: normal は設定「Allow FORMAT」(s_nmf_on)が ON のときだけ収集して送信前検査へ。
                * それ以外のモードは従来どおり無条件拒否 */
        s_nmf_seen++;   /* P1067: 分母(モード・トグルを問わない case 13 到達数) */
        if (s_rl_write_mode == WRM_DRYRUN || s_rl_write_mode == WRM_FMT1) { rl_fmt_collect_begin(); return; }
        if (s_rl_write_mode == WRM_NORMAL && s_nmf_on) { rl_fmt_collect_begin(); return; }
        s_nmf_ref[s_rl_write_mode == WRM_NORMAL ? NMF_REF_OFF : NMF_REF_MODE]++;
        rl_refuse(1, "write");
        return;
    default: rl_refuse(0, "unsupported"); return;   /* 2/12/17/25/29 */
    }
}

static void rl_start(uint8_t first, uint8_t second_mx)
{
    uint8_t cmd = first & 0x1Fu;
    s_rl_wr_is_format = 0;   /* P1063: 前のコマンドの FORMAT 収集フラグを持ち越さない */
    s_rl_cmd[0] = first;
    s_rl_cmd[1] = p955_us_mx_to_dev(second_mx);
    s_rl_cmd_n = 2;
    s_rl_us_mx = second_mx & 7u;
    s_rl_state = RL_IDLE;
    s_rl_rdnum = 0;
    s_rl_relay_hist[cmd]++;
    s_rl_wrnum = (uint32_t)P954_CMD_TABLE[cmd] - 1u;   /* 保留経路は CMD_TABLE>=1 のコマンドだけ */
    if (s_rl_wrnum == 0) rl_exec();
}

static void rl_param(uint8_t b)
{
    if (s_rl_cmd_n < sizeof(s_rl_cmd)) s_rl_cmd[s_rl_cmd_n++] = b;
    if (--s_rl_wrnum == 0) rl_exec();
}

/* SENSE INT を装置へ: 83 08 03→ST0、01→装置MSR、RQM|DIO なら 03→PCN(P1004 S6 と同じ列)。
 * 範囲外ユニット(装置US∈{2,3})のST0はゲストへ渡さず内部で再発行して読み捨てる */
static void rl_sense_int_dev(void)
{
    static const uint8_t TX_SI[]   = { FU_WR | FU_REG_DATA, 0x08, FU_REG_DATA };
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    uint32_t discards = 0;
    s_rl_sense_relay++;
    for (int iter = 0; ; iter++) {
        uint8_t st0 = 0x80, msr = 0, pcn = 0;
        int have_pcn = 0;
        int64_t t0 = rl_now_us();
        int r = rl_p1("sense_st0", TX_SI, sizeof(TX_SI), &st0, 1);
        if (r >= 1) r = rl_p1("sense_msr", TX_MSR, 1, &msr, 1);
        if (r >= 1 && (msr & 0xC0u) == 0xC0u) {
            r = rl_p1("sense_pcn", TX_DATA, 1, &pcn, 1);
            have_pcn = (r >= 1);
        }
        int inval = (st0 & 0xC0u) == 0x80u;
        int out = (r >= 1) && !inval && (st0 & 3u) > 1u;
        uint8_t st0_raw = st0;
        int ec_cleared = 0;
        /* EC と NR が両方立つ ST0 は EC を消す(XEiJ FUK.java:527-532、XM6t:434-443) */
        if (r >= 1 && (st0 & 0x18u) == 0x18u) { s_rl_ecnr_both++; st0 &= 0xEFu; ec_cleared = 1; }
        uint8_t st0_mx = (r < 1 || inval) ? (r < 1 ? 0x80u : st0) : p955_st_dev_to_mx(st0);
        debug_log(P1009_TAG " sense f=%d gen=%u route=dev iter=%d ret=%d st0_dev=0x%02x msr=0x%02x pcn=0x%02x have_pcn=%d"
                  " out_of_range=%d st0_mx=0x%02x ec_cleared=%d us=%lld\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), iter, r, (unsigned)st0_raw,
                  (unsigned)msr, (unsigned)pcn, have_pcn, out, (unsigned)st0_mx, ec_cleared,
                  (long long)(rl_now_us() - t0));
        if (r < 1) { uint8_t z = 0x80u; rl_set_result(&z, 1); return; }   /* 故障停止。保留無し扱い */
        if (out) {
            s_rl_st0_out_of_range++;
            if (++discards >= FU_SENSE_DRAIN_MAX) {
                s_rl_sense_drain_limit++;
                uint8_t z = 0x80u;
                rl_set_result(&z, 1);
                return;
            }
            continue;
        }
        rl_nmf_pcn_note(st0_raw, have_pcn, pcn);   /* P1067: 返す値は変えない(読むだけ) */
        if (inval) {
            if (st0 == 0x80u) s_rl_sense_relay_80++;
            rl_set_result(&st0, 1);
            return;
        }
        /* P1010: 範囲内・有効なST0でSE(Seek End)付きのときだけDnBを落とす(Ready変化0xC0|us等では落とさない) */
        if (st0_mx & 0x20u) rl_dnb_clear(st0_mx & 3u, "sense", st0_mx);
        else                s_rl_dnb_sense_no_se++;
        uint8_t res[2] = { st0_mx, pcn };
        rl_set_result(res, 2);
        return;
    }
}

static void rl_route_sense_int(uint32_t adr, uint8_t data)
{
    /* 中継不可だった時期に積まれた STUB は破棄(その割込みはスタブが発行済み) */
    while (s_p955_int_count > 0u && s_p955_int_fifo[s_p955_int_head] == P955_OWNER_STUB) {
        (void)p955_int_fifo_pop();
        s_rl_fifo_stale_stub++;
    }
    if (s_p955_int_count > 0u) {   /* ① 先頭が CORE(Core 側ドライブのシーク保留)なら Core へ */
        (void)p955_int_fifo_pop();
        s_rl_synth_after_read = 0;
        s_p955_owner = P955_OWNER_CORE;
        s_p955_sense_int_core++;
        s_rl_sense_core_fifo++;
        debug_log(P1009_TAG " sense f=%d gen=%u route=core_fifo\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation());
        FDC_Write(adr, data);
        rl_c_irq_flush("nonrelay");
        return;
    }
    s_p955_owner = P1009_OWNER_RELAY;
    s_rl_cmd[0] = data;
    s_rl_cmd_n = 1;
    s_rl_wrnum = 0;
    s_rl_us_mx = 0;
    s_rl_relay_hist[8]++;
    if (s_rl_synth_after_read) {   /* ② 中継READ DATA完了直後(その後コマンドも'C'も無い)は0x80を合成 */
        uint8_t z = 0x80u;
        s_rl_synth_after_read = 0;
        s_rl_sense_synth_after_read++;
        debug_log(P1009_TAG " sense f=%d gen=%u route=synth_after_read st0_mx=0x80\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation());
        rl_set_result(&z, 1);
        return;
    }
    rl_sense_int_dev();   /* ③ それ以外は装置へ */
}

/* port2 イベント 'C' */
static void rl_on_c(void)
{
    /* P1051: 同期手順中の'C'(挿入した SEEK・読み捨てる Ready 変化の割込み)はゲストへ渡さず、P1028 の保留にも入れない */
    if (s_rl_sync_active) { s_rl_sync_c++; return; }
    int st = s_rl_state;
    uint32_t seq = ++s_rl_c_seq;
    s_rl_c_events[st]++;
    int seek_u = -1;
    int64_t dt = -1;
    const char* act;
    if (st == RL_RD_WAIT || st == RL_RD_SUPPLY) {
        act = "ignored";
    } else if (st == RL_RID_WAIT) {
        act = "rid_hint";
    } else if (rl_in_write_wait()) {
        act = "wr_hint";   /* P1032 */
    } else {
        int64_t now = rl_now_us(), best = 0;
        for (int u = 0; u < 4; u++)
            if (s_rl_seek_t_us[u] && (seek_u < 0 || s_rl_seek_t_us[u] < best)) { seek_u = u; best = s_rl_seek_t_us[u]; }
        if (seek_u >= 0) { dt = now - best; s_rl_seek_t_us[seek_u] = 0; act = "seek_done"; }
        else             { s_rl_c_unsolicited++; act = "unsolicited"; }
    }
    if (s_rl_c_lines < FU_LOG_LIMIT) {
        s_rl_c_lines++;
        debug_log(P1009_TAG " c_event f=%d gen=%u seq=%u state=%s act=%s seek_unit_dev=%d dt_us=%lld synth_flag=%d PC=0x%06x SR=0x%04x\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), (unsigned)seq, RL_STATE_NAME[st],
                  act, seek_u, (long long)dt, s_rl_synth_after_read,
                  (unsigned)m68000_get_reg(M68K_PC), (unsigned)m68000_get_reg(M68K_SR));
    }
    p1028_cevent_t* e = &s_p1028_ring[seq % P1028_RING_N];
    memset(e, 0, sizeof(*e));
    e->seq = seq;
    e->f = g_mx68k_frame_num;
    e->pc = m68000_get_reg(M68K_PC);
    e->sr = (uint16_t)m68000_get_reg(M68K_SR);
    e->state = (uint8_t)st;
    e->act = act;
    e->next_cmd = P1028_NEXT_CMD_NONE;
    if (st == RL_RD_WAIT || st == RL_RD_SUPPLY) { e->irq_after = IRQH_IRQ[1]; return; }
    if (st == RL_RID_WAIT) { rl_rid_poll(1); e->irq_after = IRQH_IRQ[1]; return; }
    /* P1032: 書込み待ち中の'C'は結果取得の合図。ゲスト割込みは発行せず、P1028 の保留にも入れない
     * (割込みはリザルト確定時の rl_raise_int() 1回だけ、二重発行防止) */
    if (st == RL_WR_WAIT_W || st == RL_WR_WAIT_DONE || st == RL_WR_RESULT) {
        rl_wr_enter_result("C", 1);
        e->irq_after = IRQH_IRQ[1];
        return;
    }
    /* P1032: 'C'無しで結果確定した直後に遅れて届いた'C'(既存経路で処理する。計数のみ) */
    if (s_rl_wr_result_t_us != 0) {
        int64_t dt_res = rl_now_us() - s_rl_wr_result_t_us;
        s_rl_wr_result_t_us = 0;
        if (dt_res <= (int64_t)FU_WR_LATE_C_WINDOW_MS * 1000) {
            s_rl_wr_late_c++;
            if (s_rl_wr_lines < FU_LOG_LIMIT) {
                s_rl_wr_lines++;
                debug_log(P1032_TAG " late_c f=%d seq=%u state=%s since_result_us=%lld since_send_us=%lld late_C_after_result=%u\n",
                          g_mx68k_frame_num, (unsigned)seq, RL_STATE_NAME[st], (long long)dt_res,
                          (long long)(rl_now_us() - s_rl_wr_send_t_us), (unsigned)s_rl_wr_late_c);
            }
        }
    }
    /* P1053(R11): 同期手順の失敗後、次のゲストFDCコマンド開始までにゲストへ渡る(発行または発行保留の)'C'を数える */
    if (s_nm_sy_fail_pending) s_nm_sy_fail_c_fwd++;
    if (s_rl_fmt1_stray_pending) s_rl_fmt1_stray_c_fwd++;   /* P1064(F-8): fmt1 の Phase B 拒否後の迷子'C'(計数のみ) */
    /* READ完了後・次のコマンドより前の'C'は合成フラグを解除し、次の SENSE INT は装置へ(改訂2) */
    s_rl_synth_after_read = 0;
    /* P1028: 未読リザルトがある/コマンド受付途中なら、割込みハンドラにリザルトを横取りされないよう
     * 発行を見送り、そのコマンドの完了時(rl_c_irq_flush)に発行する */
    if (st == RL_RESULT || st == RL_WR_COLLECT || (s_p955_owner == P1009_OWNER_RELAY && s_rl_wrnum > 0u) || s_p955_pending) {
        if (!s_rl_c_irq_deferred) s_p1028_defer_f = g_mx68k_frame_num;
        s_rl_c_irq_deferred = 1;
        s_p1028_deferred_at_c++;
        e->deferred = 1;
        e->irq_after = IRQH_IRQ[1];
        if (s_p1028_lines < FU_LOG_LIMIT) {
            s_p1028_lines++;
            debug_log(P1028_DEFER_TAG " defer_at_c f=%d seq=%u state=%s wrnum=%u pending=%d PC=0x%06x SR=0x%04x\n",
                      g_mx68k_frame_num, (unsigned)seq, RL_STATE_NAME[st], (unsigned)s_rl_wrnum, s_p955_pending,
                      (unsigned)e->pc, (unsigned)e->sr);
        }
        return;
    }
    rl_raise_int();   /* XM6t と同じく全てゲストFDC割込みへ。FIFOには積まない */
    s_rl_c_irq_live = (IRQH_IRQ[1] && IRQH_CallBack[1] == (void*)FDC_Int) ? 1 : 0;
    e->irq_after = IRQH_IRQ[1];
}

static void rl_p2_frame_end(void)
{
    int kind = s_rl_p2_kind;
    s_rl_p2_kind = RL_P2_NONE;
    if (kind == RL_P2_TARGET) rl_read_frame_done();
    else if (kind == RL_P2_STRAY) s_rl_stray_frame++;
}

/* P1062: 真のG3発生直前(rl_nm_attr_on_ev 後・rl_abort_op(RE_MEDIA) 前)のスナップショット。読むだけ・ログ予算対象外。
 * judge_state: na=normal 以外 / pending=判別保留中(H1/H2) / unit=この 'd' で unit 判定 / all=この 'd' で全閉判定 /
 * all_other=判定手順外の全閉(unpaired_d 等)。ru0/ra0 は rl_nm_attr_on_ev 前の resolved_unit/resolved_all */
static void rl_g3_note(uint8_t b, uint32_t ru0, uint32_t ra0)
{
    const char* js;
    if (s_rl_write_mode != WRM_NORMAL)    js = "na";
    else if (s_nm_st != NM_ST_I)          js = "pending";
    else if (s_nm_at_res_unit != ru0)     js = "unit";
    else if (s_nm_at_res_all != ra0)      js = "all";
    else                                  js = "all_other";
    int overwrite = s_rl_g3_w_pending || s_rl_g3_bu_pending;
    int prev_f = s_rl_g3_pending_f;
    if (overwrite) s_rl_g3_pending_overwrite++;
    int bu_valid_before = s_rl_bringup_valid;
    s_rl_g3_pending_f = g_mx68k_frame_num;
    s_rl_g3_w_pending = 1;
    s_rl_g3_bu_pending = 1;
    s_rl_g3_bu_ambiguous_pending = bu_valid_before ? 0 : 1;
    uint8_t us = s_rl_cmd[1] & 3u;
    debug_log(P1062_TAG " abort f=%d ev=%c judge_state=%s st=%s res_unit_d=%u res_all_d=%u us=%u ue_begin=%u ue=%u,%u,%u,%u"
              " d_events=%u bringup_valid_before=%d overwrite=%d prev_pending_f=%d n=%u\n",
              g_mx68k_frame_num, b == 0x44u ? 'D' : 'd', js, NM_ST_NAME[s_nm_st],
              (unsigned)(s_nm_at_res_unit - ru0), (unsigned)(s_nm_at_res_all - ra0), (unsigned)us,
              (unsigned)s_rl_wr_ue_begin[us], (unsigned)s_nm_ue[0], (unsigned)s_nm_ue[1], (unsigned)s_nm_ue[2],
              (unsigned)s_nm_ue[3], (unsigned)s_rl_d_events, bu_valid_before, overwrite,
              overwrite ? prev_f : -1, (unsigned)s_rl_g3_abort + 1u);
}

static void rl_p2_byte(uint8_t b)
{
    switch (s_rl_p2_phase) {
    case 0:
        if (b == 0x52u) {   /* 'R' */
            s_rl_p2_phase = 1;
            if (s_rl_state == RL_RD_WAIT && !s_rl_seen_r) {
                s_rl_p2_kind = RL_P2_TARGET;
                s_rl_seen_r = 1;
                s_rl_last_rx_us = rl_now_us();
            } else {
                s_rl_p2_kind = RL_P2_STRAY;
            }
            return;
        }
        if (s_rl_state == RL_RD_WAIT && !s_rl_seen_r) s_rl_other_bytes++;
        if (rl_in_write_wait()) rl_wr_ev_add(b);   /* P1032: 書込み待ち中のイベント時系列 */
        switch (b) {
        case 0x43u: rl_on_c(); break;                                       /* 'C' FDC割込み */
        case 0x63u:                                                         /* 'c' 計数のみ */
            s_rl_lc_events++;
            if (s_rl_sync_active) s_rl_sync_cc++;                           /* P1051 */
            break;
        case 0x44u: case 0x64u: {                                           /* 'D'/'d' */
            uint32_t nm_co_before = s_rl_fi_coalesced;   /* P1055: この 'D' 自身の coalesce を判定条件(f)で検出するため先頭で退避 */
            uint8_t nm_was = (s_rl_write_mode == WRM_NORMAL) ? rl_nm_open_mask() : 0u;
            uint32_t g3_ru0 = s_nm_at_res_unit, g3_ra0 = s_nm_at_res_all;   /* P1062: 判定結果の差分取得用(読むだけ) */
            s_rl_d_events++;
            /* P1089(B-10): 装置は 'D' 送出と同時に未 flush の書込みキャッシュを破棄する(FW: app.c:1131-1134、'd' は破棄しない)。
             * 例外(N1): flush 契機が排出ビットつき $E94005 書込みだった場合、装置は flush を同期実行してから排出するので
             * (FW: app.c:2656-2658)、その後の 'D' は flush 済みとしてラッチせずクリアする(仮説、R15・H-4 で検証) */
            if (b == 0x44u && s_fu_wc_dirty) {
                if (s_fu_wc_flush_req && s_fu_wc_flush_src == WCF_E5_EJECT) {
                    if (s_fu_wc_lines < FU_WC_LOG_LIMIT) {
                        s_fu_wc_lines++;
                        debug_log(P1089_TAG " D_after_eject_flush f=%d est=%u pend_n=%u dirty_f=%d last_cached_f=%d n=%u\n",
                                  g_mx68k_frame_num, (unsigned)s_fu_wc_est, (unsigned)s_fu_wc_pend_n, s_fu_wc_dirty_f,
                                  s_fu_wc_last_cached_f, (unsigned)s_fu_wc_clr[WCC_D_EJECT] + 1u);
                    }
                    rl_wc_clear(WCC_D_EJECT);
                } else {
                    s_fu_wc_discard_D++;
                    rl_wc_discard("wcache_discard_D");
                }
            }
            /* P1053: 'D'/'d' はユニット番号を持たない。P1055: ここでは記録のみ(ue は動かさない)。どのユニットを閉じるかは
             * 組単位の判別保留(rl_nm_attr_on_ev → 走査 → 判定)が決め、判定できなければ全閉(現行 U-9 挙動) */
            if (s_rl_write_mode == WRM_NORMAL) rl_nm_fence_close(-1, b == 0x44u ? NM_CL_EV_D : NM_CL_EV_d, 0, 0, nm_was, NULL);
            rl_nm_attr_on_ev(b == 0x44u ? 'D' : 'd', nm_co_before);
            rl_invalidate_caches();
            /* P1032: データ未送信の書込み待ちは打ち切る。データ送信後は装置リザルトがその時点の真実なので待つ */
            if (s_rl_state == RL_WR_WAIT_W) {
                rl_g3_note(b, g3_ru0, g3_ra0);   /* P1062: 計測のみ */
                rl_abort_op(RE_MEDIA);
            }
            /* P1037: 武装後の 'D'/'d' はディスク交換の疑い → 永久停止(XEiJ FUK.java:961-965・XM6t fdc_bridger.h:932-940 では
             * FDD 割込みのオン/オフ。MX はゲストへの FDD 割込み転送をしていない=D-116) */
            if (s_rl_ar_armed) rl_ar_disarm(b == 0x44u ? "ev_D" : "ev_d");
            /* P1052(D-116): 'D'はFDD割込み要求をラッチする(発行は rl_fdd_int_service)。'd'は取り消さない(XEiJ のラッチ型) */
            if (b == 0x44u) {
                s_rl_D_events++;
                /* 発行待ち、または発行済み・未受理の要求があれば1回にまとめる(要求のラッチは1本、受理時に最新状態を読む) */
                if (s_rl_fdd_int_pending || s_rl_fi_inflight) s_rl_fi_coalesced++;
                else { s_rl_fi_pend_set++; s_rl_fdd_int_pending = 1; }
                /* MX独自(XM6t/XEiJ は 'D' で E5 送信記録を無効化しない): 次の $E94005 書込みを必ず装置へ送る */
                s_rl_e5_sent_valid = 0;
                rl_fi_log_ev("D");
            } else {
                s_rl_dl_events++;
                rl_fi_log_ev("d");
            }
            e5p_on_event(b == 0x44u ? 'D' : 'd');   /* P1054: 受動観測(既存処理の後・読むだけ) */
            break;
        }
        case 0x74u:                                                         /* 't' */
            s_rl_t_events++;
            if (s_rl_state == RL_RD_WAIT || s_rl_state == RL_RD_SUPPLY || rl_in_write_wait()) rl_abort_op(RE_T_EVENT);
            break;
        case 0x57u: {                                                        /* 'W' */
            s_rl_w_events++;
            /* P1032: 4条件が全て成立したときだけデータを送る。不成立の'W'へは絶対に port2 へ送らない。
             * P1059: normalモードはd_events比較ではなくc3と同一のユニット別述語で判定する(乖離修正)。
             * ★Fable5監査C-3: state!=WAIT_Wの場合(新規ゲストコマンドがs_rl_cmd[]を上書きした後に
             * 遅延到着した'W'等)、us_w以下のログフィールドは新しいコマンドのユニットを指し、本来の
             * 送信対象とは異なる参考値になる。判定自体(state不一致で正しく拒否)には影響しない */
            int is_nm_w = (s_rl_write_mode == WRM_NORMAL);
            uint8_t us_w = s_rl_cmd[1] & 3u;
            int d_ok_w = is_nm_w ? rl_nm_unit_unchanged(us_w) : (s_rl_d_events == s_rl_wr_d_begin);
            if (s_rl_state == RL_WR_WAIT_W && mx68k_floppyu_usb_generation() == s_rl_op_gen &&
                !s_rl_wr_data_sent && d_ok_w) {
                if (is_nm_w && !s_nmf_inflight && !s_rl_fmt1_inflight) {
                    /* P1089(M2): 書込みコマンド送信から 'W' 到着まで(装置キャッシュ満杯時の flush はここに現れる) */
                    int64_t dw = rl_now_us() - s_rl_wr_send_t_us;
                    if (dw > s_fu_wr_cmd_to_W_max_us) s_fu_wr_cmd_to_W_max_us = dw;
                    if (s_fu_wc_send_est >= 10u && dw > s_fu_wr_cmd_to_W_max_us_est10) s_fu_wr_cmd_to_W_max_us_est10 = dw;
                }
                rl_wr_send_data();
            } else {
                s_rl_wr_late_w++;
                /* P1059(R2): is_nm_w/d_ok_w/us/ue_begin/ue/st/d_now/d_beginを追記し、行単体でc3通過後の
                 * 真の乖離(あり得ないはずだが念のため)か、真のG3(c3評価後にD/dで状態変化)かを判別可能にする */
                debug_log(P1009_TAG " warn f=%d unexpected_write_event=0x%02x state=%s data_sent=%d late_W_ignored=%u"
                          " is_nm_w=%d d_ok_w=%d us=%u ue_begin=%u ue=%u st=%d d_now=%u d_begin=%u\n",
                          g_mx68k_frame_num, (unsigned)b, RL_STATE_NAME[s_rl_state], s_rl_wr_data_sent,
                          (unsigned)s_rl_wr_late_w, is_nm_w, d_ok_w, (unsigned)us_w,
                          (unsigned)s_rl_wr_ue_begin[us_w], (unsigned)s_nm_ue[us_w], s_nm_st,
                          (unsigned)s_rl_d_events, (unsigned)s_rl_wr_d_begin);
                if (s_rl_g3_w_pending) {   /* P1062: G3 後の最初の late_W 1回だけを紐付ける(計測のみ) */
                    s_rl_g3_w_pending = 0;
                    uint32_t dly = (uint32_t)(g_mx68k_frame_num - s_rl_g3_pending_f);
                    s_rl_g3_w_delay_n++;
                    s_rl_g3_w_delay_sum += dly;
                    if (dly > s_rl_g3_w_delay_max) s_rl_g3_w_delay_max = dly;
                    debug_log(P1062_TAG " late_W f=%d g3_f=%d delay=%u state=%s n=%u max=%u\n",
                              g_mx68k_frame_num, s_rl_g3_pending_f, (unsigned)dly, RL_STATE_NAME[s_rl_state],
                              (unsigned)s_rl_g3_w_delay_n, (unsigned)s_rl_g3_w_delay_max);
                }
                if (s_rl_ar_armed) rl_ar_disarm("late_W");   /* P1037: 残る穴①(滞留'W'の取り違え)の未知を停止へ変換 */
                rl_nm_latch("late_W");                       /* P1053: normal も同じ(armed の disarm と同義) */
            }
            break;
        }
        case 0x77u:                                                         /* 'w' */
            s_rl_w_events++;
            /* P1034: WAIT_W/WAIT_DONE に加え、先行'C'で RL_WR_RESULT に入ったが MSR 未成立のまま残っている場合も取得する
             * ('C'で取得済みなら既に RL_RESULT へ進んでおり、ここには来ない=二重取得しない) */
            if (rl_in_write_wait()) {
                rl_wr_enter_result("w", 0);
            } else {
                debug_log(P1009_TAG " warn f=%d unexpected_write_event=0x%02x state=%s\n",
                          g_mx68k_frame_num, (unsigned)b, RL_STATE_NAME[s_rl_state]);
            }
            break;
        default: s_rl_p2_unknown++; break;
        }
        return;
    case 1:
        s_rl_p2_len_lo = b;
        s_rl_p2_phase = 2;
        if (s_rl_p2_kind == RL_P2_TARGET) s_rl_last_rx_us = rl_now_us();
        return;
    case 2:
        s_rl_p2_len = (uint32_t)s_rl_p2_len_lo | ((uint32_t)b << 8);
        s_rl_p2_got = 0;
        if (s_rl_p2_kind == RL_P2_TARGET) {
            s_rl_frame_len = s_rl_p2_len;
            s_rl_last_rx_us = rl_now_us();
            if (s_rl_p2_len > FU_FRAME_CAP) {   /* 上限超過: 即異常終了、残りは読み捨て */
                s_rl_frame_overflow++;
                rl_abort_op(RE_OVERFLOW);
            }
        }
        if (s_rl_p2_len == 0) { s_rl_p2_phase = 0; rl_p2_frame_end(); }
        else s_rl_p2_phase = 3;
        return;
    default:
        if (s_rl_p2_kind == RL_P2_TARGET) {
            s_rl_frame[s_rl_p2_got] = b;   /* len<=FU_FRAME_CAP を確認済み */
            s_rl_last_rx_us = rl_now_us();
        }
        s_rl_p2_got++;
        if (s_rl_p2_got >= s_rl_p2_len) { s_rl_p2_phase = 0; rl_p2_frame_end(); }
        return;
    }
}

static void rl_check_timeouts(void)
{
    int64_t now = rl_now_us();
    if (s_rl_state == RL_RD_WAIT) {
        if (!s_rl_seen_r && now - s_rl_op_t0_us > (int64_t)FU_READ_DEADLINE_MS * 1000) rl_abort_op(RE_TIMEOUT);
        else if (s_rl_seen_r && now - s_rl_last_rx_us > (int64_t)FU_READ_STALL_MS * 1000) rl_abort_op(RE_STALL);
    } else if (s_rl_state == RL_RID_WAIT) {
        if (now - s_rl_op_t0_us > (int64_t)FU_RID_DEADLINE_MS * 1000) rl_abort_op(RE_TIMEOUT);
    } else if (s_rl_state == RL_WR_WAIT_W || s_rl_state == RL_WR_WAIT_DONE) {
        /* P1032: 装置FDCが実行フェーズにある間は装置へ問い合わせず打ち切る(XM6t の Send 中非アクセスに合わせる) */
        int64_t lim = (s_rl_state == RL_WR_WAIT_W) ? FU_WR_EVENT_DEADLINE_MS : FU_WR_DONE_DEADLINE_MS;
        if (now - s_rl_wr_state_t_us > lim * 1000) { s_rl_wr_timeouts++; rl_abort_op(RE_TIMEOUT); }
    } else if (s_rl_state == RL_WR_RESULT) {
        /* P1032: 打ち切る前に装置MSRを1回だけ読み、結果フェーズなら7Bを採用する(WP時に'C'も'w'も来ない場合の観測用) */
        if (now - s_rl_wr_state_t_us > (int64_t)FU_WR_RESULT_DEADLINE_MS * 1000 && !rl_wr_result_fetch("timeout_probe", 1) &&
            s_rl_state == RL_WR_RESULT) {
            s_rl_wr_timeouts++;
            rl_abort_op(RE_TIMEOUT);
        }
    }
    for (int u = 0; u < 4; u++) {
        if (s_rl_seek_t_us[u] && now - s_rl_seek_t_us[u] > (int64_t)FU_SEEK_C_TIMEOUT_MS * 1000) {
            s_rl_seek_c_timeout++;
            debug_log(P1009_TAG " seek_c_timeout f=%d gen=%u unit_dev=%d waited_us=%lld c_seq_at_send=%u c_seq_now=%u\n",
                      g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), u,
                      (long long)(now - s_rl_seek_t_us[u]), (unsigned)s_rl_seek_seq[u], (unsigned)s_rl_c_seq);
            s_rl_seek_t_us[u] = 0;   /* 合成はせず、ゲストの割込み待ちに任せる */
        }
    }
    /* P1010: ゲストに見せるDnBの上限監視(seek_c_timeoutとは別。'C'は来たがSENSE INTが来ない場合もここで解除)。
     * 割込みは合成しない(ゲストは古いPCNを見てRECAL/SEEKをやり直すかエラーにする) */
    for (uint8_t u = 0; u < 4u; u++) {
        if (((s_rl_dnb_mx >> u) & 1u) && s_rl_dnb_t_us[u] && now - s_rl_dnb_t_us[u] > (int64_t)FU_DNB_TIMEOUT_MS * 1000)
            rl_dnb_clear(u, "timeout", 0xffu);
    }
}

/* port2 を読み切ってから(一時停止明けの順序逆転防止)タイムアウトを判定する */
static void rl_poll(void)
{
    if (!s_p955_enabled || !s_rl_usb_on || s_rl_in_poll) return;
    s_rl_in_poll = 1;
    int conn = mx68k_floppyu_usb_is_connected();
    uint32_t gen = mx68k_floppyu_usb_generation();
    if (rl_op_needs_device() && (!conn || gen != s_rl_op_gen)) rl_abort_op(RE_DISCONNECT);
    rl_dnb_check_link(conn, gen);   /* P1010 */
    if (conn && (!s_rl_gen_seen_valid || gen != s_rl_gen_seen)) {
        rl_reset_link_state();
        s_rl_gen_seen = gen;
        s_rl_gen_seen_valid = 1;
    }
    if (conn && !mx68k_floppyu_vslice_claimed()) {
        uint8_t buf[512];
        for (;;) {
            size_t n = mx68k_floppyu_usb_port2_read(buf, sizeof(buf));
            for (size_t i = 0; i < n; i++) rl_p2_byte(buf[i]);
            if (n < sizeof(buf)) break;
        }
    }
    rl_check_timeouts();
    s_rl_in_poll = 0;
}

/* P1089(B-11): 装置の execSoftReset(FU_INIT)は書込みキャッシュを flush も消去もしない(FW: app.c:694-760)。
 * dirty なら FU_INIT の前に非キャッシュ SDS を1回(延長上限)送って flush と完了を確認する。US/HD は直近の ok_cached の
 * ユニット(s_rl_cmd には触れない、N2)。確認できなければ未確定セクタを uncertain にしてラッチする */
static void rl_wc_pre_reset(void)
{
    uint8_t st3 = 0;
    rl_wr_sds_log_t lg;
    int64_t t0 = rl_now_us();
    uint32_t est = s_fu_wc_est, pn = s_fu_wc_pend_n;
    uint8_t us_hd = s_fu_wc_last_us;
    int ok = rl_wr_sds_fresh_us(us_hd, &st3, &lg, WCF_PRE_RESET);
    int cleared = !s_fu_wc_dirty;
    if (s_fu_wc_lines < FU_WC_LOG_LIMIT) {
        s_fu_wc_lines++;
        debug_log(P1089_TAG " pre_reset f=%d us_hd=0x%02x est=%u pend_n=%u sds_ok=%d sds_fail=%s cleared=%d us=%lld\n",
                  g_mx68k_frame_num, (unsigned)us_hd, (unsigned)est, (unsigned)pn, ok, lg.fail, cleared,
                  (long long)(rl_now_us() - t0));
    }
    if (!cleared) { s_fu_wc_pre_reset_fail++; rl_wc_discard("pre_reset_flush_fail"); }
}

static void rl_bringup(void)
{
    uint32_t gen = mx68k_floppyu_usb_generation();
    if (s_fu_wc_dirty) rl_wc_pre_reset();   /* P1089(B-11): ⌘R・再 bringup の FU_INIT より前 */
    if (s_rl_bringup_poll_fail_gen != gen) {
        s_rl_bringup_poll_fail_gen = gen;
        s_rl_bringup_poll_fail_n = 0;
    }
    int64_t t0 = rl_now_us();
    /* P1067: 装置 FDC の初期化で PCN は不定になる。bringup は世代不変でも再実行されるので gen 比較では落ちない */
    rl_nmf_pcn_invalidate(-1, &s_nmf_pcn_inval_bringup);
    int r1 = rl_p1("bringup_init", FU_INIT, sizeof(FU_INIT), NULL, 0);

    /* 初期化直後の装置FDCに残る結果フェーズを読み捨て、MSR==RQM(結果フェーズ終了)を
     * 連続2回観測してから IOC/SPECIFY を送る。ゲストIPLの読み捨ては Core 側FDCへ向かい装置へ届かないため代行する */
    static const uint8_t TX_MSR[]  = { FU_REG_MSR };
    static const uint8_t TX_DATA[] = { FU_REG_DATA };
    uint8_t msr_hist[BRINGUP_HIST_MAX], drain_hist[BRINGUP_HIST_MAX];
    uint32_t polls = 0, drains = 0, rqm0 = 0, p1_fail = 0, idle_seq = 0;
    uint8_t last_msr = 0;
    int last_ret = 0;
    const char* poll_result = "skip";
    if (r1 >= 0) {
        int64_t deadline = rl_now_us() + (int64_t)BRINGUP_POLL_TIMEOUT_MS * 1000;
        poll_result = "timeout";
        for (;;) {
            if (!mx68k_floppyu_usb_is_connected() || mx68k_floppyu_usb_generation() != gen) { poll_result = "link"; break; }
            uint8_t msr = 0;
            int r = rl_p1_nofault(TX_MSR, 1, &msr, 1);
            last_ret = r;
            if (r == -1 || r == -3) { poll_result = "link"; break; }
            if (r >= 1) {
                if (polls < BRINGUP_HIST_MAX) msr_hist[polls] = msr;
                polls++;
                last_msr = msr;
                if (msr == MSR_RQM) {
                    if (++idle_seq >= 2) { poll_result = "ok"; break; }
                } else {
                    idle_seq = 0;
                    if ((msr & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
                        uint8_t d = 0;
                        int rd = rl_p1_nofault(TX_DATA, 1, &d, 1);
                        last_ret = rd;
                        if (rd == -1 || rd == -3) { poll_result = "link"; break; }
                        if (rd >= 1) {
                            if (drains < BRINGUP_HIST_MAX) drain_hist[drains] = d;
                            drains++;
                        } else {
                            p1_fail++;
                        }
                        if (rl_now_us() >= deadline) break;
                        continue;   /* 結果フェーズ読出し中は待たずに次のMSRを読む */
                    }
                    if (!(msr & MSR_RQM)) rqm0++;
                }
            } else {
                p1_fail++;
                idle_seq = 0;
            }
            if (rl_now_us() >= deadline) break;
            struct timespec ts = { 0, (long)BRINGUP_POLL_INTERVAL_MS * 1000000L };
            nanosleep(&ts, NULL);
            if (rl_now_us() >= deadline) break;
        }
    }
    int64_t poll_us = rl_now_us() - t0;
    int poll_ok = strcmp(poll_result, "ok") == 0;
    if (strcmp(poll_result, "timeout") == 0) s_rl_bringup_poll_fail_n++;
    char mh[2 * BRINGUP_HIST_MAX + 4], dh[2 * BRINGUP_HIST_MAX + 4];
    rl_hex(mh, sizeof(mh), msr_hist, polls < BRINGUP_HIST_MAX ? polls : BRINGUP_HIST_MAX, BRINGUP_HIST_MAX);
    rl_hex(dh, sizeof(dh), drain_hist, drains < BRINGUP_HIST_MAX ? drains : BRINGUP_HIST_MAX, BRINGUP_HIST_MAX);
    debug_log(P1009_TAG " bringup_poll f=%d gen=%u result=%s polls=%u drains=%u rqm0=%u p1_fail=%u last_ret=%d"
              " last_msr=0x%02x msr_seq=%s drain_seq=%s fail_n=%u/%d us=%lld\n",
              g_mx68k_frame_num, (unsigned)gen, poll_result, (unsigned)polls, (unsigned)drains, (unsigned)rqm0,
              (unsigned)p1_fail, last_ret, (unsigned)last_msr, polls ? mh : "-", drains ? dh : "-",
              (unsigned)s_rl_bringup_poll_fail_n, BRINGUP_FAIL_LIMIT, (long long)poll_us);

    int r2 = -9, r3 = 0;
    uint8_t sp[6] = { FU_WR | FU_REG_DATA, 0x03, FU_WR | FU_REG_DATA, s_rl_spec[0], FU_WR | FU_REG_DATA, s_rl_spec[1] };
    if (poll_ok) {
        r2 = rl_p1("bringup_ioc", FU_IOC, sizeof(FU_IOC), NULL, 0);
        if (s_rl_spec_valid) r3 = (r2 >= 0) ? rl_p1("bringup_specify", sp, sizeof(sp), NULL, 0) : -9;
    } else {
        r3 = -9;
    }
    char sh[2 * 6 + 4];
    rl_hex(sh, sizeof(sh), sp, s_rl_spec_valid ? sizeof(sp) : 0, 6);
    s_rl_bringup_n++;
    debug_log(P1009_TAG " bringup f=%d gen=%u tx_init=80FE40 ret_init=%d tx_ioc=93609106 ret_ioc=%d tx_specify=%s ret_specify=%d us=%lld\n",
              g_mx68k_frame_num, (unsigned)gen, r1, r2, s_rl_spec_valid ? sh : "-", r3,
              (long long)(rl_now_us() - t0));
    if (s_rl_g3_bu_pending) {   /* P1062: G3 後の最初の bringup 1回だけを紐付ける(計測のみ) */
        s_rl_g3_bu_pending = 0;
        const char* cls;
        if (s_rl_g3_bu_ambiguous_pending) { s_rl_g3_bringup_ambiguous++; cls = "ambiguous"; }
        else if (poll_ok)                 { s_rl_g3_bringup_poll_ok++;   cls = "ok"; }
        else                              { s_rl_g3_bringup_poll_fail++; cls = "fail"; }
        s_rl_g3_bu_ambiguous_pending = 0;
        debug_log(P1062_TAG " bringup f=%d g3_f=%d dt_f=%d poll_result=%s class=%s ok=%u fail=%u ambiguous=%u\n",
                  g_mx68k_frame_num, s_rl_g3_pending_f, g_mx68k_frame_num - s_rl_g3_pending_f, poll_result, cls,
                  (unsigned)s_rl_g3_bringup_poll_ok, (unsigned)s_rl_g3_bringup_poll_fail, (unsigned)s_rl_g3_bringup_ambiguous);
    }
    s_rl_cf_first = 1;   /* P1051: 初期化後の装置状態は不明(同上) */
    if (poll_ok && r1 >= 0 && r2 >= 0 && r3 >= 0) {
        s_rl_bringup_valid = 1;
        s_rl_bringup_gen = gen;
        s_rl_bringup_poll_fail_n = 0;
        s_rl_rebringup_pending = 0;   /* P1087(D): tick 経由でも inline 経由でも再 bringup が済めば予約は不要 */
    } else if (s_rl_bringup_poll_fail_n >= (uint32_t)BRINGUP_FAIL_LIMIT && !(s_rl_fault_valid && s_rl_fault_gen == gen)) {
        rl_fault("bringup_poll_limit", -2);
    }
}

static void rl_spec_capture(uint8_t b)
{
    if (!s_rl_spec_capturing) return;
    s_rl_spec[s_rl_spec_n++] = b;
    if (s_rl_spec_n < 2) return;
    s_rl_spec_capturing = 0;
    s_rl_spec_valid = 1;
    if (!rl_relay_ok()) return;
    uint8_t tx[6] = { FU_WR | FU_REG_DATA, 0x03, FU_WR | FU_REG_DATA, s_rl_spec[0], FU_WR | FU_REG_DATA, s_rl_spec[1] };
    char th[2 * 6 + 4];
    int64_t t0 = rl_now_us();
    rl_wc_note_trigger(WCF_CMD);   /* P1089: SPECIFY は flush 契機 */
    int r = rl_p1("specify", tx, sizeof(tx), NULL, 0);
    rl_hex(th, sizeof(th), tx, sizeof(tx), 6);
    debug_log(P1009_TAG " cmd f=%d gen=%u name=specify_mirror cmd=0x03 tx=%s ret=%d us=%lld\n",
              g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(), th, r, (long long)(rl_now_us() - t0));
}

/* 新規コマンド開始時の共通処理(Core:fdc.c:519-525/533-538 の「新コマンドで未読破棄」に倣う) */
static void rl_on_new_command(uint8_t data)
{
    int withdrawn = 0;
    /* P1028: IRQ1はFDC/SASI/PRT共有だが、所有者確認(IRQH_CallBack[1]==FDC_Int)と
     * 「直近の発行が中継'C'由来」(live)の両方を満たすときだけ取り下げ、他デバイス由来の保留中IRQには一切触れない */
    if (s_rl_usb_on && s_rl_c_irq_live && IRQH_IRQ[1] && IRQH_CallBack[1] == (void*)FDC_Int) {
        IRQH_IRQCallBack(1);
        if (!s_rl_c_irq_deferred) s_p1028_defer_f = g_mx68k_frame_num;
        s_rl_c_irq_deferred = 1;
        s_rl_c_irq_live = 0;
        s_p1028_withdrawn++;
        withdrawn = 1;
        if (s_p1028_lines < FU_LOG_LIMIT) {
            s_p1028_lines++;
            debug_log(P1028_DEFER_TAG " withdraw f=%d new_cmd=0x%02x state=%s PC=0x%06x SR=0x%04x withdrawn=%u cmd_start=%u\n",
                      g_mx68k_frame_num, (unsigned)data, RL_STATE_NAME[s_rl_state],
                      (unsigned)m68000_get_reg(M68K_PC), (unsigned)m68000_get_reg(M68K_SR),
                      (unsigned)s_p1028_withdrawn, (unsigned)s_p955_cmd_start);
        }
    }
    /* 直近の'C'(コマンド未着のもの)へ、このコマンドを「直後のコマンド」として記録する */
    for (uint32_t s = s_rl_c_seq, k = 0; s != 0u && k < P1028_RING_N; s--, k++) {
        p1028_cevent_t* e = &s_p1028_ring[s % P1028_RING_N];
        if (e->seq != s || e->next_cmd != P1028_NEXT_CMD_NONE) break;
        e->next_cmd = data;
        e->withdrawn = (uint8_t)withdrawn;
    }
    if (rl_in_read_phase()) rl_abort_op(RE_ABORTED);
    if (rl_in_write_wait()) rl_abort_op(RE_ABORTED);   /* P1032: 送信後の中断はラッチ(rl_abort_op 内) */
    if (s_rl_state == RL_WR_COLLECT) {
        /* P1031: dryrun は装置へ何も送っていないので物理的な後始末は不要 */
        s_rl_wrcollect_aborted++;
        s_p1100_active = 0;   /* P1100: 中断経路でもリセットし、n_total の汚染を防ぐ */
        if (s_rl_wrcollect_lines < FU_LOG_LIMIT) {
            s_rl_wrcollect_lines++;
            debug_log(P1031_TAG " aborted f=%d new_cmd=0x%02x pos=%u n=%u aborted=%u/%u\n",
                      g_mx68k_frame_num, (unsigned)data, (unsigned)s_rl_wrcollect_pos, (unsigned)s_rl_wrcollect_n,
                      (unsigned)s_rl_wrcollect_aborted, (unsigned)s_rl_wrcollect_begin);
        }
        s_rl_state = RL_IDLE;
        s_rl_rdnum = 0;
    }
    if (s_rl_state == RL_RESULT) { s_rl_state = RL_IDLE; s_rl_rdnum = 0; }
    s_rl_wrnum = 0;
    s_rl_spec_capturing = 0;
}

/* 中継側の MSR(Core式。READ/READ ID 待ちのみ XM6t fdc_bridger.h:237-239 の 0x10) */
static uint8_t rl_msr(void)
{
    if (s_rl_wrnum > 0u) return p1009_core_msr(s_rl_wrnum, 0);
    switch (s_rl_state) {
    case RL_RD_WAIT: case RL_RID_WAIT: return 0x10u;
    case RL_RD_SUPPLY:                 return p1009_core_msr(0, FU_RESULT_LEN);   /* rdnum=7 → 0xD0 */
    case RL_RESULT:                    return p1009_core_msr(0, s_rl_rdnum);
    case RL_WR_COLLECT:                return 0x90u;   /* P1031: 実行フェーズ・ホスト→FDC(XM6t/XEiJ の Command中 0x90) */
    case RL_WR_WAIT_W: case RL_WR_WAIT_DONE: case RL_WR_RESULT:
        return 0x10u;   /* P1032: 送信中(XM6t fdc_bridger.h:260-261、XEiJ FUK.java:378-379 の Send 0x10) */
    default:                           return 0x80u;
    }
}

static uint8_t rl_read_data(void)
{
    if (s_rl_state == RL_RD_SUPPLY) return rl_supply_byte();
    if (s_rl_state == RL_RESULT && s_rl_rdnum > 0u) {
        uint8_t b = s_rl_rsp[s_rl_rdptr++];
        if (--s_rl_rdnum == 0u) {
            s_rl_state = RL_IDLE;
            rl_c_irq_flush("result_read");   /* P1028: リザルト読了で再発行 */
        }
        return b;
    }
    return 0xffu;   /* Core:fdc.c:454 の初期値 */
}

/* 次のフレーム境界で、DMAでゲストメモリへ実際に書かれた n バイトを Core のアクセサで読み照合する */
static void rl_verify(void)
{
    s_rl_verify_pending = 0;
    uint32_t n = s_rl_verify_n, mar_now = DMA[0].MAR & 0xFFFFFFu;
    const char* skip = NULL;
    if (!s_rl_mar0_valid)                                  skip = "no_dma";
    else if (!(s_rl_ocr0 & 0x80u))                         skip = "not_dev_to_mem";
    else if (((mar_now - s_rl_mar0) & 0xFFFFFFu) != n)     skip = "mar_delta";
    else if (s_rl_mar0 + n > 0xC00000u)                    skip = "not_ram";   /* 副作用の無い RAM 域に限る */
    if (skip) {
        debug_log(P1009_TAG " read verify f=%d MAR0=0x%06x MAR_now=0x%06x n=%u skip=%s frame_fnv=0x%08x\n",
                  g_mx68k_frame_num, (unsigned)s_rl_mar0, (unsigned)mar_now, (unsigned)n, skip, (unsigned)s_rl_frame_fnv);
        return;
    }
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ dma_readmem24(s_rl_mar0 + i)) * 16777619u;
    debug_log(P1009_TAG " read verify f=%d MAR0=0x%06x n=%u guest_fnv=0x%08x frame_fnv=0x%08x p1008_ref=0x%08x match=%d\n",
              g_mx68k_frame_num, (unsigned)s_rl_mar0, (unsigned)n, (unsigned)h, (unsigned)s_rl_frame_fnv,
              (unsigned)FU_P1008_REF_FNV, h == s_rl_frame_fnv ? 1 : 0);
}

/* P1089(B-6): 能動的な flush 確認。dirty 中に最後の書込み送信から FU_WC_IDLE_CONFIRM_MS 書込みが途絶え、状態機械が
 * 待機中・再入中でなく、中継可能・書込みラッチ無し・bringup 有効なら、非キャッシュ SDS を1回(延長上限)送る
 * (装置のアイドル自発 flush 500ms より前に MX 主導で確定させる)。US/HD は直近の ok_cached のユニット(s_rl_cmd には
 * 触れない、N2)。この往復はエミュレーションスレッドを flush 完了まで止める(R7) */
static void rl_wc_idle_tick(void)
{
    if (!s_fu_wc_dirty) return;
    if (rl_now_us() - s_rl_wr_send_t_us < (int64_t)FU_WC_IDLE_CONFIRM_MS * 1000) return;
    if (s_rl_state != RL_IDLE || s_rl_in_poll) return;
    if (s_rl_wr_latched_off || !s_rl_bringup_valid || !rl_relay_ok() || !s_fu_wc_dirty) return;
    uint8_t st3 = 0;
    rl_wr_sds_log_t lg;
    int64_t t0 = rl_now_us();
    uint32_t est = s_fu_wc_est, pn = s_fu_wc_pend_n;
    uint8_t us_hd = s_fu_wc_last_us;
    int ok = rl_wr_sds_fresh_us(us_hd, &st3, &lg, WCF_IDLE);
    if (s_fu_wc_lines < FU_WC_LOG_LIMIT) {
        s_fu_wc_lines++;
        debug_log(P1089_TAG " idle_confirm f=%d us_hd=0x%02x est=%u pend_n=%u sds_ok=%d sds_fail=%s cleared=%d since_send_us=%lld"
                  " us=%lld n=%u\n",
                  g_mx68k_frame_num, (unsigned)us_hd, (unsigned)est, (unsigned)pn, ok, lg.fail, s_fu_wc_dirty ? 0 : 1,
                  (long long)(t0 - s_rl_wr_send_t_us), (long long)(rl_now_us() - t0), (unsigned)s_fu_wc_clr[WCC_IDLE_CONFIRM]);
    }
}

/* P1089: 設定画面の状態行用に「高速モードで受理し、まだ読み戻しで照合していないセクタ数」を数え直す */
static void rl_wc_recount_unverified(void)
{
    uint32_t n = 0;
    for (uint32_t u = 0; u < 4u; u++)
        for (uint32_t i = 0; i < FU_AR_NENT; i++) {
            const rl_nm_sec_t* e = &s_nm_map[u][i];
            if (e->written && e->cached && e->pending && !e->uncertain) n++;
        }
    __atomic_store_n(&s_nm_cached_unverified, n, __ATOMIC_RELAXED);
}

/* 毎フレーム(300フレームゲートより前)。振り分け器有効時のみ呼ばれる */
static void rl_tick(void)
{
    rl_relay_ok();
    if (s_rl_reason != NULL && strcmp(s_rl_reason, "no_bringup") == 0) {
        rl_bringup();
        rl_relay_ok();
    }
    rl_poll();
    rl_rid_poll(0);
    rl_wr_result_poll(0, "poll");   /* P1032 */
    rl_wc_idle_tick();              /* P1089(B-6) */
    if (s_nm_fast && (g_mx68k_frame_num % 30) == 0) rl_wc_recount_unverified();   /* P1089(B-12) */
    /* P1028: 完了点を通らずに保留が残った場合の安全網(発火件数は reraise_tick で可視化) */
    if (s_rl_c_irq_deferred && s_rl_state == RL_IDLE && s_rl_wrnum == 0u && !s_p955_pending) rl_c_irq_flush("tick");
    rl_fdd_int_service();   /* P1052: FDC側の保留を先に解放してから、FDD割込みを発行する */
    rl_nm_attr_tick();      /* P1055: 発行後の発行待ち状態で判別保留の打切りを判断する(normal のみ) */
    if (s_rl_verify_pending) rl_verify();
    if (s_rl_frame_active) {
        int64_t us = s_rl_blk_frame_us;
        s_rl_relay_frames++;
        s_rl_blk_hist[us < 1000 ? 0 : us < 5000 ? 1 : us < 18000 ? 2 : 3]++;
        if (us > s_rl_blk_us_max) s_rl_blk_us_max = us;
    }
    s_rl_blk_frame_us = 0;
    s_rl_frame_active = 0;
    e5p_tick();   /* P1054: 観測窓のタイムアウト判定(観測のみ) */
}

static void rl_hist_str(char* out, size_t sz, const uint32_t* h)
{
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < 32; i++)
        if (h[i] && o + 16 < sz) o += (size_t)snprintf(out + o, sz - o, "%s%d:%u", o ? "," : "", i, (unsigned)h[i]);
    if (o == 0) snprintf(out, sz, "-");
}

/* P1067: normal FORMAT の集計行。分母(case 13 到達数 seen、モード・トグル問わず)と全拒否理由(0件含む)を出す */
static void rl_nmf_log_sum(const char* why)
{
    char rs[NMF_REF_N * 34 + 8];
    size_t o = 0;
    rs[0] = '\0';
    for (int i = 0; i < NMF_REF_N && o + 34 < sizeof(rs); i++)
        o += (size_t)snprintf(rs + o, sizeof(rs) - o, "%s%s=%u", i ? " " : "", NMF_REF_NAME[i], (unsigned)s_nmf_ref[i]);
    int en = s_p955_enabled;
    debug_log(P1067_TAG " sum f=%u why=%s on=%d src=%s nmf_seen=%u %s sends=%u ok=%u nw=%u n_unexp=%u invalid=%u tracks_ok=%u"
              " verify_runs=%u verify_ok=%u verify_bad=%u verify_skipped=%u verify_at_seek=%u verify_at_fmt=%u verify_at_write=%u"
              " verify_pending=%d c_deficit_n=%u relay_lost_n=%u runs_started=%u runs_completed=%u runs_aborted=%u"
              " run_active=%d run_us=%u run_pos=%u run_n=%u,%u,%u,%u chain_ok=%d halt=%d halt_why=%s partial=%d"
              " ui_halted=%d inflight=%d pcn_rec=%u pcn_inval_seek=%u pcn_inval_sense=%u pcn_inval_bringup=%u"
              " pcn_inval_sync=%u pcn_inval_all=%u map_inval=%u wr_refused_verify=%u wr_mt=%u wr_multi=%u wr_n_large=%u"
              " c77_seen=%u c77_cand=%u absorbed_c77=%u ld_valid=%d ld_exp=write:%u,seek:%u,install:%u,halt:%u,fmt:%u"
              " latched=%d router=%d\n",
              (unsigned)g_mx68k_frame_num, why, s_nmf_on, NMF_SRC_NAME[s_nmf_src], (unsigned)s_nmf_seen, rs,
              (unsigned)s_nmf_sends, (unsigned)s_nmf_ok, (unsigned)s_nmf_nw, (unsigned)s_nmf_n_unexp, (unsigned)s_nmf_invalid,
              (unsigned)s_nmf_tracks_ok, (unsigned)s_nmf_verify_runs, (unsigned)s_nmf_verify_ok, (unsigned)s_nmf_verify_bad,
              (unsigned)s_nmf_verify_skipped, (unsigned)s_nmf_verify_at[NMF_VSRC_SEEK], (unsigned)s_nmf_verify_at[NMF_VSRC_FMT],
              (unsigned)s_nmf_verify_at[NMF_VSRC_WRITE], s_nmf_verify_pending, (unsigned)s_nmf_c_deficit_n,
              (unsigned)s_nmf_relay_lost_n, (unsigned)s_nmf_runs_started, (unsigned)s_nmf_runs_completed,
              (unsigned)s_nmf_runs_aborted, s_nmf_run_active, (unsigned)s_nmf_run_us, (unsigned)s_nmf_run_pos,
              (unsigned)s_nmf_run_n[0], (unsigned)s_nmf_run_n[1], (unsigned)s_nmf_run_n[2], (unsigned)s_nmf_run_n[3],
              s_nmf_chain_ok, s_nmf_halt, s_nmf_halt_why, s_nmf_partial,
              (s_nmf_halt || (s_rl_wr_latched_off && s_nmf_run_active)) ? 1 : 0, s_nmf_inflight,
              (unsigned)s_nmf_pcn_rec, (unsigned)s_nmf_pcn_inval_seek, (unsigned)s_nmf_pcn_inval_sense,
              (unsigned)s_nmf_pcn_inval_bringup, (unsigned)s_nmf_pcn_inval_sync, (unsigned)s_nmf_pcn_inval_all,
              (unsigned)s_nmf_map_inval, (unsigned)s_nmf_wr_refused, (unsigned)s_nmf_wr_mt, (unsigned)s_nmf_wr_multi,
              (unsigned)s_nmf_wr_nbig,
              (unsigned)s_nmf_c77_seen, (unsigned)s_nmf_c77_cand, (unsigned)s_nmf_absorbed, s_nmf_ld_valid,
              (unsigned)s_nmf_ld_exp_write, (unsigned)s_nmf_ld_exp_seek, (unsigned)s_nmf_ld_exp_install,
              (unsigned)s_nmf_ld_exp_halt, (unsigned)s_nmf_ld_exp_fmt,
              s_rl_wr_latched_off, en);
}

/* P1089(B-8): 高速モードの集計行。fast=0(安全優先)でも normal なら出す(H-1 で fast=0 ok_cached=0 p1_flush_ext=0 を確認)。
 * 分母(sends・form_*)と経路選択(fast_nosync・sds_*)を同じ行に出し、ok_cached=0 の原因を行内で区別できるようにする。
 * 派生値 wc_dirty には生値(dirty になったフレーム・直近 ok_cached のフレーム・直近のクリア理由)を併記する */
static void rl_fast_log_sum(const char* why)
{
    rl_wc_recount_unverified();
    debug_log(P1089_TAG " sum f=%u why=%s fast=%d fast_cfg=%d fast_src=%d sends=%u ok_cached=%u ok_written=%u fast_nosync=%u"
              " sds_fresh_skipped_fast=%u sds_fresh_cf_first=%u form_a=%u form_c=%u form4=%u form_other=%u"
              " wc_dirty=%d wc_flush_req=%d wc_flush_src=%s wc_est=%u wc_pend_n=%u wc_dirty_f=%d wc_last_cached_f=%d"
              " wc_last_clear=%s wc_dirty_set=%u wc_flush_req_n=%u"
              " wc_dirty_clear=rx_after_flush:%u,forma:%u,idle_confirm:%u,pre_reset:%u wc_idle_confirm=%u"
              " shutdown_dirty_logonly=%d wc_discard_D=%u wc_discard_sectors=%u wc_clear_D_eject=%u wc_gen_dirty=%u"
              " wc_pre_reset_fail=%u wc_pend_ovf=%u p1_flush_ext=%u p1_flush_ext_max_us=%lld p1_timeouts=%u"
              " wr_cmd_to_W_max_us=%lld wr_cmd_to_W_max_us_est10=%lld v_match_cached=%u v_mismatch_cached=%u"
              " cached_unverified=%u latched=%d latch_why=%s\n",
              (unsigned)g_mx68k_frame_num, why, s_nm_fast, s_rl_write_fast_cfg, s_nm_fast_src, (unsigned)s_rl_wr_sends,
              (unsigned)s_nm_ok_cached, (unsigned)s_nm_ok_written, (unsigned)s_nm_fast_nosync,
              (unsigned)s_nm_sds_skip_fast, (unsigned)s_nm_sds_cf_first, (unsigned)s_rl_sy_form_a, (unsigned)s_rl_sy_form_c,
              (unsigned)s_rl_sy_form4, (unsigned)s_rl_sy_form_other,
              s_fu_wc_dirty, s_fu_wc_flush_req, WCF_NAME[s_fu_wc_flush_src], (unsigned)s_fu_wc_est, (unsigned)s_fu_wc_pend_n,
              s_fu_wc_dirty_f, s_fu_wc_last_cached_f, s_fu_wc_last_clear < 0 ? "-" : WCC_NAME[s_fu_wc_last_clear],
              (unsigned)s_fu_wc_dirty_set, (unsigned)s_fu_wc_flush_req_n,
              (unsigned)s_fu_wc_clr[WCC_RX_AFTER_FLUSH], (unsigned)s_fu_wc_clr[WCC_FORMA],
              (unsigned)s_fu_wc_clr[WCC_IDLE_CONFIRM], (unsigned)s_fu_wc_clr[WCC_PRE_RESET],
              (unsigned)s_fu_wc_clr[WCC_IDLE_CONFIRM],
              s_fu_wc_shutdown_logonly, (unsigned)s_fu_wc_discard_D, (unsigned)s_fu_wc_discard_sectors,
              (unsigned)s_fu_wc_clr[WCC_D_EJECT], (unsigned)s_fu_wc_gen_dirty, (unsigned)s_fu_wc_pre_reset_fail,
              (unsigned)s_fu_wc_pend_ovf_n, (unsigned)s_rl_p1_flush_ext, (long long)s_rl_p1_flush_ext_max_us,
              (unsigned)s_rl_p1_timeouts, (long long)s_fu_wr_cmd_to_W_max_us, (long long)s_fu_wr_cmd_to_W_max_us_est10,
              (unsigned)s_nm_v_match_cached, (unsigned)s_nm_v_mismatch_cached, (unsigned)s_nm_cached_unverified,
              s_rl_wr_latched_off, s_nm_latch_why);
}

static void rl_log_sum(void)
{
    char hr[256], hs[256], hf[256];
    rl_hist_str(hr, sizeof(hr), s_rl_relay_hist);
    rl_hist_str(hs, sizeof(hs), s_rl_stub_hist);
    rl_hist_str(hf, sizeof(hf), s_rl_refused_hist);
    debug_log(P1009_TAG " sum f=%u gen=%u mode=%s reason=%s relay_cmd=%s stub_cmd=%s refused_cmd=%s"
              " read_begin=%u read_ok=%u err_timeout=%u err_stall=%u err_disconnect=%u err_short=%u err_overflow=%u"
              " err_aborted=%u err_t=%u err_fault=%u rid_begin=%u rid_ok=%u rid_err=%u"
              " sds_usb=%u sds_cache=%u e5_usb=%u e5_cache=%u e5_sent=%u e7_sent=%u"
              " c_events=%u/%u/%u/%u/%u/%u/%u/%u c_unsolicited=%u lc_events=%u d_events=%u t_events=%u w_events=%u stray_frame=%u"
              " p2_unknown=%u frame_overflow=%u p2_rx_overflowed=%d"
              " sense_relay=%u sense_relay_80=%u sense_core_fifo=%u sense_synth_after_read=%u st0_unit_out_of_range=%u"
              " sense_drain_limit=%u fifo_stale_stub=%u seek_c_timeout=%u ecnr_both=%u"
              " p1_calls=%u p1_timeouts=%u p1_errs=%u int_raised=%u bringup=%u fault=%u"
              " blk_lt1ms=%u blk_1_5=%u blk_5_18=%u blk_ge18=%u blk_us_max=%lld relay_frames=%u"
              " rp_e5_rd=%u rp_e5_wr=%u rp_e7_wr=%u rp_cmd_wr=%u rp_msr_rd=%u rp_total=%u retry_same_chr=%u"
              " dnb_set=%u dnb_clr_sense=%u dnb_clr_timeout=%u dnb_clr_disc=%u dnb_clr_fault=%u dnb_clr_other=%u"
              " dnb_busy_reads=%u dnb_sense_no_se=%u dnb_mask_now=0x%02x result_discarded=%u/%u"
              " rebringup_inline=%u/%u/%u\n",
              (unsigned)g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(),
              s_rl_last_mode == 1 ? "relay" : "stub", s_rl_reason ? s_rl_reason : "-", hr, hs, hf,
              (unsigned)s_rl_read_begin, (unsigned)s_rl_read_ok, (unsigned)s_rl_read_err[RE_TIMEOUT],
              (unsigned)s_rl_read_err[RE_STALL], (unsigned)s_rl_read_err[RE_DISCONNECT], (unsigned)s_rl_read_err[RE_SHORT],
              (unsigned)s_rl_read_err[RE_OVERFLOW], (unsigned)s_rl_read_err[RE_ABORTED], (unsigned)s_rl_read_err[RE_T_EVENT],
              (unsigned)s_rl_read_err[RE_FAULT], (unsigned)s_rl_rid_begin, (unsigned)s_rl_rid_ok, (unsigned)s_rl_rid_err,
              (unsigned)s_rl_sds_usb, (unsigned)s_rl_sds_cache, (unsigned)s_rl_e5_usb, (unsigned)s_rl_e5_cache,
              (unsigned)s_rl_e5_sent_n, (unsigned)s_rl_e7_sent_n,
              (unsigned)s_rl_c_events[RL_IDLE], (unsigned)s_rl_c_events[RL_RD_WAIT], (unsigned)s_rl_c_events[RL_RD_SUPPLY],
              (unsigned)s_rl_c_events[RL_RID_WAIT], (unsigned)s_rl_c_events[RL_RESULT],
              (unsigned)s_rl_c_events[RL_WR_WAIT_W], (unsigned)s_rl_c_events[RL_WR_WAIT_DONE],
              (unsigned)s_rl_c_events[RL_WR_RESULT],
              (unsigned)s_rl_c_unsolicited, (unsigned)s_rl_lc_events, (unsigned)s_rl_d_events, (unsigned)s_rl_t_events,
              (unsigned)s_rl_w_events, (unsigned)s_rl_stray_frame, (unsigned)s_rl_p2_unknown, (unsigned)s_rl_frame_overflow,
              mx68k_floppyu_usb_port2_rx_overflowed(),
              (unsigned)s_rl_sense_relay, (unsigned)s_rl_sense_relay_80, (unsigned)s_rl_sense_core_fifo,
              (unsigned)s_rl_sense_synth_after_read, (unsigned)s_rl_st0_out_of_range, (unsigned)s_rl_sense_drain_limit,
              (unsigned)s_rl_fifo_stale_stub, (unsigned)s_rl_seek_c_timeout, (unsigned)s_rl_ecnr_both,
              (unsigned)s_rl_p1_calls, (unsigned)s_rl_p1_timeouts, (unsigned)s_rl_p1_errs, (unsigned)s_rl_int_raised,
              (unsigned)s_rl_bringup_n, (unsigned)s_rl_fault_n,
              (unsigned)s_rl_blk_hist[0], (unsigned)s_rl_blk_hist[1], (unsigned)s_rl_blk_hist[2], (unsigned)s_rl_blk_hist[3],
              (long long)s_rl_blk_us_max, (unsigned)s_rl_relay_frames,
              (unsigned)s_rl_rp_e5_rd, (unsigned)s_rl_rp_e5_wr, (unsigned)s_rl_rp_e7_wr, (unsigned)s_rl_rp_cmd_wr,
              (unsigned)s_rl_rp_msr_rd, (unsigned)s_rl_rp_total, (unsigned)s_rl_retry_same_chr,
              (unsigned)s_rl_dnb_set, (unsigned)s_rl_dnb_clr_sense, (unsigned)s_rl_dnb_clr_timeout,
              (unsigned)s_rl_dnb_clr_disc, (unsigned)s_rl_dnb_clr_fault, (unsigned)s_rl_dnb_clr_other,
              (unsigned)s_rl_dnb_busy_reads, (unsigned)s_rl_dnb_sense_no_se, (unsigned)s_rl_dnb_mx,
              (unsigned)s_rl_result_discarded, (unsigned)s_p955_cmd_start,
              (unsigned)s_rl_rebringup_ok, (unsigned)s_rl_rebringup_fail, (unsigned)s_rl_rebringup_skip);   /* P1087(D): ok/fail/skip */
    /* P1052: 既存の集計行の書式は変えず、別の1行で出す */
    debug_log(P1052_TAG " sum f=%u D=%u d=%u pend_set=%u coalesced=%u raised=%u masked=%u deferred_irq1=%u"
              " deferred_cdef=%u deferred_wr=%u defer_streak_max=%u requeued=%u requeued_ext=%u inflight=%d acked=%u"
              " pending=%d reset_drop_pend=%u reset_drop_inflight=%u ack_after_reset=%u\n",
              (unsigned)g_mx68k_frame_num, (unsigned)s_rl_D_events, (unsigned)s_rl_dl_events,
              (unsigned)s_rl_fi_pend_set, (unsigned)s_rl_fi_coalesced, (unsigned)s_rl_fi_raised, (unsigned)s_rl_fi_masked,
              (unsigned)s_rl_fi_deferred_irq1, (unsigned)s_rl_fi_deferred_cdef, (unsigned)s_rl_fi_deferred_wr,
              (unsigned)s_rl_fi_streak_max, (unsigned)s_rl_fi_requeued, (unsigned)s_rl_fi_requeued_ext,
              s_rl_fi_inflight, (unsigned)s_rl_fi_acked, s_rl_fdd_int_pending,
              (unsigned)s_rl_fi_reset_drop_pend, (unsigned)s_rl_fi_reset_drop_inflight,
              (unsigned)s_rl_fi_ack_after_reset);
    if (s_rl_write_mode != WRM_OFF)
        debug_log(P1031_TAG " sum f=%u begin=%u done=%u aborted=%u in_progress=%d precond_refused=%u relay_cmd5=%u\n",
                  (unsigned)g_mx68k_frame_num, (unsigned)s_rl_wrcollect_begin, (unsigned)s_rl_wrcollect_done,
                  (unsigned)s_rl_wrcollect_aborted, s_rl_state == RL_WR_COLLECT ? 1 : 0,
                  (unsigned)s_rl_wrcollect_refused, (unsigned)s_rl_relay_hist[5]);
    char mxs[16];
    if (s_rl_write_mode != WRM_OFF)
        debug_log(P1032_TAG " sum f=%u mode=%d wp_checks=%u refused_writable=%u refused_no_media=%u refused_unknown=%u"
                  " refused_latched=%u latched=%d sends=%u/%s ev_W=%u ev_w=%u ev_C=%u ev_t=%u ev_D=%u data_sent=%u"
                  " late_W_ignored=%u late_C_after_result=%u results=%u invalid_results=%u timeouts=%u"
                  " suppressed_e5r=%u suppressed_e5w=%u suppressed_e7w=%u state=%s"
                  " g3_abort=%u g3_w_delay_n=%u g3_w_delay_sum=%u g3_w_delay_max=%u g3_bringup_ok=%u g3_bringup_fail=%u"
                  " g3_pending_overwrite=%u g3_bringup_ambiguous=%u g3_pending=%d,%d g3_pending_f=%d fmt_dryrun_n=%u\n",
                  (unsigned)g_mx68k_frame_num, s_rl_write_mode, (unsigned)s_rl_wr_checks,
                  (unsigned)s_rl_wr_ref_writable, (unsigned)s_rl_wr_ref_no_media, (unsigned)s_rl_wr_ref_unknown,
                  (unsigned)s_rl_wr_ref_latched, s_rl_wr_latched_off, (unsigned)s_rl_wr_sends, rl_wr_max_str(mxs, sizeof(mxs)),
                  (unsigned)s_rl_wr_ev_W, (unsigned)s_rl_wr_ev_w, (unsigned)s_rl_wr_ev_C, (unsigned)s_rl_wr_ev_t,
                  (unsigned)s_rl_wr_ev_D, (unsigned)s_rl_wr_data_sent_n, (unsigned)s_rl_wr_late_w,
                  (unsigned)s_rl_wr_late_c, (unsigned)s_rl_wr_results, (unsigned)s_rl_wr_invalid,
                  (unsigned)s_rl_wr_timeouts,
                  (unsigned)s_rl_wr_supp_e5r, (unsigned)s_rl_wr_supp_e5w, (unsigned)s_rl_wr_supp_e7w,
                  RL_STATE_NAME[s_rl_state],
                  (unsigned)s_rl_g3_abort, (unsigned)s_rl_g3_w_delay_n, (unsigned)s_rl_g3_w_delay_sum,
                  (unsigned)s_rl_g3_w_delay_max, (unsigned)s_rl_g3_bringup_poll_ok, (unsigned)s_rl_g3_bringup_poll_fail,
                  (unsigned)s_rl_g3_pending_overwrite, (unsigned)s_rl_g3_bringup_ambiguous,
                  s_rl_g3_w_pending, s_rl_g3_bu_pending, s_rl_g3_pending_f, (unsigned)s_rl_fmt_dryrun_n);
    if (s_rl_write_mode == WRM_FMT1) {
        /* P1064: 分母 fmt1_seen と拒否理由別の計数・送信・結果・検証を1行に出す */
        char rs[FM_REF_N * 32 + 8], mx1[16], ac1[16];
        size_t o = 0;
        rs[0] = '\0';
        for (int i = 0; i < FM_REF_N && o + 32 < sizeof(rs); i++)
            o += (size_t)snprintf(rs + o, sizeof(rs) - o, "%s%s=%u", i ? " " : "", FM_REF_NAME[i], (unsigned)s_rl_fmt1_ref[i]);
        rl_fmt1_ac_str(ac1, sizeof(ac1), 0);
        debug_log(P1064_TAG " sum f=%u fmt1_seen=%u %s sends=%u/%s spent=%d inflight=%d"
                  " hmask=0x%02x spent_mask=0x%02x chain_ok=%d halt=%d halt_why=%s ok=%u nw=%u n_unexp=%u invalid=%u"
                  " verify_runs=%u verify_ok=%u verify_bad=%u verify_skipped=%u verify_pending=%d stray_c_fwd=%u seek_n=%u"
                  " latched=%d env_ok=%d allow_unit=%d allow_ch=%s"
                  " verify_at_seek=%u verify_at_fmt=%u c_deficit_n=%u relay_lost_n=%u\n",
                  (unsigned)g_mx68k_frame_num, (unsigned)s_rl_fmt1_seen, rs, (unsigned)s_rl_wr_sends,
                  rl_wr_max_str(mx1, sizeof(mx1)), s_rl_fmt1_spent, s_rl_fmt1_inflight,
                  (unsigned)s_rl_fm_hmask, (unsigned)s_rl_fmt1_spent_hmask, s_rl_fmt1_chain_ok, s_rl_fmt1_halt,
                  s_rl_fmt1_halt_why, (unsigned)s_rl_fmt1_ok,
                  (unsigned)s_rl_fmt1_nw, (unsigned)s_rl_fmt1_n_unexp, (unsigned)s_rl_fmt1_invalid,
                  (unsigned)s_rl_fmt1_verify_runs, (unsigned)s_rl_fmt1_verify_ok, (unsigned)s_rl_fmt1_verify_bad,
                  (unsigned)s_rl_fmt1_verify_skipped, s_rl_fmt1_verify_pending, (unsigned)s_rl_fmt1_stray_c_fwd,
                  (unsigned)s_rl_fmt1_seek_n, s_rl_wr_latched_off, s_rl_fm_env_ok, s_rl_fm_unit, ac1,
                  (unsigned)s_rl_fmt1_verify_at_seek, (unsigned)s_rl_fmt1_verify_at_fmt, (unsigned)s_rl_fmt1_c_deficit_n,
                  (unsigned)s_rl_fmt1_relay_lost_n);
    }
    if (s_rl_write_mode == WRM_WRITE)
        debug_log(P1035_TAG " sum f=%u fence_ok=%d pass=%u refused_fence_env=%u refused_fence_unit=%u refused_fence_chr=%u"
                  " refused_send_max=%u ok_written=%u ok_chrn_unexpected=%u wv_pending=%d wv_pre_valid=%d"
                  " fence_reads_ok=%u verify_match=%u verify_mismatch=%u sends=%u/%u latched=%d\n",
                  (unsigned)g_mx68k_frame_num, s_rl_wf_env_ok, (unsigned)s_rl_m1_pass, (unsigned)s_rl_m1_ref_env,
                  (unsigned)s_rl_m1_ref_unit, (unsigned)s_rl_m1_ref_chr, (unsigned)s_rl_m1_ref_max,
                  (unsigned)s_rl_m1_ok_written, (unsigned)s_rl_m1_ok_chrn_unexp, s_rl_wv_pending, s_rl_wv_pre_valid,
                  (unsigned)s_rl_m1_fence_reads_ok, (unsigned)s_rl_wv_match, (unsigned)s_rl_wv_mismatch,
                  (unsigned)s_rl_wr_sends, (unsigned)FU_WR_SEND_MAX_WRITE, s_rl_wr_latched_off);
    if (s_rl_write_mode == WRM_ARMED) {
        /* P1037: verified_changed(照合表で verified=1 && changed=1 のセクタ数)が D-8-wv(暫定)への寄与 */
        uint32_t dw = 0, ver = 0, vc = 0, pend = 0, unc = 0;
        for (uint32_t i = 0; i < FU_AR_NENT; i++) {
            const rl_ar_sec_t* e = &s_rl_ar_map[i];
            if (e->uncertain) unc++;
            if (!e->written) continue;
            dw++;
            if (e->verified) { ver++; if (rl_ar_changed(e)) vc++; }
            if (e->pending) pend++;
        }
        char cy[8];
        if (s_rl_ar_env_ok) snprintf(cy, sizeof(cy), "%u-%u", s_rl_ar_cmin, s_rl_ar_cmax);
        else                snprintf(cy, sizeof(cy), "-");
        debug_log(P1037_TAG " sum f=%u fence_ok=%d cyl=%s armed=%d arm_used=%d arm_f=%d disarm=%s marker_reads=%u"
                  " arm_ref_latched=%u arm_ref_arm_used=%u arm_ref_d_during=%u arm_ref_len=%u arm_ref_marker_mismatch=%u"
                  " pass=%u refused_env=%u refused_unit=%u refused_range=%u refused_marker=%u refused_not_armed=%u"
                  " refused_send_max=%u refused_wp=%u refused_drain=%u sends=%u/%u latched=%d"
                  " ok_written=%u ok_unexpected=%u distinct_written=%u verified=%u verified_changed=%u"
                  " pending_unverified=%u uncertain=%u vmatch=%u vmismatch=%u d_events=%u ry0_sds=%u late_W=%u"
                  " disarm_calls=%u read_err_skip=%u\n",
                  (unsigned)g_mx68k_frame_num, s_rl_ar_env_ok, cy, s_rl_ar_armed, s_rl_ar_arm_used, s_rl_ar_arm_f,
                  s_rl_ar_disarm, (unsigned)s_rl_ar_marker_reads,
                  (unsigned)s_rl_ar_arm_ref[AR_REF_LATCHED], (unsigned)s_rl_ar_arm_ref[AR_REF_ARM_USED],
                  (unsigned)s_rl_ar_arm_ref[AR_REF_D_DURING], (unsigned)s_rl_ar_arm_ref[AR_REF_LEN],
                  (unsigned)s_rl_ar_arm_ref[AR_REF_MISMATCH],
                  (unsigned)s_rl_ar_pass, (unsigned)s_rl_ar_ref_env, (unsigned)s_rl_ar_ref_unit, (unsigned)s_rl_ar_ref_range,
                  (unsigned)s_rl_ar_ref_mark, (unsigned)s_rl_ar_ref_notarmed, (unsigned)s_rl_ar_ref_max,
                  (unsigned)s_rl_ar_ref_wp, (unsigned)s_rl_ar_ref_drain, (unsigned)s_rl_wr_sends,
                  (unsigned)FU_WR_SEND_MAX_ARMED, s_rl_wr_latched_off,
                  (unsigned)s_rl_ar_ok_written, (unsigned)s_rl_ar_ok_unexp, (unsigned)dw, (unsigned)ver, (unsigned)vc,
                  (unsigned)pend, (unsigned)unc, (unsigned)s_rl_ar_vmatch, (unsigned)s_rl_ar_vmismatch,
                  (unsigned)s_rl_d_events, (unsigned)s_rl_ar_ry0_sds, (unsigned)s_rl_wr_late_w,
                  (unsigned)s_rl_ar_disarm_calls, (unsigned)s_rl_ar_read_err_skip);
        /* 書込み済みセクタのうち、前回の出力以降に変化したエントリだけを出す(rl_log_sum は周期的にも呼ばれるため) */
        for (uint32_t i = 0; i < FU_AR_NENT; i++) {
            const rl_ar_sec_t* e = &s_rl_ar_map[i];
            if (!e->written) continue;
            uint32_t key = (uint32_t)e->writes + e->vmatch + e->vmismatch + e->uncertain;
            if (key == s_rl_ar_map_key[i]) continue;
            s_rl_ar_map_key[i] = key;
            char ph[16];
            if (e->pre_valid) snprintf(ph, sizeof(ph), "0x%08x", (unsigned)e->pre_fnv);
            else              snprintf(ph, sizeof(ph), "-");
            debug_log(P1037_MAP_TAG " C=%u H=%u R=%u writes=%u last_fnv=0x%08x pre_fnv=%s vmatch=%u vmismatch=%u"
                      " verified=%d changed=%d uncertain=%d pending=%d\n",
                      (unsigned)(i / (FU_AR_NHEAD * FU_AR_NSEC)), (unsigned)((i / FU_AR_NSEC) % FU_AR_NHEAD),
                      (unsigned)(i % FU_AR_NSEC + 1u), (unsigned)e->writes, (unsigned)e->last_fnv, ph,
                      (unsigned)e->vmatch, (unsigned)e->vmismatch, e->verified, rl_ar_changed(e), e->uncertain, e->pending);
        }
    }
    if (s_rl_write_mode == WRM_NORMAL) {
        /* P1053: 分母(checks・pass)と全拒否理由・柵の閉鎖/解除・連続検知・照合・同期・リザルト形を1行に出す */
        uint32_t dw = 0, pend = 0, unc = 0;
        for (uint32_t u = 0; u < 4u; u++)
            for (uint32_t i = 0; i < FU_AR_NENT; i++) {
                const rl_nm_sec_t* e = &s_nm_map[u][i];
                if (e->uncertain) unc++;
                if (e->written) { dw++; if (e->pending) pend++; }
            }
        char ua[8];
        if (s_nm_unit_state == 0)      snprintf(ua, sizeof(ua), "all");
        else if (s_nm_unit_state == 1) snprintf(ua, sizeof(ua), "%d", s_nm_unit);
        else                           snprintf(ua, sizeof(ua), "bad");
        uint32_t fce = 0;   /* P1055: 閉鎖記録の合計(新3項 attr_unit/attr_all/outside_change を含む全種別) */
        for (int k = 0; k < NM_CL_N; k++) fce += s_nm_close_ev[k];
        debug_log(P1053_TAG " sum f=%u allow_unit=%s checks=%u pass=%u ref_need_read=%u ref_attr_hold=%u ref_loop=%u ref_latched=%u ref_wp=%u"
                  " ref_no_media=%u ref_drain=%u ref_unit=%u ref_env=%u ref_unknown=%u"
                  " close_startup=%u close_install_reset=%u close_ev_D=%u close_ev_d=%u close_eject=%u close_ry0=%u close_gen=%u"
                  " close_attr_unit=%u close_attr_all=%u close_outside=%u"
                  " fence_close_ev=%u fence_open_n=%u open_refused_n=%u open_ref_st=%u open_ref_d=%u open_ref_gen=%u open_now=0x%x"
                  " loop_trip=%u loop_cnt_max=%u loop_time_reset=%u v_match=%u v_mismatch=%u v_skip_epoch=%u v_unmapped=%u"
                  " v_skip_err=%u v_skip_hold=%u distinct_written=%u pending_unverified=%u uncertain=%u ok_written=%u ok_unexpected=%u"
                  " sy_ok=%u sy_refused=%u sy_fail_c_fwd=%u sy_fence_closed=%u form_a=%u form_c=%u form4=%u form_other=%u"
                  " would_cache_n=%u sends=%u/- latched=%d latch_why=%s latch_calls=%u invalid_results=%u late_W=%u d_events=%u"
                  " se_n=%u ej_supp=%u nm_err=%u/%u nm_err_limit=%u nm_err_bits=nd:%u,de:%u,or:%u,ma:%u nm_err_rej=%u\n",
                  (unsigned)g_mx68k_frame_num, ua, (unsigned)s_rl_wr_checks, (unsigned)s_nm_pass, (unsigned)s_nm_ref_need_read,
                  (unsigned)s_nm_ref_attr_hold,
                  (unsigned)s_nm_ref_loop, (unsigned)s_nm_ref_latched, (unsigned)s_nm_ref_wp, (unsigned)s_nm_ref_no_media,
                  (unsigned)s_nm_ref_drain, (unsigned)s_nm_ref_unit, (unsigned)s_nm_ref_env, (unsigned)s_nm_ref_unknown,
                  (unsigned)s_nm_close_ev[NM_CL_STARTUP], (unsigned)s_nm_close_ev[NM_CL_INSTALL], (unsigned)s_nm_close_ev[NM_CL_EV_D],
                  (unsigned)s_nm_close_ev[NM_CL_EV_d], (unsigned)s_nm_close_ev[NM_CL_EJECT], (unsigned)s_nm_close_ev[NM_CL_RY0],
                  (unsigned)s_nm_close_ev[NM_CL_GEN], (unsigned)s_nm_close_ev[NM_CL_ATTR_UNIT],
                  (unsigned)s_nm_close_ev[NM_CL_ATTR_ALL], (unsigned)s_nm_close_ev[NM_CL_OUTSIDE], (unsigned)fce,
                  (unsigned)s_nm_open_n, (unsigned)(s_nm_open_ref[NM_OR_ST] + s_nm_open_ref[NM_OR_D] + s_nm_open_ref[NM_OR_GEN]),
                  (unsigned)s_nm_open_ref[NM_OR_ST], (unsigned)s_nm_open_ref[NM_OR_D], (unsigned)s_nm_open_ref[NM_OR_GEN],
                  (unsigned)rl_nm_open_mask(),
                  (unsigned)s_nm_loop_trip, (unsigned)s_nm_loop_cnt_max, (unsigned)s_nm_loop_time_reset,
                  (unsigned)s_nm_v_match, (unsigned)s_nm_v_mismatch, (unsigned)s_nm_v_skip_epoch, (unsigned)s_nm_v_unmapped,
                  (unsigned)s_nm_v_skip_err, (unsigned)s_nm_v_skip_hold, (unsigned)dw, (unsigned)pend, (unsigned)unc, (unsigned)s_nm_ok_written,
                  (unsigned)s_nm_ok_unexp, (unsigned)s_rl_sy_ok, (unsigned)s_rl_sy_refused, (unsigned)s_nm_sy_fail_c_fwd,
                  (unsigned)s_nm_sy_fence_closed, (unsigned)s_rl_sy_form_a, (unsigned)s_rl_sy_form_c, (unsigned)s_rl_sy_form4,
                  (unsigned)s_rl_sy_form_other, (unsigned)s_rl_sy_would_cache_n, (unsigned)s_rl_wr_sends, s_rl_wr_latched_off,
                  s_nm_latch_why, (unsigned)s_nm_latch_calls, (unsigned)s_rl_wr_invalid, (unsigned)s_rl_wr_late_w,
                  (unsigned)s_rl_d_events, (unsigned)s_rl_se_n, (unsigned)s_rl_e5_ej_supp,
                  (unsigned)s_nm_err_n, (unsigned)FU_NM_ERR_MAX, (unsigned)s_nm_err_limit,   /* P1087 */
                  (unsigned)s_nm_err_nd, (unsigned)s_nm_err_de, (unsigned)s_nm_err_or, (unsigned)s_nm_err_ma,
                  (unsigned)s_nm_err_rej);
        rl_nm_attr_log_sum();   /* P1055 */
    }
    if (s_rl_write_mode == WRM_NORMAL || s_fu_wc_dirty) rl_fast_log_sum("periodic");   /* P1089 */
    if (s_rl_write_mode == WRM_NORMAL || s_fu_wc_dirty)                                 /* P1100: P1089 sum と同じ条件 */
        debug_log(P1100_TAG " sum f=%u n_total=%u zero_calls_total=%llu active=%d\n",
                  (unsigned)g_mx68k_frame_num, (unsigned)s_p1100_n_total, s_p1100_zero_total, s_p1100_active);
    if (s_rl_write_mode == WRM_NORMAL || s_nmf_seen > 0u) rl_nmf_log_sum("periodic");   /* P1067 */
    debug_log(P1028_DEFER_TAG " sum f=%u c_seq=%u cmd_start=%u withdrawn=%u deferred_at_c=%u merged=%u"
              " reraise_result=%u reraise_seek=%u reraise_nonrelay=%u reraise_tick=%u deferred_now=%d live_now=%d irq1_now=%d\n",
              (unsigned)g_mx68k_frame_num, (unsigned)s_rl_c_seq, (unsigned)s_p955_cmd_start,
              (unsigned)s_p1028_withdrawn, (unsigned)s_p1028_deferred_at_c, (unsigned)s_p1028_merged,
              (unsigned)s_p1028_reraise_result, (unsigned)s_p1028_reraise_seek, (unsigned)s_p1028_reraise_nonrelay,
              (unsigned)s_p1028_reraise_tick, s_rl_c_irq_deferred, s_rl_c_irq_live, (int)IRQH_IRQ[1]);
    /* P1028: 前回ダンプ以降の'C'を出す。最新の1件が直後コマンド未着なら次回へ持ち越す */
    uint32_t last = s_rl_c_seq;
    if (last > s_p1028_ring_dumped_seq) {
        const p1028_cevent_t* newest = &s_p1028_ring[last % P1028_RING_N];
        if (newest->seq == last && newest->next_cmd == P1028_NEXT_CMD_NONE) last--;
    }
    if (last > s_p1028_ring_dumped_seq) {
        uint32_t from = s_p1028_ring_dumped_seq + 1u, dropped = 0;
        if (last - from + 1u > P1028_RING_N) { dropped = last - from + 1u - P1028_RING_N; from = last - P1028_RING_N + 1u; }
        debug_log(P1028_RING_TAG " dump f=%u from_seq=%u to_seq=%u dropped=%u\n",
                  (unsigned)g_mx68k_frame_num, (unsigned)from, (unsigned)last, (unsigned)dropped);
        for (uint32_t s = from; s <= last; s++) {
            const p1028_cevent_t* e = &s_p1028_ring[s % P1028_RING_N];
            if (e->seq != s) continue;
            char nc[8];
            if (e->next_cmd == P1028_NEXT_CMD_NONE) snprintf(nc, sizeof(nc), "-");
            else                                    snprintf(nc, sizeof(nc), "0x%02x", (unsigned)e->next_cmd);
            debug_log(P1028_RING_TAG " seq=%u f=%d state=%s act=%s PC=0x%06x SR=0x%04x irq_after=%u deferred=%u next_cmd=%s withdrawn=%u\n",
                      (unsigned)e->seq, e->f, RL_STATE_NAME[e->state], e->act ? e->act : "-",
                      (unsigned)e->pc, (unsigned)e->sr, (unsigned)e->irq_after, (unsigned)e->deferred, nc,
                      (unsigned)e->withdrawn);
        }
        s_p1028_ring_dumped_seq = last;
    }
    e5p_log_sum("periodic");   /* P1054 */
}

/* P1035: mode1 の柵(許可ユニット・許可セクタ)を環境変数から厳格に解析する。
 * 受理するのは「数字1〜3桁 × ちょうど4個をカンマ1文字で区切った文字列」だけ。符号・空白・空トークン・余分な文字は全て不正。
 * strtoul/atoi は先頭空白・符号を受け付けてしまうため使わず、1文字ずつ手で解析する */
static int rl_wf_parse_chrn(const char* s, uint8_t out[4])
{
    static const unsigned MAXV[4] = { 255u, 1u, 255u, 3u };   /* C, H, R, N */
    static const unsigned MINV[4] = { 0u, 0u, 1u, 0u };
    if (s == NULL) return 0;
    for (int k = 0; k < 4; k++) {
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3u) return 0;   /* 4桁以上 */
            v = v * 10u + (unsigned)(*s - '0');
            s++;
        }
        if (digits == 0u) return 0;        /* 空トークン・符号・空白で始まる */
        if (v < MINV[k] || v > MAXV[k]) return 0;
        out[k] = (uint8_t)v;
        if (k < 3) { if (*s != ',') return 0; s++; }
    }
    return *s == '\0';                     /* 末尾の余分な文字(カンマ・空白・5個目)を拒否 */
}

static void rl_wf_parse_env(void)
{
    const char* u = getenv("MX68K_FLOPPYU_WRITE_UNIT");
    const char* c = getenv("MX68K_FLOPPYU_WRITE_CHR");
    int unit_ok = (u != NULL && u[0] >= '0' && u[0] <= '3' && u[1] == '\0');   /* ちょうど1文字 '0'〜'3' */
    uint8_t chrn[4] = { 0, 0, 0, 0 };
    int chr_ok = rl_wf_parse_chrn(c, chrn);
    s_rl_wf_unit = unit_ok ? (int)(u[0] - '0') : -1;
    if (chr_ok) memcpy(s_rl_wf_chrn, chrn, 4);
    s_rl_wf_env_ok = unit_ok && chr_ok;
    char cs[24];
    if (chr_ok) snprintf(cs, sizeof(cs), "%u,%u,%u,%u", chrn[0], chrn[1], chrn[2], chrn[3]);
    else        snprintf(cs, sizeof(cs), "-");
    debug_log(P1035_TAG " env f=%d unit_env=%.16s chr_env=%.32s unit=%d unit_ok=%d chr=%s chr_ok=%d fence_ok=%d"
              " send_max=%u wr_sends=%u wr_latched=%d\n",
              g_mx68k_frame_num, u ? u : "unset", c ? c : "unset", s_rl_wf_unit, unit_ok, cs, chr_ok,
              s_rl_wf_env_ok, (unsigned)FU_WR_SEND_MAX_WRITE, (unsigned)s_rl_wr_sends, s_rl_wr_latched_off);
}

/* P1037: armed の柵(許可ユニット・許可シリンダ範囲)を環境変数から厳格に解析する。UNIT は段階1と同じ規則
 * (ちょうど1文字 '0'〜'3')。MX68K_FLOPPYU_WRITE_CHR は armed では柵として読まない(設定されていれば env 行に出すのみ) */
static void rl_ar_parse_env(void)
{
    const char* u = getenv("MX68K_FLOPPYU_WRITE_UNIT");
    const char* c = getenv("MX68K_FLOPPYU_WRITE_CYL");
    int unit_ok = (u != NULL && u[0] >= '0' && u[0] <= '3' && u[1] == '\0');
    uint8_t a = 0, b = 0;
    int cyl_ok = rl_ar_parse_cyl(c, &a, &b);
    rl_ar_marker_build();
    s_rl_ar_unit = unit_ok ? (int)(u[0] - '0') : -1;
    if (cyl_ok) { s_rl_ar_cmin = a; s_rl_ar_cmax = b; }
    s_rl_ar_env_ok = unit_ok && cyl_ok;
    char cs[8];
    if (cyl_ok) snprintf(cs, sizeof(cs), "%u-%u", a, b);
    else        snprintf(cs, sizeof(cs), "-");
    debug_log(P1037_ARM_TAG " env f=%d unit_env=%.16s cyl_env=%.16s unit=%d cyl=%s fence_ok=%d"
              " send_max=%u marker_chrn=%u,%u,%u,%u marker_fnv=0x%08x fill_fnv=0x%08x chr_env_ignored=%d"
              " armed=%d arm_used=%d disarm=%s wr_sends=%u wr_latched=%d d_events=%u\n",
              g_mx68k_frame_num, u ? u : "unset", c ? c : "unset", s_rl_ar_unit, cs, s_rl_ar_env_ok,
              (unsigned)FU_WR_SEND_MAX_ARMED, FU_AR_MARK_CHRN[0], FU_AR_MARK_CHRN[1], FU_AR_MARK_CHRN[2],
              FU_AR_MARK_CHRN[3], (unsigned)s_rl_ar_marker_fnv, (unsigned)s_rl_ar_fill_fnv,
              getenv("MX68K_FLOPPYU_WRITE_CHR") != NULL ? 1 : 0, s_rl_ar_armed, s_rl_ar_arm_used, s_rl_ar_disarm,
              (unsigned)s_rl_wr_sends, s_rl_wr_latched_off, (unsigned)s_rl_d_events);
}

/* P1064: fmt1 の柵。MX68K_FLOPPYU_WRITE_UNIT は mode1 と同じ規則(ちょうど1文字 '0'〜'3'=装置側ユニット、未設定・不正=全拒否)。
 * MX68K_FLOPPYU_FORMAT_CH は文字列がちょうど "0,0" または "0,1" の場合だけ有効(単一トラックモード、それ以外は全て不正)。
 * P1066: MX68K_FLOPPYU_FORMAT_CYL は文字列がちょうど "0-0" の場合だけ有効(複数トラックモード=C0 の2面)。
 * CH と CYL が両方設定されていたら柵は不成立(どちらを優先するかを暗黙に決めない) */
static void rl_fm_parse_env(void)
{
    const char* u = getenv("MX68K_FLOPPYU_WRITE_UNIT");
    const char* ch = getenv("MX68K_FLOPPYU_FORMAT_CH");
    const char* cy = getenv("MX68K_FLOPPYU_FORMAT_CYL");
    int unit_ok = (u != NULL && u[0] >= '0' && u[0] <= '3' && u[1] == '\0');
    int ch_ok = 0;
    uint8_t c = 0, h = 0;
    if (ch != NULL && strcmp(ch, "0,0") == 0)      { ch_ok = 1; c = 0; h = 0; }
    else if (ch != NULL && strcmp(ch, "0,1") == 0) { ch_ok = 1; c = 0; h = 1; }
    int cyl_ok = (cy != NULL && strcmp(cy, "0-0") == 0);
    int both_set = (ch != NULL && cy != NULL);
    s_rl_fm_unit = unit_ok ? (int)(u[0] - '0') : -1;
    s_rl_fm_multi = 0;
    s_rl_fm_hmask = 0;
    if (!both_set && ch_ok)       { s_rl_fm_c = c; s_rl_fm_hmask = (uint8_t)(1u << h); }
    else if (!both_set && cyl_ok) { s_rl_fm_c = 0; s_rl_fm_hmask = 0x3u; s_rl_fm_multi = 1; }
    /* send_max(rl_wr_send_max)は multi・hmask・env_ok を全て設定した後に評価する(Fable5 C-2) */
    s_rl_fm_env_ok = unit_ok && !both_set && (ch_ok || cyl_ok);
    char cs[16];
    if (ch_ok) snprintf(cs, sizeof(cs), "%u,%u", c, h);
    else       snprintf(cs, sizeof(cs), "-");
    debug_log(P1064_TAG " env f=%d unit_env=%.16s ch_env=%.16s cyl_env=%.16s multi=%d hmask=0x%02x both_set=%d"
              " unit=%d unit_ok=%d ch=%s ch_ok=%d env_ok=%d"
              " send_max=%u sends=%u latched=%d spent=%d spent_mask=0x%02x halt=%d halt_why=%s\n",
              g_mx68k_frame_num, u ? u : "unset", ch ? ch : "unset", cy ? cy : "unset", s_rl_fm_multi,
              (unsigned)s_rl_fm_hmask, both_set, s_rl_fm_unit, unit_ok, cs, ch_ok, s_rl_fm_env_ok,
              (unsigned)rl_wr_send_max(), (unsigned)s_rl_wr_sends, s_rl_wr_latched_off, s_rl_fmt1_spent,
              (unsigned)s_rl_fmt1_spent_hmask, s_rl_fmt1_halt, s_rl_fmt1_halt_why);
}

/* P1053: normal の許可ユニット(MX68K_FLOPPYU_WRITE_UNIT)。★mode1/armed とは規約が逆: 未設定=全ユニット許可、
 * ちょうど1文字 '0'〜'3'=そのユニットのみ、それ以外の設定値=全拒否(フェイルセーフ)。_CHR/_CYL は柵として読まない */
static void rl_nm_parse_env(void)
{
    const char* u = getenv("MX68K_FLOPPYU_WRITE_UNIT");
    if (u == NULL)                                                 { s_nm_unit_state = 0;  s_nm_unit = -1; }
    else if (u[0] >= '0' && u[0] <= '3' && u[1] == '\0')           { s_nm_unit_state = 1;  s_nm_unit = u[0] - '0'; }
    else                                                           { s_nm_unit_state = -1; s_nm_unit = -1; }
    debug_log(P1053_TAG " env f=%d unit_env=%.16s unit_state=%s unit=%d chr_ignored=%d cyl_ignored=%d loop_max=%u"
              " loop_reset_ms=%d send_max=- wr_sends=%u wr_latched=%d latch_why=%s d_events=%u\n",
              g_mx68k_frame_num, u ? u : "unset", s_nm_unit_state == 0 ? "all" : (s_nm_unit_state == 1 ? "one" : "invalid"),
              s_nm_unit, getenv("MX68K_FLOPPYU_WRITE_CHR") != NULL ? 1 : 0, getenv("MX68K_FLOPPYU_WRITE_CYL") != NULL ? 1 : 0,
              (unsigned)FU_WR_LOOP_MAX, (int)FU_WR_LOOP_RESET_MS, (unsigned)s_rl_wr_sends, s_rl_wr_latched_off,
              s_nm_latch_why, (unsigned)s_rl_d_events);
}

/* install(リセット)ごとの位相巻戻し。累積カウンタ・故障停止(接続世代単位)・SPECIFY最新値は保持する。
 * P1032: 書込みの送信回数(s_rl_wr_sends)とラッチ(s_rl_wr_latched_off)は保持し、ここでは決して0に戻さない */
static void rl_install_reset(void)
{
    /* P1037: 武装中のリセットは永久停止(ゲストの FAT キャッシュの前提が変わるため。再武装は MX 再起動のみ)。
     * モードを再解析する前(武装中ならモードは armed のまま)に判定する */
    if (s_rl_ar_armed) rl_ar_disarm("install_reset");
    for (int pd = 0; pd < 4; pd++) s_floppyu_present[pd] = 0;   /* P1095: install/hard reset時は未確認へ戻す */
    /* P1054: 有効判定は通信層の1箇所に集約(getenv の二重実装をしない)。ハードリセットでの遅延起動は install より前 */
    s_rl_usb_on = mx68k_floppyu_usb_is_enabled();
    /* P1054: 書込みモードは 環境変数 > 設定。環境変数が設定されていれば従来どおりその値で解析し、設定は無視する。
     * 未設定なら設定 ON=normal / OFF=書込み全拒否(normal の MX68K_FLOPPYU_WRITE_UNIT 解析は下でそのまま適用) */
    const char* w = getenv("MX68K_FLOPPYU_WRITE");
    int unsupported = 0;
    s_rl_cfg_ignored = (w != NULL && s_rl_write_cfg) ? 1 : 0;
    s_rl_mode_src = (w != NULL) ? RL_MSRC_ENV : (s_rl_write_cfg ? RL_MSRC_CONFIG : RL_MSRC_OFF);
    if (w == NULL)                       s_rl_write_mode = s_rl_write_cfg ? WRM_NORMAL : WRM_OFF;
    else if (strcmp(w, "dryrun") == 0)   s_rl_write_mode = WRM_DRYRUN;
    else if (strcmp(w, "wponly") == 0)   s_rl_write_mode = WRM_WPONLY;
    else if (strcmp(w, "1") == 0)        s_rl_write_mode = WRM_WRITE;   /* P1035 段階1(柵つき) */
    else if (strcmp(w, "armed") == 0)    s_rl_write_mode = WRM_ARMED;   /* P1037 段階2a */
    else if (strcmp(w, "normal") == 0)   s_rl_write_mode = WRM_NORMAL;  /* P1053 段階2b-1 */
    else if (strcmp(w, "fmt1") == 0)     s_rl_write_mode = WRM_FMT1;    /* P1064 単一トラック実FORMAT */
    else                                 { s_rl_write_mode = WRM_OFF; unsupported = 1; }   /* 未知値は拒否へ倒す */
    if (w != NULL)
        debug_log(P1031_TAG " env f=%d MX68K_FLOPPYU_WRITE=%s mode=%d dryrun=%d wponly=%d unsupported_value=%d usb_env=%d usb_on=%d"
                  " wr_sends=%u wr_latched=%d\n",
                  g_mx68k_frame_num, w, s_rl_write_mode, s_rl_write_mode == WRM_DRYRUN ? 1 : 0,
                  s_rl_write_mode == WRM_WPONLY ? 1 : 0, unsupported, s_rl_usb_on, s_rl_usb_on,
                  (unsigned)s_rl_wr_sends, s_rl_wr_latched_off);
    /* P1035: 柵は全モードで毎回まず無効化し、mode1 のときだけ解析する(設定なので install ごとの再解析でよい) */
    s_rl_wf_env_ok = 0; s_rl_wf_unit = -1; memset(s_rl_wf_chrn, 0, sizeof(s_rl_wf_chrn));
    if (s_rl_write_mode == WRM_WRITE) rl_wf_parse_env();
    /* P1037: armed の柵も同じく毎回まず無効化し、armed のときだけ解析する */
    s_rl_ar_env_ok = 0; s_rl_ar_unit = -1; s_rl_ar_cmin = 0; s_rl_ar_cmax = 0;
    if (s_rl_write_mode == WRM_ARMED) rl_ar_parse_env();
    /* P1064: fmt1 の柵も同じく毎回まず全拒否側へ戻し、fmt1 のときだけ解析する */
    /* P1066: 面マスク・複数モードも全拒否側へ戻す。送信済みマスク・停止ラッチ・連鎖系はプロセス寿命でリセットしない */
    s_rl_fm_env_ok = 0; s_rl_fm_unit = -1; s_rl_fm_c = 0; s_rl_fm_hmask = 0; s_rl_fm_multi = 0;
    if (s_rl_write_mode == WRM_FMT1) rl_fm_parse_env();
    /* P1053: normal の許可ユニットも毎回まず全拒否側へ戻し、normal のときだけ解析する。柵は全ユニット閉じる
     * (ラッチ・連続検知の計数は保持=プロセス起動時以外に初期化しない規律)。初回は startup として記録する */
    s_nm_unit_state = -1; s_nm_unit = -1;
    if (s_rl_write_mode == WRM_NORMAL) {
        rl_nm_parse_env();
        /* P1055(E6): 判別保留も破棄し、基準値を全ユニット無効化する(全閉の共通処理 rl_nm_attr_abort 経由) */
        uint8_t was = rl_nm_open_mask_nohold();
        rl_nm_attr_reset(NM_AR_INSTALL, s_nm_startup_logged ? NM_CL_INSTALL : NM_CL_STARTUP, 1, was);
        s_nm_startup_logged = 1;
    } else {
        memset(s_nm_fo_valid, 0, sizeof(s_nm_fo_valid));
        /* P1055: normal 以外では状態機械を動かさない。前回 normal で残った保留・基準値だけを初期状態へ戻す(ue は不変、ログなし) */
        rl_nm_hold_clear();
        memset(s_nm_base_ok, 0, sizeof(s_nm_base_ok));
    }
    /* P1067: normal FORMAT の許可。設定由来の normal かつ設定 ON、または開発・単体テスト用の env 経路
     * (MX68K_FLOPPYU_WRITE=normal のときだけ有効な MX68K_FLOPPYU_NM_FORMAT=1)。環境変数 MX68K_FLOPPYU_WRITE が
     * 設定されていれば設定値は無視する(P1054 の優先規律)。書込みが OFF なら s_rl_write_mode=WRM_OFF なので必ず 0 */
    {
        const char* nf = getenv("MX68K_FLOPPYU_NM_FORMAT");
        int nf_env = (w != NULL && strcmp(w, "normal") == 0 && nf != NULL && strcmp(nf, "1") == 0);
        if (s_rl_write_mode == WRM_NORMAL && s_rl_mode_src == RL_MSRC_CONFIG && s_rl_format_cfg) {
            s_nmf_on = 1; s_nmf_src = NMF_SRC_CONFIG;
        } else if (s_rl_write_mode == WRM_NORMAL && nf_env) {
            s_nmf_on = 1; s_nmf_src = NMF_SRC_ENV;
        } else {
            s_nmf_on = 0;
            s_nmf_src = (w != NULL && s_rl_format_cfg) ? NMF_SRC_IGNORED : NMF_SRC_OFF;
        }
        rl_nmf_pcn_invalidate(-1, &s_nmf_pcn_inval_all);
        if (s_nmf_ld_valid) { s_nmf_ld_valid = 0; s_nmf_ld_exp_install++; }   /* P1070 X3: install で完走記録を失効 */
        debug_log(P1067_TAG " env f=%d format_cfg=%d nm_format_env=%.8s write_env=%.16s mode=%d mode_src=%s on=%d src=%s"
                  " halt=%d halt_why=%s partial=%d run_active=%d run_us=%u run_pos=%u tracks_ok=%u runs_per_epoch=%u"
                  " wr_latched=%d\n",
                  g_mx68k_frame_num, s_rl_format_cfg, nf ? nf : "unset", w ? w : "unset", s_rl_write_mode,
                  RL_MSRC_NAME[s_rl_mode_src], s_nmf_on, NMF_SRC_NAME[s_nmf_src], s_nmf_halt, s_nmf_halt_why, s_nmf_partial,
                  s_nmf_run_active, (unsigned)s_nmf_run_us, (unsigned)s_nmf_run_pos, (unsigned)s_nmf_tracks_ok,
                  (unsigned)FU_NMF_RUNS_PER_EPOCH, s_rl_wr_latched_off);
        rl_nmf_log_sum("install");
    }
    /* P1089(B-1): 書込みモード「高速」は、設定由来の normal かつ設定「高速」のときだけ有効。環境変数 MX68K_FLOPPYU_WRITE が
     * 設定されていれば無視する(FORMAT の NMF_SRC_IGNORED と同じ扱い。高速用の環境変数は新設しない)。
     * 装置キャッシュの dirty はここでは落とさない(bringup の FU_INIT 前に flush を確認する、B-11) */
    s_nm_fast = (s_rl_write_mode == WRM_NORMAL && s_rl_mode_src == RL_MSRC_CONFIG && s_rl_write_fast_cfg) ? 1 : 0;
    s_nm_fast_src = s_nm_fast ? NMFAST_SRC_CONFIG : ((w != NULL && s_rl_write_fast_cfg) ? NMFAST_SRC_IGNORED : NMFAST_SRC_OFF);
    debug_log(P1089_TAG " env f=%d fast_cfg=%d fast=%d fast_src=%s mode=%d mode_src=%s write_env=%.16s wc_dirty=%d wc_est=%u"
              " wc_pend_n=%u latched=%d\n",
              g_mx68k_frame_num, s_rl_write_fast_cfg, s_nm_fast,
              s_nm_fast_src == NMFAST_SRC_CONFIG ? "config" : (s_nm_fast_src == NMFAST_SRC_IGNORED ? "ignored" : "off"),
              s_rl_write_mode, RL_MSRC_NAME[s_rl_mode_src], w ? w : "unset", s_fu_wc_dirty, (unsigned)s_fu_wc_est,
              (unsigned)s_fu_wc_pend_n, s_rl_wr_latched_off);
    if (rl_in_write_wait()) {
        /* P1032: 送信後の書込み待ちをリセットが踏んだ。装置の状態は不明なのでラッチする(Fable5 M3) */
        s_rl_fmt1_inflight = 0;   /* P1064: spent はそのまま */
        s_rl_wr_latched_off = 1;
        s_rl_bringup_valid = 0;
        rl_wr_log_end("install_reset", "unknown");
        rl_ar_after_unknown("install_reset", NULL);   /* P1037: armed のみ */
        if (s_nmf_inflight) { s_nmf_inflight = 0; rl_nmf_halt("install_reset"); }   /* P1067 */
    } else if (s_rl_state == RL_WR_COLLECT) {
        /* 装置へは未送信なのでログのみ(ラッチしない) */
        s_rl_wrcollect_aborted++;
        s_p1100_active = 0;   /* P1100: 中断経路でもリセットし、n_total の汚染を防ぐ */
        if (s_rl_wrcollect_lines < FU_LOG_LIMIT) {
            s_rl_wrcollect_lines++;
            debug_log(P1031_TAG " aborted f=%d by=install_reset pos=%u n=%u aborted=%u/%u\n",
                      g_mx68k_frame_num, (unsigned)s_rl_wrcollect_pos, (unsigned)s_rl_wrcollect_n,
                      (unsigned)s_rl_wrcollect_aborted, (unsigned)s_rl_wrcollect_begin);
        }
    }
    s_rl_state = RL_IDLE;
    s_rl_rdnum = 0; s_rl_rdptr = 0; s_rl_wrnum = 0; s_rl_cmd_n = 0;
    s_rl_bringup_valid = 0;        /* XM6t がリセットごとに Init() するのと同じ */
    s_rl_rebringup_pending = 0;    /* P1087(D): リセット後は inline 再 bringup の対象外(従来どおり tick で再 bringup、Fable5 M2) */
    s_rl_last_mode = -1;
    s_rl_last_reason = NULL;
    s_rl_synth_after_read = 0;
    s_rl_spec_capturing = 0;
    s_rl_verify_pending = 0;
    s_rl_c_irq_live = 0;           /* P1028: リセットで保留中の割込みは無効になる */
    s_rl_c_irq_deferred = 0;
    /* P1052(Fable5 C-4): FDD割込みの発行待ち・未受理もリセットで無効にする(USB再接続 rl_reset_link_state では消さない、R11) */
    s_rl_fi_reset_drop_pend     += (uint32_t)s_rl_fdd_int_pending;
    s_rl_fi_reset_drop_inflight += (uint32_t)s_rl_fi_inflight;
    s_rl_fdd_int_pending = 0;
    s_rl_fi_inflight = 0;
    s_rl_fi_streak = 0;
    if (s_rl_p2_kind == RL_P2_TARGET) s_rl_p2_kind = RL_P2_DISCARD;   /* 受信途中のフレームは読み捨て(ストリーム位相は保つ) */
    memset(s_rl_seek_t_us, 0, sizeof(s_rl_seek_t_us));
    rl_dnb_clear_all("install");   /* P1010: 振り分け器の再有効化もここを通る */
    rl_invalidate_caches();
    s_rl_e5_sent_valid = 0;
    s_rl_e7_sent_valid = 0;
}

/* ---- 振り分け規則(ドライブ非指定コマンド/レジスタ) ----
 *  Specify(cmd3)                 : Coreのみ
 *  SenseIntStatus(cmd8)          : Seek/Recal完了割込みの発行側FIFO(深さ8)の先頭側へ。空ならCore
 *  未定義コマンド(CMD_TABLE=0、≠8): Coreのみ(Coreは無視)
 *  $E94001 MSR                   : 保留中=0x90、owner=STUB→スタブMSR、それ以外Core
 *  $E94003 read                  : 保留中=0xff、owner=STUB→スタブリザルト、それ以外Core
 *  $E94005 write                 : Coreへは割当てビットを落とした値、装置用変換値は保持のみ
 *  $E94005 read                  : (core_ctrl & 0x0F)が非0のときのみCoreを読み、スタブ寄与(常に0)とOR
 *  $E94007 write                 : Coreへ常に素通し、装置用変換値は保持のみ
 *  $E94007 read                  : Core(常に0xff)
 *  DMA ReadyCB                   : 保留中またはowner=STUB→0、それ以外FDC_IsDataReady()
 *  FDC割込み線                   : スタブはFDC_SetIntと同じ発行(IOC bit7共有)
 *  P1009(中継可能時のみ。不成立なら上記のスタブ規則のまま):
 *  割当てドライブ宛てコマンド    : owner=RELAY(FIFOには積まない)
 *  SenseIntStatus(cmd8)          : FIFO先頭がCORE→Core / 中継READ DATA完了直後→0x80合成 / それ以外→装置
 *  Specify(cmd3)                 : Coreへ+装置へもミラー
 *  $E94001/$E94003/ReadyCB       : owner=RELAY→中継状態機械(rl_msr/rl_read_data/RD_SUPPLY中のみ1)
 *  $E94005 write                 : 装置用変換値が前回送信値と異なるときのみ装置へ送信
 *  $E94005 read                  : (dev_ctrl & 0x0F)が非0なら装置寄与(500msキャッシュ)をOR
 *  $E94007 write                 : 割当てドライブ選択またはモータOFFで、前回送信値と異なるとき装置へ送信 */

static int p955_core_takes_continuation(void) {
    /* Core:fdc.c:509/516 の「続きとして消費」(bufnum!=0 または wrnum!=0)を公開APIだけで判定 */
    if (FDC_IsDataReady()) return 1;
    return (FDC_Read(0xE94001u) & 0x50u) == 0x10u;   /* MSR読出しは副作用なし(Core:fdc.c:459-464) */
}

/* P1087(D): 書込み系の異常で bringup を無効化した直後、次 tick を待たずに来たゲストコマンドがスタブ NR
 * (「ディスクが入っていません」)へ落ちないよう、1回だけ inline で再 bringup して中継可否を再評価する。
 * 呼出し条件(呼出し元で確認済み): 割当てドライブ宛て・rl_relay_ok() 偽・理由 no_bringup・予約あり。
 * 順序は rl_tick() と同じ bringup→relay_ok→poll(bringup 中に port2 へ届いた事象を中継判断の前に処理する、Fable5 S1)。
 * 再入中(s_rl_in_poll)は port2 を読めないので行わず従来どおりスタブ(予約は残し tick に任せる)。
 * 予約時と USB 世代が違えば予約を捨てる(世代変化は tick 側の通常経路で扱う)。戻り値: 中継可能になったら1 */
static int p1087_rebringup_inline(void)
{
    const char* result;
    int ok = 0;
    uint32_t gen = mx68k_floppyu_usb_generation();
    if (s_rl_in_poll) {
        result = "skip_in_poll";
        s_rl_rebringup_skip++;
    } else if (s_rl_rebringup_gen != gen) {
        result = "skip_gen";
        s_rl_rebringup_pending = 0;
        s_rl_rebringup_skip++;
    } else {
        s_rl_rebringup_pending = 0;   /* 1回だけ */
        rl_bringup();
        ok = rl_relay_ok();
        rl_poll();                    /* rl_poll 自体が s_rl_in_poll を立てて再入を防ぐ */
        if (ok && !rl_relay_ok()) ok = 0;   /* poll 中に切断・故障停止等が起きた場合は中継しない */
        result = ok ? "ok" : "fail";
        if (ok) s_rl_rebringup_ok++; else s_rl_rebringup_fail++;
    }
    debug_log(P1009_TAG " rebringup_inline f=%d gen=%u trigger=%s result=%s reason_after=%s ok=%u fail=%u skip=%u\n",
              g_mx68k_frame_num, (unsigned)gen, s_rl_rebringup_trigger, result, s_rl_reason ? s_rl_reason : "-",
              (unsigned)s_rl_rebringup_ok, (unsigned)s_rl_rebringup_fail, (unsigned)s_rl_rebringup_skip);
    return ok;
}

static void p955_resolve_pending(uint32_t adr, uint8_t data) {
    uint8_t cmd = s_p955_pending_byte & 0x1Fu, us = data & 3u;
    int to_stub   = (s_p955_mask >> us) & 1u;
    int by_drvsel = (s_p955_drvsel_mx >= 0) && ((s_p955_mask >> s_p955_drvsel_mx) & 1u);
    s_p955_pending = 0;
    s_p955_us_hist[us]++;
    if (to_stub != by_drvsel) s_p955_key_disagree++;   /* 帰属キーはUS(装置側実FDCが見る値)。食い違いは計数のみ */
    int relay = to_stub && rl_relay_ok();
    if (to_stub && !relay && s_rl_rebringup_pending && s_rl_reason != NULL && strcmp(s_rl_reason, "no_bringup") == 0)
        relay = p1087_rebringup_inline();
    if (relay) {
        /* P1009: 中継可能なら実機へ(SEEK/RECAL の完了は装置の'C'で割込み、FIFOには積まない) */
        s_p955_owner = P1009_OWNER_RELAY;
        s_p955_last_us_mx = data & 7u;
        s_p955_last_us_dev = p955_us_mx_to_dev(data) & 7u;
        rl_start(s_p955_pending_byte, data);
        return;
    }
    if (to_stub) {
        s_p955_owner = P955_OWNER_STUB; s_p955_stub_cmd_count++;
        s_rl_stub_hist[cmd]++;
        s_p955_last_stub_cmd = s_p955_pending_byte; s_p955_last_us_mx = data & 7u;
        uint8_t dev = p955_us_mx_to_dev(data); s_p955_last_us_dev = dev & 7u;
        p955_stub_start(s_p955_pending_byte);
        p955_stub_param(dev);
    } else {
        s_p955_owner = P955_OWNER_CORE; s_p955_core_cmd++;
        FDC_Write(s_p955_pending_adr, s_p955_pending_byte);   /* Coreは同期処理なのでリプレイで等価 */
        FDC_Write(adr, data);
    }
    if (cmd == 7u || cmd == 15u) p955_int_fifo_push(s_p955_owner);   /* 溢れたら最古を捨て fifo_overflow++ */
    /* P1028: 中継以外(スタブ/Core)へ振り分けたコマンドは中継側の完了点を通らないため、ここで再発行する */
    rl_c_irq_flush("nonrelay");
}

static void p955_write_data(uint32_t adr, uint8_t data) {
    if (rl_in_read_phase()) s_rl_rp_cmd_wr++;
    if (s_p955_pending) { p955_resolve_pending(adr, data); return; }
    if (s_p955_owner == P1009_OWNER_RELAY && s_rl_wrnum > 0) { rl_param(data); return; }
    if (s_p955_owner == P955_OWNER_STUB && s_p955_stub_wrnum > 0) { p955_stub_param(data); return; }
    if (s_p955_owner == P955_OWNER_CORE && p955_core_takes_continuation()) {
        rl_spec_capture(data);   /* P1009: SPECIFY の続き2バイトを捕まえて装置へもミラー */
        FDC_Write(adr, data);
        return;
    }
    if (s_p955_owner == P1009_OWNER_RELAY && s_rl_state == RL_WR_COLLECT) { rl_collect_byte(data); return; }   /* P1031 */
    /* 新規コマンド開始(Core:fdc.c:519-525 と同じく、未読リザルトは破棄扱い) */
    s_p955_cmd_start++;
    s_nm_sy_fail_pending = 0;   /* P1053: 迷子'C'の計数窓はここまで */
    s_rl_fmt1_stray_pending = 0;   /* P1064: 同上(fmt1) */
    /* P1018(D-112): rl_on_new_command() が未読 RL_RESULT を破棄する瞬間を、同じ判定条件で記録する */
    if (s_rl_state == RL_RESULT) {
        s_rl_result_discarded++;
        uint32_t p1018_pc = m68000_get_reg(M68K_PC);
        uint16_t p1018_sr = (uint16_t)m68000_get_reg(M68K_SR);
        debug_log(P1009_TAG " result_discard f=%d gen=%u new_cmd=0x%02x rdnum_lost=%u PC=0x%06x SR=0x%04x discarded=%u cmd_start=%u\n",
                  g_mx68k_frame_num, (unsigned)mx68k_floppyu_usb_generation(),
                  (unsigned)data, (unsigned)s_rl_rdnum, (unsigned)p1018_pc, (unsigned)p1018_sr,
                  (unsigned)s_rl_result_discarded, (unsigned)s_p955_cmd_start);
    }
    rl_on_new_command(data);
    uint8_t cmd = data & 0x1Fu;
    if (cmd == 8u) {
        if (rl_relay_ok()) { rl_route_sense_int(adr, data); return; }
        s_rl_synth_after_read = 0;
        /* SenseIntStatus: Seek/Recal割込みの到着順FIFOの先頭側へ。空ならCore */
        int side = p955_int_fifo_pop();
        s_p955_owner = side;
        if (side == P955_OWNER_STUB) { s_p955_sense_int_stub++; s_rl_stub_hist[8]++; p955_stub_start(data); }
        else                         { s_p955_sense_int_core++; FDC_Write(adr, data); }
        rl_c_irq_flush("nonrelay");
        return;
    }
    s_rl_synth_after_read = 0;
    if (cmd == 3u || P954_CMD_TABLE[cmd] == 0u) {
        /* Specify・未定義コマンドはCoreのみ。P1009: Specify は続き2バイトを装置へもミラーする(rl_spec_capture) */
        s_p955_owner = P955_OWNER_CORE; s_p955_core_noparam++;
        if (cmd == 3u) { s_rl_spec_capturing = 1; s_rl_spec_n = 0; }
        FDC_Write(adr, data);
        rl_c_irq_flush("nonrelay");
        return;
    }
    s_p955_pending = 1; s_p955_pending_adr = adr; s_p955_pending_byte = data;   /* 第1バイト保留 */
}

/* P1009 §1-f: $E94005 の装置側送信(前回送信値と異なるときのみ。XM6t.t.log 先頭の 01→00→02→00 も全て送られる) */
static void rl_e5_write(void)
{
    if (rl_in_read_phase()) s_rl_rp_e5_wr++;
    if (!rl_relay_ok()) return;
    /* P1032: 装置FDCが書込み実行/結果フェーズにある間は装置へ送らない(後で再送されるよう送信済み記録を無効化) */
    if (rl_in_write_wait()) { s_rl_e5_sent_valid = 0; s_rl_wr_supp_e5w++; return; }
    uint8_t v = s_p955_dev_ctrl;
    uint8_t v_orig = v;   /* P1058: E5Wログ・is_eject 判定はゲストの元の指示値で行う */
    int us_match = s_rl_se_valid && (s_rl_se_us_mx == s_rl_us_mx);
    /* P1058: 書込み系の合成エラー直後の排出指示は実機へ中継しない(bit5 のみ落とし、他ビットは送る) */
    if ((v & 0x20u) && s_rl_se_valid &&
        (uint32_t)(g_mx68k_frame_num - s_rl_se_f) <= FU_EJECT_GUARD_FRAMES) {
        v &= ~0x20u;
        s_rl_e5_ej_supp++;
        /* P1058: 抑止ログ、予算対象外で必ず出す(排出関連は稀かつ診断上重要、P1057と同方針) */
        debug_log(P1058_EJGUARD_TAG " suppress f=%d v=0x%02x sent=0x%02x dt_f=%d se_st1=0x%02x"
                  " se_cmd=0x%02x se_us=0x%02x us_match=%d\n",
                  g_mx68k_frame_num, (unsigned)v_orig, (unsigned)v,
                  (int)(g_mx68k_frame_num - s_rl_se_f), (unsigned)s_rl_se_st1,
                  (unsigned)s_rl_se_cmd, (unsigned)s_rl_se_us_mx, us_match);
    }
    if (s_rl_e5_sent_valid && v == s_rl_e5_sent) return;
    uint8_t tx[2] = { FU_WR | FU_REG_E5, v };
    /* P1089: $E94005 書込みは装置キャッシュの flush 契機(FW: app.c:2657)。排出ビットは実際に送る値で判定する
     * (EJGUARD で落とした場合は排出しないので e5 扱い) */
    rl_wc_note_trigger((v & 0x20u) ? WCF_E5_EJECT : WCF_E5);
    if (rl_p1("e5_write", tx, 2, NULL, 0) < 0) return;
    /* P1057: 排出ビット(bit5)の中継は稀・診断上重要なため予算対象外で必ず出す(P1058: 抑止時も v_orig で判定) */
    int is_eject = (v_orig & 0x20u) ? 1 : 0;
    if (is_eject || s_rl_e5w_lines < FU_LOG_LIMIT) {
        if (!is_eject) s_rl_e5w_lines++;
        debug_log(P1033_E5_TAG " f=%d v=0x%02x sent=0x%02x eject_bit5=%d prev=0x%02x prev_valid=%d PC=0x%06x\n",
                  g_mx68k_frame_num, (unsigned)v_orig, (unsigned)v, is_eject,
                  (unsigned)s_rl_e5_sent, s_rl_e5_sent_valid, (unsigned)(m68000_get_reg(M68K_PC) & 0xFFFFFFu));
    }
    s_rl_e5_sent = v;
    s_rl_e5_sent_valid = 1;
    s_rl_e5_sent_n++;
    rl_invalidate_caches();   /* $E94005 読出しキャッシュと SDS キャッシュ(bit5 の排出を含む)を破棄 */
}

/* P1009 §1-g: $E94005 読出しの装置寄与。P1054: どの分岐で返したかを *src に返す(戻り値・分岐順は不変) */
static uint8_t rl_e5_read(int* src)
{
    if (rl_in_read_phase()) s_rl_rp_e5_rd++;
    if ((s_p955_dev_ctrl & 0x0Fu) == 0u) { *src = E5P_SRC_NOBIT; return 0; }
    if (!rl_relay_ok()) { *src = E5P_SRC_NORELAY; return 0; }
    /* P1032: 書込み待ち中は装置へ問い合わせず、最後のキャッシュ値(経過時間を問わない)、無ければ0 */
    if (rl_in_write_wait()) { s_rl_wr_supp_e5r++; *src = E5P_SRC_WRWAIT; return s_rl_e5c_valid ? s_rl_e5c_val : 0u; }
    int64_t now = rl_now_us();
    if (s_rl_e5c_valid && s_rl_e5c_key == s_p955_dev_ctrl && now - s_rl_e5c_t < (int64_t)FU_E5_CACHE_MS * 1000) {
        s_rl_e5_cache++;
        *src = E5P_SRC_CACHE;
        return s_rl_e5c_val;
    }
    static const uint8_t TX[] = { FU_REG_E5 };
    uint8_t v = 0;
    if (rl_p1("e5_read", TX, 1, &v, 1) < 1) { *src = E5P_SRC_USBFAIL; return 0; }
    *src = E5P_SRC_USB;
    s_rl_e5_usb++;
    s_rl_e5c_valid = 1;
    s_rl_e5c_key = s_p955_dev_ctrl;
    s_rl_e5c_val = v;
    s_rl_e5c_t = now;
    return v;
}

/* P1009 §1-h: $E94007 の装置側送信。割当てドライブ選択時とモータOFF時のみ(非割当てドライブ選択時は送らない) */
static void rl_e7_write(uint8_t data)
{
    if (rl_in_read_phase()) s_rl_rp_e7_wr++;
    if (!rl_relay_ok()) return;
    if (rl_in_write_wait()) { s_rl_e7_sent_valid = 0; s_rl_wr_supp_e7w++; return; }   /* P1032 */
    uint8_t d = data & 3u, v;
    int assigned = (s_p955_mask >> d) & 1u;
    if (data & 0x80u) {
        if (!assigned) return;
        v = (uint8_t)((data & ~3u) | (uint8_t)(d - s_p955_base));
    } else {
        v = (uint8_t)((data & 0x7Cu) | (assigned ? (uint8_t)(d - s_p955_base) : 0u));
    }
    if (s_rl_e7_sent_valid && v == s_rl_e7_sent) return;
    uint8_t tx[2] = { FU_WR | FU_REG_E7, v };
    /* P1089: $E94007 は装置側の値が変化したときだけ flush 契機(FW: app.c:2684-2687)。前回送信値が不明(無効化済み)の
     * 送信は装置側で変化しないことがあるので契機にしない(dirty を保持する安全側) */
    if (s_rl_e7_sent_valid) rl_wc_note_trigger(WCF_E7);
    if (rl_p1("e7_write", tx, 2, NULL, 0) < 0) return;
    s_rl_e7_sent = v;
    s_rl_e7_sent_valid = 1;
    s_rl_e7_sent_n++;
    memset(s_rl_sds_valid, 0, sizeof(s_rl_sds_valid));
}

static void p955_route_write(uint32_t adr, uint8_t data) {
    switch (adr & 7u) {
    case 0x03u:
        p955_write_data(adr, data);
        break;
    case 0x05u: {
        uint8_t core = data & (uint8_t)~s_p955_mask;
        s_p955_core_ctrl = core;
        s_p955_dev_ctrl  = p955_ctrl_mx_to_dev(data);
        /* P1037: ゲストの排出指示(bit5、[training knowledge])で永久停止。装置へ送るか(rl_e5_write の抑止)とは独立に判定する */
        if (s_rl_ar_armed && (s_p955_dev_ctrl & 0x20u)) rl_ar_disarm("eject_bit5");
        /* P1053: normal はゲストの排出指示で柵を閉じる(装置へ送るかとは独立)。
         * P1058: 割当てドライブ1台だけの指定ならそのユニットのみ、未割当てドライブだけの指定なら柵に触れず、
         * それ以外(複数ユニット・ドライブ指定無し)は全閉 */
        if (s_rl_write_mode == WRM_NORMAL && (s_p955_dev_ctrl & 0x20u)) {
            uint8_t dsel = s_p955_dev_ctrl & 0x0Fu;
            uint8_t msel = data & 0x0Fu;
            if (dsel == 0u && msel != 0u) {
                /* 未割当てドライブのみの排出: ふろっぴU君側の柵は無関係 */
            } else {
                uint8_t was = rl_nm_open_mask_nohold();
                if (dsel != 0u && (dsel & (uint8_t)(dsel - 1u)) == 0u) {
                    int u = 0;
                    while (!((dsel >> u) & 1u)) u++;
                    rl_nm_fence_close(u, NM_CL_EJECT, 1, 1, was, NULL);
                } else {
                    rl_nm_fence_close(-1, NM_CL_EJECT, 1, 1, was, NULL);
                }
            }
        }
        FDC_Write(adr, core);
        rl_e5_write();   /* P1009: 中継可能時のみ装置へ */
        break;
    }
    case 0x07u: {
        int32_t sel = (data & 0x80u) ? (int32_t)(data & 0x03u) : -1;
        s_p955_drvsel_mx = sel;
        if (sel >= 0 && ((s_p955_mask >> sel) & 1u))
            s_p955_dev_drvsel = (uint8_t)((data & ~3u) | (uint8_t)(sel - s_p955_base));
        else
            s_p955_dev_drvsel = data & 0x7Fu;   /* 装置側では非選択 */
        /* Core:fdc.c:552 の FDD_SetAccess(ランプ)と fdc.drv を保つため常に素通しする。
         * 割当てドライブ宛てコマンドはCoreへ行かないので、fdc.drv が割当て番号を指しても実害は無い。 */
        FDC_Write(adr, data);
        rl_e7_write(data);   /* P1009: 中継可能時のみ装置へ */
        break;
    }
    default:
        FDC_Write(adr, data);
        break;
    }
}

static uint8_t p955_route_read(uint32_t adr) {
    switch (adr & 7u) {
    case 0x01u:
        /* P1010: 実機MSRは全ドライブのDnBを同時に見せるので、STUB以外の全経路でs_rl_dnb_mxをORする */
        if (s_p955_pending) {
            if (s_rl_dnb_mx) s_rl_dnb_busy_reads++;   /* 保留中は経路別の内訳に入れず総数のみ */
            return (uint8_t)(0x90u | s_rl_dnb_mx);
        }
        if (s_p955_owner == P955_OWNER_STUB) return p955_stub_msr();   /* 中継不可時はDnB解除済みのはず */
        if (s_p955_owner == P1009_OWNER_RELAY) {
            if (rl_in_read_phase()) s_rl_rp_msr_rd++;
            if (s_rl_state == RL_RD_WAIT) s_rl_msr_wait++;
            rl_poll();
            rl_rid_poll(0);
            rl_wr_result_poll(0, "poll");   /* P1032 */
            if (s_rl_dnb_mx) {
                s_rl_dnb_busy_reads++;
                for (int u = 0; u < 4; u++) if ((s_rl_dnb_mx >> u) & 1u) s_rl_dnb_busy_relay[u]++;
            }
            return (uint8_t)(rl_msr() | s_rl_dnb_mx);
        }
        /* owner=CORE: 中継ユニットのシーク中にCore側SENSE INTでownerが移っても'C'受信を遅らせない */
        if (s_rl_dnb_mx) {
            rl_poll();
            if (s_rl_dnb_mx) {
                s_rl_dnb_busy_reads++;
                for (int u = 0; u < 4; u++) if ((s_rl_dnb_mx >> u) & 1u) s_rl_dnb_busy_core[u]++;
            }
        }
        return (uint8_t)(FDC_Read(adr) | s_rl_dnb_mx);
    case 0x03u:
        if (s_p955_pending) return 0xffu;
        if (s_p955_owner == P955_OWNER_STUB) return p955_stub_read_data();
        if (s_p955_owner == P1009_OWNER_RELAY) { rl_poll(); return rl_read_data(); }
        return FDC_Read(adr);
    case 0x05u: {
        /* スタブ寄与(Not Ready固定)は常に0。中継可能時は装置寄与 */
        uint8_t core_part = 0x00u;
        int src = E5P_SRC_NOBIT;
        if (s_p955_core_ctrl & 0x0Fu) core_part = FDC_Read(adr);
        uint8_t dev_part = rl_e5_read(&src);
        rl_nm_attr_on_e5_read(dev_part, src);    /* P1055: 判別保留の走査/保留外の変化検出(normal のみ、戻り値は不変) */
        e5p_on_read(core_part, dev_part, src);   /* P1054: 戻り値確定後に読むだけ */
        /* P1095(D-127): present表示用。P1055のrl_nm_attr_on_e5_readと同じeligible条件
         * (単一割当てユニット・USB新鮮読出しのみ)を満たす場合だけ、実際に読んだbit7を反映する。
         * Core FDC_Read case5(FDD_IsReady)と同じ意味(bit7=0x80=媒体あり)。既存の安全柵・
         * s_nm_*状態には一切触れない(読み取るだけ) */
        {
            uint8_t key = s_p955_dev_ctrl & 0x0Fu;
            uint8_t pa = rl_nm_assigned_keys();
            if (e5p_single_bit(key) && (key & pa) != 0u && src == E5P_SRC_USB) {
                int pu = rl_nm_key_unit(key);
                int mxd = fu_status_dev_to_mx((uint8_t)pu);
                int newval = (dev_part & 0x80u) ? 1 : 0;
                if (s_floppyu_present[mxd] != newval) {
                    debug_log(P1095_TAG " present_change f=%d mxd=%d dev_part=0x%02x new=%d e7_sent=0x%02x\n",
                              g_mx68k_frame_num, mxd, (unsigned)dev_part, newval, (unsigned)s_rl_e7_sent);
                }
                s_floppyu_present[mxd] = newval;
            }
        }
        return (uint8_t)(core_part | dev_part);
    }
    default:
        return FDC_Read(adr);
    }
}

static int32_t p955_route_ready(void) {
    if (s_p955_pending || s_p955_owner == P955_OWNER_STUB) return 0;
    if (s_p955_owner == P1009_OWNER_RELAY) {
        if (s_rl_state == RL_RD_WAIT) rl_poll();   /* DMA 有効中は毎ラインの受信機会 */
        return (s_rl_state == RL_RD_SUPPLY || s_rl_state == RL_WR_COLLECT) ? 1 : 0;
    }
    return FDC_IsDataReady();
}

static uint8_t mx68k_floppyu_fdc_pt_read(uint32_t adr)
{
    s_p949_read_count++;
    if (s_p955_enabled) return p955_route_read(adr);
    return FDC_Read(adr);
}

static void mx68k_floppyu_fdc_pt_write(uint32_t adr, uint8_t data)
{
    s_p949_write_count++;
    if ((adr & 7u) == 0x07u) {
        /* Core:fdc.c:551-552 の複写 */
        s_p954_drvsel = (data & 0x80u) ? (int32_t)(data & 0x03u) : -1;
    } else if ((adr & 7u) == 0x03u) {
        if (s_p954_wrnum > 0) {
            if (s_p954_us_pending) {
                uint32_t us = data & 3u;
                if ((int32_t)us == s_p954_drvsel) s_p954_us_match_count++;
                else                               s_p954_us_mismatch_count++;
                s_p954_us_pending = 0;
            }
            s_p954_wrnum--;
        } else {
            uint8_t cmd = data & 0x1Fu;
            s_p954_cmd_total++;
            s_p954_wrnum = P954_CMD_TABLE[cmd];
            if (cmd == 5u || cmd == 9u || cmd == 13u || cmd == 17u || cmd == 25u || cmd == 29u) {
                /* Write方向データフェーズを持つコマンド — 誤認識の場合もあるため
                 * 「真の開始回数」ではなく「この値を持つコマンド開始として解釈した回数」。 */
                s_p954_write_phase_cmd_count++;
            }
            if (cmd == 3u) {
                s_p954_specify_count++;
                s_p954_us_pending = 0;
            } else if (s_p954_wrnum == 0) {
                if (cmd == 8u) s_p954_sense_int_status_count++;
                else           s_p954_other_no_param_count++;
                s_p954_us_pending = 0;
            } else {
                s_p954_us_pending = 1;
            }
        }
    }
    if (s_p955_enabled) p955_route_write(adr, data);
    else                FDC_Write(adr, data);
}

static int32_t mx68k_floppyu_fdc_pt_ready(void)
{
    s_p949_ready_count++;
    s_p951_ready_ocr_count[DMA[0].OCR & 3u]++;
    s_p952_ready_dir_count[(DMA[0].OCR & 0x80u) ? 1u : 0u]++;
    if (s_p955_enabled) return p955_route_ready();
    return FDC_IsDataReady();
}

void mx68k_floppyu_fdcpt_observe_dma0_post(void)
{
    if (!s_p949_enabled) return;
    s_p951_dma0_exec_count++;
    if ((IOC_IntStat & 0x80u) && DMA[0].MTC != 0u) {
        s_p951_irq_before_drain_count++;   /* 割込み済みなのに MTC 未消化 = R1 懸念の実例 */
    }
    /* P1100: 収集中の DMA_Exec(0) 1回あたりの MTC 減少量を数える(読取りのみ) */
    if (s_p1100_active) {
        s_p1100_calls++;
        uint16_t mtc_now = DMA[0].MTC;
        uint16_t delta = (uint16_t)(s_p1100_last_mtc - mtc_now);
        if (delta == 0) s_p1100_calls_zero++; else s_p1100_calls_moved++;
        if (delta > s_p1100_max_delta) s_p1100_max_delta = delta;
        s_p1100_last_mtc = mtc_now;
        if (s_rl_state != RL_WR_COLLECT) {
            /* 収集完了後の最初の呼出し(最後の1バイトの差分を含めるため、ここで集計行を出す) */
            s_p1100_n_total++;
            s_p1100_zero_total += (unsigned long long)s_p1100_calls_zero;
            if (s_p1100_lines < 32) {
                s_p1100_lines++;
                int frames = s_p1100_fn - s_p1100_f0;
                int lines_moved = s_p1100_vln - s_p1100_vl0;   /* フレーム境界を跨ぐと負値になり得る、解釈時に注意 */
                debug_log(P1100_TAG " coll f0=%d vl0=%d f1=%d vl1=%d fn=%d vln=%d frames=%d lines=%d "
                          "calls=%d moved=%d zero=%d max_delta=%u ccr0=0x%02x csr0=0x%02x mtc0=%u fmt=%d\n",
                          s_p1100_f0, s_p1100_vl0, s_p1100_f1, s_p1100_vl1, s_p1100_fn, s_p1100_vln,
                          frames, lines_moved, s_p1100_calls, s_p1100_calls_moved, s_p1100_calls_zero,
                          (unsigned)s_p1100_max_delta, (unsigned)s_p1100_ccr0, (unsigned)s_p1100_csr0,
                          (unsigned)s_p1100_mtc0, s_p1100_is_fmt);
            }
            s_p1100_active = 0;
        }
    }
}

/* P1054: install 1回につき1行(上限 FU_LOG_LIMIT)。設定由来の normal を確認できる唯一の行 */
static void rl_cfg_log(const char* site, int cfg_enabled, int32_t assigned_base)
{
    if (s_rl_cfg_lines >= FU_LOG_LIMIT) return;
    s_rl_cfg_lines++;
    const char* w = getenv("MX68K_FLOPPYU_WRITE");
    int m = s_p955_enabled ? s_rl_write_mode : WRM_OFF;
    debug_log(P1054_CFG_TAG " install f=%d why=%s floppyu_enabled=%d router=%d base=%d usb_on=%d usb_src=%s write_cfg=%d"
              " write_env=\"%.16s\" mode=%d mode_src=%s cfg_ignored=%d latched=%d fast_cfg=%d fast=%d fast_src=%d\n",
              g_mx68k_frame_num, site ? site : "(null)", cfg_enabled ? 1 : 0, s_p955_enabled, (int)assigned_base,
              mx68k_floppyu_usb_is_enabled(), mx68k_floppyu_usb_enable_src(), s_rl_write_cfg, w ? w : "(unset)", m,
              s_p955_enabled ? RL_MSRC_NAME[s_rl_mode_src] : "router_off", s_p955_enabled ? s_rl_cfg_ignored : 0,
              s_rl_wr_latched_off, s_rl_write_fast_cfg, s_p955_enabled ? s_nm_fast : 0,   /* P1089 */
              s_p955_enabled ? s_nm_fast_src : NMFAST_SRC_OFF);
}

void mx68k_floppyu_fdcpt_install(const char *site, int enabled, int32_t assigned_base, int ext_fdd_wired)
{
    /* P1054: 観測窓の強制終端(観測のみ)。振り分け器を無効へ戻す install でも閉じる(以後 rl_tick が回らないため) */
    e5p_close("install");
    /* P958: 設定値から毎回再計算する(ラッチ無し)。判定の生入力は配線行に全て出す */
    int effective;
    const char *reason;
    if (!enabled) {
        effective = 0; reason = "off";
    } else if (assigned_base != 0 && assigned_base != 2) {
        effective = 0; reason = "bad_base";
    } else if (assigned_base == 2 && !ext_fdd_wired) {
        effective = 0; reason = "ext_fdd_off";
    } else {
        effective = 1; reason = "on";
    }

    /* P958: 元値の保存。表が自分の関数を指していないときだけ取り込む(read/write個別)。
     * 0x4A の初期値は素の FDC_Read ではなく P939 観測ラッパーなので、OFF時は必ずこの保存値を
     * 書き戻すこと(素の FDC_Read/FDC_Write へ直接戻すと観測ラッパーが外れる)。 */
    if (MemReadTable[P949_FDC_TABLE_IDX]  != mx68k_floppyu_fdc_pt_read)
        s_p958_orig_read  = MemReadTable[P949_FDC_TABLE_IDX];
    if (MemWriteTable[P949_FDC_TABLE_IDX] != mx68k_floppyu_fdc_pt_write)
        s_p958_orig_write = MemWriteTable[P949_FDC_TABLE_IDX];

    s_p949_enabled = effective;
    s_p955_enabled = effective;
    const char *table;

    if (effective) {
        s_p955_mask = (uint8_t)(0x3u << (unsigned)assigned_base);
        s_p955_base = (uint8_t)assigned_base;
        /* 番号変換の前提(mask 内の全 d が base <= d <= base+1)を検査 */
        for (uint8_t d = 0; d < 4u; d++) {
            if (((s_p955_mask >> d) & 1u) && (d < s_p955_base || d > (uint8_t)(s_p955_base + 1u))) {
                s_p955_enabled = 0;
            }
        }
        debug_log("[P949-FDCPT] enabled=1\n");
        if (s_p955_enabled) {
            debug_log("[P955-ROUTER] enabled assigned=%d mask=0x%x base=%u\n",
                      (int)assigned_base, (unsigned)s_p955_mask, (unsigned)s_p955_base);
        } else {
            debug_log("[P955-ROUTER] mask/base precondition violated mask=0x%x base=%u router disabled\n",
                      (unsigned)s_p955_mask, (unsigned)s_p955_base);
        }

        /* P954: 位相変数のみリセット時に巻き戻す(累積カウンタは P509/P686 同様に保持) */
        s_p954_wrnum      = 0;
        s_p954_us_pending = 0;
        s_p954_drvsel     = 0;   /* Core:fdc.c FDC_Init() の memset により fdc.drv=0 */

        /* P955: 振り分け器の位相状態を巻き戻す(累積カウンタは P509/P686/P954 同様に保持) */
        if (s_p955_enabled) {
            s_p955_owner      = P955_OWNER_CORE;
            s_p955_pending    = 0;
            s_p955_pending_adr  = 0;
            s_p955_pending_byte = 0;
            s_p955_drvsel_mx  = 0;
            /* P68 のリセット時プリセット $E94005=0x01(Bridge:EmulatorBridge.c:3049)は振り分け器を
             * 通らず Core へ入るため、Core の ctrl を割当てビットを落とした値と見なす。割当てが
             * ドライブ0のときは0となり、IPLROM の $E94005 読出しで Core 側ドライブ0の Ready が
             * 漏れない。P68 が無効でも Core ctrl は0なので「Coreを読まない」判定は結果的に正しい。 */
            s_p955_core_ctrl  = 0x01u & (uint8_t)~s_p955_mask;
            s_p955_dev_ctrl   = 0;
            s_p955_dev_drvsel = 0;
            s_p955_int_head   = 0;
            s_p955_int_count  = 0;
            s_p955_stub_cmd   = 0;
            memset(s_p955_stub_prm, 0, sizeof(s_p955_stub_prm));
            memset(s_p955_stub_rsp, 0, sizeof(s_p955_stub_rsp));
            s_p955_stub_wrptr = 0; s_p955_stub_wrnum = 0;
            s_p955_stub_rdptr = 0; s_p955_stub_rdnum = 0;
            s_p955_stub_int_st0 = 0x80;
            rl_install_reset();   /* P1009: ブリングアップ無効化(次のフレーム境界で再送)・位相巻戻し */
            s_e5p_seen = 1;       /* P1054: shutdown 時に累積要約を出す */
        }

        MemReadTable[P949_FDC_TABLE_IDX]  = mx68k_floppyu_fdc_pt_read;
        MemWriteTable[P949_FDC_TABLE_IDX] = mx68k_floppyu_fdc_pt_write;
        /* DMA_Init() が IsReady[0] を FDC_IsDataReady へ戻すため、毎回再登録する */
        DMA_SetReadyCB(0, mx68k_floppyu_fdc_pt_ready);
        table = "pt";
        debug_log("[P949-FDCPT] install site=%s f=%d\n",
                  site ? site : "(null)", g_mx68k_frame_num);
    } else {
        /* 前回差し替えた分だけ、保存済み元値へ read/write 個別に戻す */
        int restored = 0;
        if (MemReadTable[P949_FDC_TABLE_IDX] == mx68k_floppyu_fdc_pt_read && s_p958_orig_read != NULL) {
            MemReadTable[P949_FDC_TABLE_IDX] = s_p958_orig_read;
            restored = 1;
        }
        if (MemWriteTable[P949_FDC_TABLE_IDX] == mx68k_floppyu_fdc_pt_write && s_p958_orig_write != NULL) {
            MemWriteTable[P949_FDC_TABLE_IDX] = s_p958_orig_write;
            restored = 1;
        }
        table = restored ? "restored" : "orig";
        /* ReadyCB(0) は触らない: DMA_Init() が IsReady[0] を FDC_IsDataReady へ毎回戻し、
         * 直前の mx68k_fdlat_install(P940)が登録した場合はそれを壊さないため */
    }

    debug_log("[P958-FLOPPYU] site=%s f=%d cfg_enabled=%d cfg_base=%d ext_fdd_wired=%d effective=%d reason=%s table=%s orig_is_p939=%d orig_saved=%d\n",
              site ? site : "(null)", g_mx68k_frame_num, enabled ? 1 : 0, (int)assigned_base,
              ext_fdd_wired ? 1 : 0, effective, reason, table,
              (s_p958_orig_read == mx68k_floppyu_fdc_read_probe) ? 1 : 0,
              (s_p958_orig_read != NULL && s_p958_orig_write != NULL) ? 1 : 0);
    rl_cfg_log(site, enabled, assigned_base);   /* P1054 */
}

/* P1054: 設定「Allow writing to FloppyU disks」。install 直前に EmulatorBridge.c から渡され、
 * rl_install_reset のモード解析(環境変数 MX68K_FLOPPYU_WRITE が優先)で使う */
void mx68k_floppyu_fdcpt_set_write_config(int enabled)
{
    s_rl_write_cfg = enabled ? 1 : 0;
}

/* P1067: 設定「Allow FORMAT on FloppyU disks」。install 直前に EmulatorBridge.c から渡され、rl_install_reset で
 * normal(設定由来)のときだけ有効になる(環境変数 MX68K_FLOPPYU_WRITE が設定されていれば無視) */
void mx68k_floppyu_fdcpt_set_format_config(int enabled)
{
    s_rl_format_cfg = enabled ? 1 : 0;
}

/* P1089: 設定「書込みモード」(0=安全優先 / 1=高速)。install 直前に EmulatorBridge.c から渡され、rl_install_reset で
 * normal(設定由来)のときだけ有効になる(環境変数 MX68K_FLOPPYU_WRITE が設定されていれば無視) */
void mx68k_floppyu_fdcpt_set_write_fast_config(int fast)
{
    s_rl_write_fast_cfg = fast ? 1 : 0;
}

/* P1089: 設定画面の1秒周期表示用(メインスレッドから読むだけ)。宣言は EmulatorBridge.h。
 * fast: 1=高速モードが有効 / src: 0=off 1=設定 3=設定は高速だが MX68K_FLOPPYU_WRITE 優先で無視 /
 * dirty: 1=ふろっぴU君に未書込みのデータが残っている可能性 / cached_unverified: 高速で受理し未照合のセクタ数 /
 * v_mismatch_cached: 高速で受理したセクタの読み戻し不一致数 / discarded: 1=ふろっぴU君が未書込みデータを破棄した('D')ため停止中 */
void mx68k_floppyu_get_write_fast(int32_t* fast, int32_t* src, int32_t* dirty, int32_t* cached_unverified,
                                  int32_t* v_mismatch_cached, int32_t* discarded)
{
    int en = __atomic_load_n(&s_p955_enabled, __ATOMIC_RELAXED);
    int fs = __atomic_load_n(&s_nm_fast, __ATOMIC_RELAXED);
    int sr = __atomic_load_n(&s_nm_fast_src, __ATOMIC_RELAXED);
    int dt = __atomic_load_n(&s_fu_wc_dirty, __ATOMIC_RELAXED);
    uint32_t cu = __atomic_load_n(&s_nm_cached_unverified, __ATOMIC_RELAXED);
    uint32_t vm = __atomic_load_n(&s_nm_v_mismatch_cached, __ATOMIC_RELAXED);
    uint32_t dd = __atomic_load_n(&s_fu_wc_discard_D, __ATOMIC_RELAXED);
    if (fast)              *fast = (en && fs) ? 1 : 0;
    if (src)               *src = en ? sr : NMFAST_SRC_OFF;
    if (dirty)             *dirty = (en && dt) ? 1 : 0;
    if (cached_unverified) *cached_unverified = (int32_t)cu;
    if (v_mismatch_cached) *v_mismatch_cached = (int32_t)vm;
    if (discarded)         *discarded = (en && dd > 0u) ? 1 : 0;
}

/* P1067: 設定画面の1秒周期表示用(メインスレッドから読むだけ)。宣言は EmulatorBridge.h。
 * on: 1=FORMAT 許可が有効 / halted: 1=停止中(停止ラッチ、または走行中に書込みラッチが立った)/
 * tracks_ok: プロセス通算で検証 ok になったトラック数 / src: 0=off 1=config 2=env(MX68K_FLOPPYU_NM_FORMAT)
 * 3=設定 ON だが MX68K_FLOPPYU_WRITE 優先のため無視 */
void mx68k_floppyu_get_format_status(int32_t* on, int32_t* halted, int32_t* tracks_ok, int32_t* src)
{
    int en = __atomic_load_n(&s_p955_enabled, __ATOMIC_RELAXED);
    int o  = __atomic_load_n(&s_nmf_on, __ATOMIC_RELAXED);
    int h  = __atomic_load_n(&s_nmf_halt, __ATOMIC_RELAXED);
    int la = __atomic_load_n(&s_rl_wr_latched_off, __ATOMIC_RELAXED);
    int ra = __atomic_load_n(&s_nmf_run_active, __ATOMIC_RELAXED);
    int sr = __atomic_load_n(&s_nmf_src, __ATOMIC_RELAXED);
    uint32_t tk = __atomic_load_n(&s_nmf_tracks_ok, __ATOMIC_RELAXED);
    if (on)        *on = (en && o) ? 1 : 0;
    if (halted)    *halted = (en && (h || (la && ra))) ? 1 : 0;
    if (tracks_ok) *tracks_ok = (int32_t)tk;
    if (src)       *src = en ? sr : NMF_SRC_OFF;
}

/* P1054: mx68k_shutdown から呼ぶ。開いている観測窓を閉じ、累積要約を出す(振り分け器を一度も有効にしていなければ何もしない) */
void mx68k_floppyu_fdcpt_shutdown(void)
{
    /* P1089(B-11 N3): 高速モードで装置キャッシュが dirty のまま終了する場合はログのみ(能動 SDS は送らない)。
     * mx68k_shutdown 時点でエミュレーションスレッドが停止済みである保証が無い(P54 未解決、AppDelegate.swift
     * applicationWillTerminate のコメント)ため、port1 往復で振り分け器の状態へ新たな並行アクセスを加えない。
     * 未 flush データは装置のアイドル自発 flush(最終送信から約500ms)に委ねる。P54 解決時に能動 flush 確認を再検討する */
    if (s_fu_wc_dirty) {
        char ps[FU_WC_PEND_MAX * 16 + 8];
        rl_wc_pend_str(ps, sizeof(ps));
        s_fu_wc_shutdown_logonly = 1;
        debug_log(P1089_TAG " shutdown_dirty_logonly f=%d est=%u pend_n=%u ovf=%d pend=%s flush_req=%d flush_src=%s dirty_f=%d"
                  " last_cached_f=%d since_send_us=%lld\n",
                  g_mx68k_frame_num, (unsigned)s_fu_wc_est, (unsigned)s_fu_wc_pend_n, s_fu_wc_pend_ovf, ps, s_fu_wc_flush_req,
                  WCF_NAME[s_fu_wc_flush_src], s_fu_wc_dirty_f, s_fu_wc_last_cached_f,
                  (long long)(rl_now_us() - s_rl_wr_send_t_us));
    }
    if (!s_e5p_seen) return;
    e5p_close("shutdown");
    e5p_log_sum("shutdown");
    s_e5p_seen = 0;   /* 二重呼出しで要約を重複させない(電源OFF→ONの再 init では install が再び立てる) */
}

/* P1054: 設定画面の1秒周期表示用(メインスレッドから読むだけ)。宣言は EmulatorBridge.h */
void mx68k_floppyu_get_write_status(int32_t* mode, int32_t* latched, int32_t* env_override)
{
    int en = __atomic_load_n(&s_p955_enabled, __ATOMIC_RELAXED);
    int wm = __atomic_load_n(&s_rl_write_mode, __ATOMIC_RELAXED);
    int ms = __atomic_load_n(&s_rl_mode_src, __ATOMIC_RELAXED);
    int la = __atomic_load_n(&s_rl_wr_latched_off, __ATOMIC_RELAXED);
    int32_t m = (!en || wm == WRM_OFF) ? 0 : (wm == WRM_NORMAL ? 1 : 2);
    if (mode)         *mode = m;
    if (latched)      *latched = (m != 0 && la) ? 1 : 0;
    if (env_override) *env_override = (en && ms == RL_MSRC_ENV) ? 1 : 0;
}

/* P1092(D-127): ステータスバーのFD表示用(drive は MX 側ドライブ番号 0-3)。表示専用で送信判断には使わない */
int mx68k_floppyu_status_assigned(int drive)
{
    if (drive < 0 || drive > 3) return 0;
    int en = __atomic_load_n(&s_p955_enabled, __ATOMIC_RELAXED);
    uint8_t mask = __atomic_load_n(&s_p955_mask, __ATOMIC_RELAXED);
    return (en && ((mask >> drive) & 1u)) ? 1 : 0;
}

int mx68k_floppyu_status_present(int drive)
{
    if (!mx68k_floppyu_status_assigned(drive)) return 0;
    return __atomic_load_n(&s_floppyu_present[drive], __ATOMIC_RELAXED) ? 1 : 0;
}

int mx68k_floppyu_status_active(int drive)
{
    if (drive < 0 || drive > 3) return 0;
    if (!mx68k_floppyu_status_assigned(drive)) return 0;   /* P1095: presentではなくassignedにのみ依存 */
    int last = __atomic_load_n(&s_rl_last_io_frame[drive], __ATOMIC_RELAXED);
    /* P1097: 12→90フレームへ拡大。EmulatorEngine(1Hz=約56-60フレーム周期)・
     * DebuggerView/DisassemblyMonitorView(0.5秒=約28-30フレーム周期)いずれの
     * ポーリング周期より十分大きい窓にすることで、短時間READバースト(送信時刻を
     * last_io_frameに記録、完了は見ない)が次回ポーリングまでに取りこぼされない
     * ようにする。非消費型(値を変更しない)なので複数の読み手が同時に読んでも
     * 互いに干渉しない */
    return (g_mx68k_frame_num - last) < 90 ? 1 : 0;
}

void mx68k_floppyu_fdcpt_tick_frame(void)
{
    if (!s_p949_enabled) return;
    if (s_p955_enabled) rl_tick();   /* P1009: ブリングアップ・port2受信・タイムアウト・照合(有効時のみ) */
    if ((g_mx68k_frame_num % P949_SUM_PERIOD) != 0) return;
    if (g_mx68k_frame_num == s_p949_last_sum_f) return;
    s_p949_last_sum_f = g_mx68k_frame_num;
    debug_log("[P949-FDCPT] f=%u read_count=%u write_count=%u ready_count=%u\n",
              (unsigned)g_mx68k_frame_num, (unsigned)s_p949_read_count,
              (unsigned)s_p949_write_count, (unsigned)s_p949_ready_count);
    debug_log("[P951-FDCTIMING] f=%u dma0_exec_count=%u irq_before_drain_count=%u ready_ocr0=%u ocr1=%u ocr2=%u ocr3=%u dir_write=%u dir_read=%u\n",
              (unsigned)g_mx68k_frame_num, (unsigned)s_p951_dma0_exec_count,
              (unsigned)s_p951_irq_before_drain_count,
              (unsigned)s_p951_ready_ocr_count[0], (unsigned)s_p951_ready_ocr_count[1],
              (unsigned)s_p951_ready_ocr_count[2], (unsigned)s_p951_ready_ocr_count[3],
              (unsigned)s_p952_ready_dir_count[0], (unsigned)s_p952_ready_dir_count[1]);
    debug_log("[P954-FDCATTR] f=%u cmd_total=%u us_match=%u us_mismatch=%u specify=%u sense_int_status=%u other_no_param=%u write_phase_cmd=%u\n",
              (unsigned)g_mx68k_frame_num, (unsigned)s_p954_cmd_total,
              (unsigned)s_p954_us_match_count, (unsigned)s_p954_us_mismatch_count,
              (unsigned)s_p954_specify_count, (unsigned)s_p954_sense_int_status_count,
              (unsigned)s_p954_other_no_param_count, (unsigned)s_p954_write_phase_cmd_count);
    if (s_p955_enabled) {
        debug_log("[P955-ROUTER] f=%u mask=0x%x cmd_start=%u core_cmd=%u stub_cmd=%u core_noparam=%u sense_int_core=%u sense_int_stub=%u pending_now=%d key_disagree=%u fifo_overflow=%u stub_int=%u us_hist=%u/%u/%u/%u last_stub_cmd=0x%02x last_us_mx=0x%02x last_us_dev=0x%02x last_st0_dev=0x%02x last_st0_mx=0x%02x last_stub_f=%d\n",
                  (unsigned)g_mx68k_frame_num, (unsigned)s_p955_mask,
                  (unsigned)s_p955_cmd_start, (unsigned)s_p955_core_cmd,
                  (unsigned)s_p955_stub_cmd_count, (unsigned)s_p955_core_noparam,
                  (unsigned)s_p955_sense_int_core, (unsigned)s_p955_sense_int_stub,
                  s_p955_pending, (unsigned)s_p955_key_disagree,
                  (unsigned)s_p955_fifo_overflow, (unsigned)s_p955_stub_int_raised,
                  (unsigned)s_p955_us_hist[0], (unsigned)s_p955_us_hist[1],
                  (unsigned)s_p955_us_hist[2], (unsigned)s_p955_us_hist[3],
                  (unsigned)s_p955_last_stub_cmd, (unsigned)s_p955_last_us_mx,
                  (unsigned)s_p955_last_us_dev, (unsigned)s_p955_last_st0_dev,
                  (unsigned)s_p955_last_st0_mx, s_p955_last_stub_f);
        rl_log_sum();
    }
}

#ifdef MX68K_FLOPPYU_USB_SELFTEST
/* P1054 T-1 e5p-c: 観測関数(e5p_*)が振り分け器の状態に書き込まないことの機械確認(単体テスト専用)。
 * 呼出し前後で柵・キャッシュ・状態機械・割込み転送・装置通信の計数をスナップショット比較する */
typedef struct {
    int      state, e5c_valid, e5_sent_valid, e7_sent_valid, sds_valid[8], fi_pending, fi_inflight, write_mode, latched;
    int      owner, pending, nm_fo_valid[4], nm_st, nm_base_ok[4], nm_hold_amb, nm_hold_multibit, nm_oc_frame[4];
    uint8_t  e5c_key, e5c_val, e5_sent, e7_sent, dev_ctrl, core_ctrl, nm_open, nm_base_val[4], nm_oc, nm_hold_read;
    int64_t  e5c_t;
    uint32_t d_events, D_events, dl_events, fi_coalesced, fi_acked, nm_fo_ue[4], nm_ue[4], nm_fo_gen[4], nm_close_ev[NM_CL_N];
    uint32_t nm_hold_nd, nm_holds;
    uint32_t e5_usb, e5_cache, e5_sent_n, p1_calls, gen_seen;
} e5p_rl_snap_t;

static void e5p_rl_snap(e5p_rl_snap_t* s)
{
    memset(s, 0, sizeof(*s));
    s->state = s_rl_state; s->e5c_valid = s_rl_e5c_valid; s->e5_sent_valid = s_rl_e5_sent_valid;
    s->e7_sent_valid = s_rl_e7_sent_valid; memcpy(s->sds_valid, s_rl_sds_valid, sizeof(s->sds_valid));
    s->fi_pending = s_rl_fdd_int_pending; s->fi_inflight = s_rl_fi_inflight; s->write_mode = s_rl_write_mode;
    s->latched = s_rl_wr_latched_off; s->owner = s_p955_owner; s->pending = s_p955_pending;
    memcpy(s->nm_fo_valid, s_nm_fo_valid, sizeof(s->nm_fo_valid));
    s->e5c_key = s_rl_e5c_key; s->e5c_val = s_rl_e5c_val; s->e5_sent = s_rl_e5_sent; s->e7_sent = s_rl_e7_sent;
    s->dev_ctrl = s_p955_dev_ctrl; s->core_ctrl = s_p955_core_ctrl; s->nm_open = rl_nm_open_mask();
    s->e5c_t = s_rl_e5c_t;
    s->d_events = s_rl_d_events; s->D_events = s_rl_D_events; s->dl_events = s_rl_dl_events;
    s->fi_coalesced = s_rl_fi_coalesced; s->fi_acked = s_rl_fi_acked;
    memcpy(s->nm_fo_ue, s_nm_fo_ue, sizeof(s->nm_fo_ue)); memcpy(s->nm_ue, s_nm_ue, sizeof(s->nm_ue));
    memcpy(s->nm_fo_gen, s_nm_fo_gen, sizeof(s->nm_fo_gen));
    memcpy(s->nm_close_ev, s_nm_close_ev, sizeof(s->nm_close_ev));
    /* P1055: 判別保留の状態機械(e5p_* はこれらにも書き込まないこと) */
    s->nm_st = s_nm_st; memcpy(s->nm_base_ok, s_nm_base_ok, sizeof(s->nm_base_ok));
    memcpy(s->nm_base_val, s_nm_base_val, sizeof(s->nm_base_val)); s->nm_oc = s_nm_oc_mask;
    memcpy(s->nm_oc_frame, s_nm_oc_frame, sizeof(s->nm_oc_frame));
    s->nm_hold_read = s_nm_hold_read; s->nm_hold_amb = s_nm_hold_amb; s->nm_hold_multibit = s_nm_hold_multibit;
    s->nm_hold_nd = s_nm_hold_nd; s->nm_holds = s_nm_at_holds;
    s->e5_usb = s_rl_e5_usb; s->e5_cache = s_rl_e5_cache; s->e5_sent_n = s_rl_e5_sent_n; s->p1_calls = s_rl_p1_calls;
    s->gen_seen = s_rl_gen_seen;
}

/* 戻り値: 1=前後で同一 / 0=差分あり */
int mx68k_floppyu_fdcpt_selftest_e5p_invariance(void)
{
    e5p_rl_snap_t a, b;
    e5p_rl_snap(&a);
    e5p_on_event('D');
    e5p_on_read(0x00u, 0x80u, E5P_SRC_USB);
    e5p_on_read(0x00u, 0x00u, E5P_SRC_CACHE);
    e5p_on_read(0x00u, 0x00u, E5P_SRC_NOBIT);
    e5p_on_read(0x00u, 0x00u, E5P_SRC_NORELAY);
    e5p_on_event('d');
    e5p_tick();
    e5p_close("selftest");
    e5p_on_read(0x00u, 0x80u, E5P_SRC_WRWAIT);
    e5p_log_sum("selftest");
    e5p_rl_snap(&b);
    return memcmp(&a, &b, sizeof(a)) == 0 ? 1 : 0;
}

/* P1055 T-1 attr-*: 判別保留の状態機械の生値(読取りのみ) */
void mx68k_floppyu_fdcpt_selftest_nm_attr(int* st, uint32_t ue[4], int base_ok[4], uint8_t base_val[4], uint8_t* oc_mask,
                                         uint32_t* D_events, uint32_t* dl_events, int* fi_pending)
{
    *st = s_nm_st;
    memcpy(ue, s_nm_ue, sizeof(s_nm_ue));
    memcpy(base_ok, s_nm_base_ok, sizeof(s_nm_base_ok));
    memcpy(base_val, s_nm_base_val, sizeof(s_nm_base_val));
    *oc_mask = s_nm_oc_mask;
    *D_events = s_rl_D_events;
    *dl_events = s_rl_dl_events;
    *fi_pending = s_rl_fdd_int_pending;
}

/* P1064 T-1 fn-fmt1-sendfail*: 次の1回の FORMAT 送信を装置へ送らず、送信関数の戻り値 r として扱う */
void mx68k_floppyu_fdcpt_selftest_fmt1_force_send_ret(int r)
{
    s_st_fmt1_force_ret = r;
}

/* P1067 T-1 fn-nmf-*: 次の1回の normal FORMAT 送信を装置へ送らず、送信関数の戻り値 r として扱う */
void mx68k_floppyu_fdcpt_selftest_nmf_force_send_ret(int r)
{
    s_st_nmf_force_ret = r;
}

/* P1067 T-1 fn-nmf-bringup-pcn: 世代を変えずに次の tick で再ブリングアップさせる(打切り後の再 bringup と同じ入口) */
void mx68k_floppyu_fdcpt_selftest_force_rebringup(void)
{
    s_rl_bringup_valid = 0;
}

/* P1087 T-1 fn-d-rebringup-poll: 再入中(s_rl_in_poll)にゲストコマンドが来た場合の inline 再 bringup 見送りを再現する */
void mx68k_floppyu_fdcpt_selftest_set_in_poll(int v)
{
    s_rl_in_poll = v;
}

/* P1067 T-1 fn-nmf-*: normal FORMAT の内部状態(読取りのみ) */
void mx68k_floppyu_fdcpt_selftest_nmf_state(int* run_active, uint32_t* run_pos, int* halt, int* partial, int* verify_pending,
                                           int pcn_valid[4], uint8_t pcn[4])
{
    *run_active = s_nmf_run_active;
    *run_pos = s_nmf_run_pos;
    *halt = s_nmf_halt;
    *partial = s_nmf_partial;
    *verify_pending = s_nmf_verify_pending;
    memcpy(pcn_valid, s_nmf_pcn_valid, sizeof(s_nmf_pcn_valid));
    memcpy(pcn, s_nmf_pcn, sizeof(s_nmf_pcn));
}

/* P1089 T-1 fq-*: 高速モードの装置キャッシュ推定状態と port1 往復上限(読取りのみ) */
void mx68k_floppyu_fdcpt_selftest_fast_state(int* nm_fast, int* dirty, int* flush_req, int* flush_src, uint32_t* est,
                                            uint32_t* pend_n, int* pend_ovf, uint8_t* last_us, int* timeout_ms)
{
    *nm_fast = s_nm_fast;
    *dirty = s_fu_wc_dirty;
    *flush_req = s_fu_wc_flush_req;
    *flush_src = s_fu_wc_flush_src;
    *est = s_fu_wc_est;
    *pend_n = s_fu_wc_pend_n;
    *pend_ovf = s_fu_wc_pend_ovf;
    *last_us = s_fu_wc_last_us;
    *timeout_ms = rl_p1_timeout_ms();
}

/* P1089 T-1 fq-c-*: B-1 の確定式を経ずに s_nm_fast を立てる(B-4 のモード条件が mode1/armed を守ることの確認用) */
void mx68k_floppyu_fdcpt_selftest_force_nm_fast(int v)
{
    s_nm_fast = v ? 1 : 0;
}

/* P1089 T-1 fq-*: normal の照合表1エントリ(装置ユニット us、C/H/R)。out = written, pending, cached, uncertain */
void mx68k_floppyu_fdcpt_selftest_nm_sec(int us, uint8_t c, uint8_t h, uint8_t r, int out[4])
{
    const rl_nm_sec_t* e = &s_nm_map[us & 3][rl_ar_idx(c, h, r)];
    out[0] = e->written; out[1] = e->pending; out[2] = e->cached; out[3] = e->uncertain;
}

/* P1089 T-1 fq-l: 振り分け器のコマンドバッファ s_rl_cmd[] の写し(能動 flush 確認が触れないことの確認用) */
void mx68k_floppyu_fdcpt_selftest_cmd(uint8_t out[10], uint32_t* n)
{
    memcpy(out, s_rl_cmd, sizeof(s_rl_cmd));
    *n = s_rl_cmd_n;
}

/* P1089 T-1 fq-l2: normal の書込みラッチを立てる(ラッチ中は能動 flush 確認を送らないことの確認用) */
void mx68k_floppyu_fdcpt_selftest_latch(void)
{
    rl_nm_latch("selftest");
}
#endif /* MX68K_FLOPPYU_USB_SELFTEST */
