#include "status.h"

/* Bridge 側の宣言: StatBar_SetFDD/ParamFDD はプラットフォーム用スタブである。
   fdd.c がこれらを呼ぶ。定義は Core/px68k/x68k/status.h ではなくここにある。 */
void StatBar_SetFDD(int32_t drive, const char* filename);
void StatBar_ParamFDD(int32_t drive, int32_t state, int32_t emask, int32_t blink);

/* P160: FDD アクセス(リード/シーク)動作インジケータ。Core fdd.c は、あるドライブが
 * 能動的に選択されたドライブであるとき(ドライブ選択 $E94007 への書き込み時の
 * FDD_SetAccess)、state==2 で StatBar_ParamFDD を呼ぶ。ドライブごとにそうした
 * 最後のイベントのフレームを記録し、mx68k_fdd_accessing() は短い減衰窓の間
 * 「アクセス中」を報告する。これにより操作ごとの短いパルスもステータスバーで見える。 */
extern int g_mx68k_frame_num;   /* フレームカウンタ(EmulatorBridge.c で定義) */
/* P684: FDD 2 台 → 4 台。Core fdd.c は元から drive 0-3 の範囲チェックを持ち
 * (fdd.c:91,113,133,149 の `drive>3`)、ドライブ 2/3 に対しても
 * StatBar_ParamFDD() を呼んでいる。本配列が 2 要素だった間、その呼び出しは
 * 下の `drive > 1` ガードで捨てられていた。 */
static int s_fdd_present[4]     = { 0, 0, 0, 0 };
static int s_fdd_last_access[4] = { -100000, -100000, -100000, -100000 };
static int s_fdd_selected       = 0;   /* P181: 最後に能動的に選択されたドライブ (fdd.Access) */

void StatBar_SetFDD(int32_t drive, const char* filename)
{
    (void)drive;
    (void)filename;
}

void StatBar_ParamFDD(int32_t drive, int32_t state, int32_t emask, int32_t blink)
{
    (void)emask;
    (void)blink;
    if (drive < 0 || drive > 3) return;   /* P684: 2 台 → 4 台 */
    s_fdd_present[drive] = (state != 0);
    if (state == 2) s_fdd_selected = drive;   /* P181: 選択ドライブを追跡する。単なる選択だけでは点灯させない */
}

/* P181: DMAC ch0 (FDD 専用) が実際に転送中 = 実際のディスク Read/Write のときに
 * DMA_Exec(0) の呼び出し箇所から呼ばれる。現在選択中のドライブのランプを点灯させる。 */
void mx68k_fdd_note_rw(void);
void mx68k_fdd_note_rw(void)
{
    s_fdd_last_access[s_fdd_selected] = g_mx68k_frame_num;
}

int mx68k_fdd_accessing(int drive);
int mx68k_fdd_accessing(int drive)
{
    if (drive < 0 || drive > 3) return 0;   /* P684: 2 台 → 4 台 */
    if (!s_fdd_present[drive]) return 0;
    /* 直近約 12 フレーム(60 fps で約 200 ms)以内にアクセスあり = アクティブ表示。 */
    return (g_mx68k_frame_num - s_fdd_last_access[drive]) < 12;
}

/* P443 (D-7): メディア有無のアクセサ。Core fdd.c は FDD_EjectFD() から(ゲスト起点の
 * イジェクトも含めて)StatBar_ParamFDD(drive, 0, ...) を呼ぶため、s_fdd_present[] は
 * 既に実際のメディア状態を追跡している。mx68k_get_status() がこれを Swift 層へ渡せる
 * ようここで公開し、Swift 層は自身の fdd0Path/fdd1Path をこれと突き合わせる。
 * 情報源は FDD_IsReady() ではなく s_fdd_present[] である: FDD_IsReady() は挿入直後の
 * SetDelay 猶予窓の間 false を返すため、1 Hz のステータスポーリングがそれを拾うと
 * 挿入したばかりのパスを誤ってクリアしかねない。 */
int mx68k_fdd_media_present(int drive);
int mx68k_fdd_media_present(int drive)
{
    if (drive < 0 || drive > 3) return 0;   /* P684: 2 台 → 4 台 */
    return s_fdd_present[drive];
}

/* P201: HD BUSY アクセスインジケータ。Core sasi.c は SASI バスが動作中のとき
 * StatBar_HDD((SASI_Phase)?2:0) を呼ぶ (busy=2 / idle=0)。status.c の空スタブは
 * ビルドから除外している (pbxproj の Compile Sources) ため、この定義が有効になる。
 * 最後の busy パルスのフレームを記録し、mx68k_hdd_accessing() は短い減衰窓
 * (FDD ランプと同じ約 12 フレーム)の間「busy」を報告する。これにより短いパルスも
 * ステータスバーで見える。 */
static int s_hdd_last_access = -100000;

void StatBar_HDD(int32_t sw)
{
    if (sw != 0) s_hdd_last_access = g_mx68k_frame_num;   /* busy パルス */
}

int mx68k_hdd_accessing(void);
int mx68k_hdd_accessing(void)
{
    return (g_mx68k_frame_num - s_hdd_last_access) < 12;   /* 約 200 ms で減衰 */
}
