/* mx_cpu_iface.h — CPUコア抽象層(P860、第1段)
 * Bridge内のステートセーブ/ロードとモニタ機能が使う、CPUレジスタの読み書き口。
 * 実装はm68000_bridge.c末尾の「mx_cpu_* 実装」節(c68k版)と Bridge/mx_cpu_musashi.c(Musashi版)。
 * P861で初期化・リセット・フェッチ表・m68000_get/set_reg・windrvもここを通るようになった。
 * P862で実行ループ・割込み要求・メモリアクセス中のサイクル操作もここを通るようになった。
 * P868でバスエラー合成はiface経由(mx_cpu_jump_raw32)になった。チャンク境界の補正
 * (P47-E/P14/P565)はc68k専用のままで、Musashi実行時は行わない。
 * P868: 環境変数 MX68K_CPU_CORE=musashi でプロセス起動時にMusashiを選べる(既定はc68k)。
 * ★CPUコア固有の型・定数はこのヘッダに一切出さない(<stdint.h>のみに依存)。
 *   将来の別コア(030用等)へ差し替える際の継ぎ目とするため。
 * ★Swiftには公開しない(EmulatorBridge.hからはincludeしない)。 */
#ifndef MX_CPU_IFACE_H
#define MX_CPU_IFACE_H
#include <stdint.h>

/* --- レジスタ読み出し(モニタ・ステートセーブ) --- */
uint32_t mx_cpu_get_dreg(int n);   /* n = 0..7 */
uint32_t mx_cpu_get_areg(int n);   /* n = 0..7。A7は現在有効なSP */
uint32_t mx_cpu_get_pc(void);
uint32_t mx_cpu_get_sr(void);
uint32_t mx_cpu_get_usp(void);
uint32_t mx_cpu_get_ssp(void);     /* 68000のスーパーバイザSP(c68kではMSP) */

/* --- レジスタ書き込み(ステートロード・m68000_set_reg・windrv) --- */
void mx_cpu_set_dreg(int n, uint32_t v);
void mx_cpu_set_areg(int n, uint32_t v);
void mx_cpu_set_sr(uint32_t v);
void mx_cpu_set_usp(uint32_t v);   /* 結果はSRのSビットに依存する。SR→A7→USPの順で呼ぶこと */
void mx_cpu_set_pc(uint32_t v);    /* 24bitにマスクしてから設定(m68000_set_regと同じ) */
void mx_cpu_set_ssp(uint32_t v);   /* 68000のスーパーバイザSP(c68kではMSP)。現在のモードに関係なく設定される */

/* 例外の飛び先へ、32bit値のままPCを設定する(上位byteを保持、24bitマスクしない)。
 * mx_cpu_set_pc とは別物(あちらは24bitマスク)。68000のPCは32bitレジスタで、上位8bitは
 * バスに出ないがレジスタとスタックへ積まれる値には残る(68000ファミリハンドブック 書籍p.10)。
 * IPLROMのbsrタグ技法がこの性質に依存する。★命令境界(mx_cpu_execute の外)でのみ呼ぶこと。 (P868) */
void mx_cpu_jump_raw32(uint32_t pc32);

/* --- 初期化・リセット(P861、第2段) --- */
/* コアがメモリ・割込み受理のために呼ぶ関数一式。
 * read8/read16 の戻り値は下位8/16bitが有効。write8/write16 の data も同様。
 * int_ack は割込み受理時に level(1..7)で呼ばれ、例外ベクタ番号を返す。
 *   ★戻り値の規約(オートベクタの表し方)は現状 c68k バックエンドの解釈に素通しする。
 *     別コアを足すときは、そのバックエンド側で変換すること。 */
typedef struct {
    uint32_t (*read8)(uint32_t addr);
    uint32_t (*read16)(uint32_t addr);
    void     (*write8)(uint32_t addr, uint32_t data);
    void     (*write16)(uint32_t addr, uint32_t data);
    int32_t  (*int_ack)(int32_t level);
} mx_cpu_bus;

/* コアを電源投入直後の状態に初期化し、bus を登録する。
 * ★命令フェッチ表も消えるので、この後 mx_cpu_map_fetch をすべてやり直すこと。 */
void mx_cpu_init(const mx_cpu_bus *bus);

/* ゲストアドレス lo..hi の命令フェッチを、ホストメモリ host(= lo に対応する位置)から
 * 直接行ってよいとコアへ伝える。粒度とホスト側のバイト順はバックエンド依存
 * (c68k: 64KBページ単位、lo>>16 〜 hi>>16 の各ページに適用。ホスト側はLE16スワップ済み配置)。
 * この仕組みを持たないコアでは何もしない実装でよい。
 * ★c68kはPC設定時にこの表を引くので、mx_cpu_reset / mx_cpu_set_pc より前に設定すること。 */
void mx_cpu_map_fetch(uint32_t lo, uint32_t hi, const void *host);

/* リセット例外処理(ゲストアドレス0からSSP、4からPCを bus 経由で読む)。
 * SSP偶数化などのMX固有の補正は呼び出し側で行う。 */
void mx_cpu_reset(void);

/* --- 実行・割込み・サイクル操作(P862、第3段)。実行ループとメモリアクセスのたびに呼ばれる --- */
/* 最大 cycles サイクル分の命令を実行し、実際に消費したサイクル数を返す。
 * 命令の途中では止まらないので、戻り値が cycles を少し超えることがある。
 * HALT/STOP待ちのまま(保留中の割込みでも起きない)なら命令を実行せず cycles をそのまま返す(c68kの挙動)。 */
int32_t mx_cpu_execute(int32_t cycles);

/* 割込み要求レベル(0..7)を設定する。STOP/HALT待ちを解除する
 * (c68kは常に解除、Musashiは受理された割込みでのみSTOPを解除——CI-1 §5-3)。
 * mx_cpu_execute の実行中(メモリ・割込み受理のコールバック内)から呼ばれた場合は、
 * 今の命令の後で実行を切り上げ、割込みを受け付けられるようにする。 */
void mx_cpu_set_irq(int32_t level);

/* メモリコールバック内から呼ぶ: 今の命令の後で mx_cpu_execute を終わらせる(バスエラー合成などのため)。
 * mx_cpu_execute の外で呼んだ場合は何もしない。 */
void mx_cpu_end_timeslice(void);

/* メモリコールバック内から呼ぶ: 実行中の命令に cycles サイクルを追加で消費させる(I/Oウェイト)。
 * mx_cpu_execute の外で呼んだ場合は何もしない。 */
void mx_cpu_add_cycles(int32_t cycles);

/* mx_cpu_execute の実行中に、この呼出しまでに完了した命令の消費サイクル数を返す(命令境界精度)。
 * mx_cpu_execute の外では -1 を返す。 */
int32_t mx_cpu_cycles_done(void);

/* --- 実行状態(モニタ) --- */
int32_t mx_cpu_get_irq_line(void); /* 保留中の割込みレベル(生値) */
int     mx_cpu_is_halted(void);    /* HALT または STOP待ちなら1 */

/* --- 実行状態の生値(ステート専用、コアごとに中身が違う不透明な値) --- */
uint32_t mx_cpu_state_get_run_status(void);
int32_t  mx_cpu_state_get_irq_line(void);
/* 生値をそのまま書き戻す。割込み要求(コアのIRQ設定関数)ではない:
 * 実行中断・HALT解除などの副作用は起こさない。 */
void     mx_cpu_state_restore_run(uint32_t run_status, int32_t irq_line);

#endif /* MX_CPU_IFACE_H */
