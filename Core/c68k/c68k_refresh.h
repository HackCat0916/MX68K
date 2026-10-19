/* P976 (D-15): 主記憶DRAMリフレッシュの周期スチール(c68k)——計算部分のみ。
 *
 * 実機計測(P975、X68000 XVI): 周期 Tr=14.70us、調停 M2(期限が進行中のバスサイクルの中なら
 * そのバスの終わりから、内部動作中なら期限の時刻から占有)、占有 4 CPUclk。占有と重なった
 * バスサイクルは占有の終わりまで遅れる。
 *
 * c68k には命令内のバス位置の情報が無いので、命令の終わり(RET)で「既定の並び」を当てはめて
 * 衝突を計算する: バス(4clk)を命令の先頭に BusCnt 回並べ、残りを内部動作とする。
 * 例外は分岐成立(Bcc/BRA、BSR除外、所要10clk)だけで「内部2clk→バス→バス」とする。
 * 判定規則は .mx68k_cycles/p976_impl_sim.py の候補(e)(seg_sim の M2 規則)と同一。
 *
 * 時間は命令の先頭を 0 とする前向きの 1/256clk 単位(Q8)。
 * このヘッダは c68k_struc に依存しない(単体照合ハーネスから Core を編集せずに include できる)。 */

#ifndef C68K_REFRESH_H
#define C68K_REFRESH_H

#include <stdint.h>

#define C68K_REFRESH_BUS_Q8   (4 * 256)        /* バスサイクル 1 回 = 4clk */
#define C68K_REFRESH_OCC_Q8   (4 * 256)        /* リフレッシュ 1 回の占有 = 4clk */
#define C68K_REFRESH_MAXBUS   64               /* 並べるバスの上限(超えた分は内部動作扱い) */
#define C68K_REFRESH_MAXLEN   (1 << 20)        /* 命令長の上限(Q8 換算の桁あふれ防止) */
#define C68K_REFRESH_NEVER    (INT32_MIN / 2)  /* 無効時の期限(CCnt がここまで下がることは無い) */
#define C68K_REFRESH_ALWAYS   INT32_MAX        /* キャリー保留中の期限(次の RET で必ず成立) */

typedef struct {
    int32_t stall;      /* この命令で課金するストール clk(前命令からのキャリー分を含む) */
    int32_t hits;       /* 処理した期限の数 */
    int32_t nego;       /* 期限が命令の先頭より前だったため 0 へ丸めた回数 */
    int32_t dmax;       /* 1 回の衝突で生じた最大ストール clk */
    int32_t carry_q8;   /* 出力: 命令の終わりをはみ出した占有の残り(次の命令の先頭から数えた
                         * 占有の終わり、Q8、0=なし) */
} c68k_refresh_res;

/* 分岐成立(Bcc/BRA、BSR除外)で所要 10clk の命令か */
static inline int c68k_refresh_is_branch10(uint32_t opcode, int32_t L)
{
    return ((opcode & 0xF000u) == 0x6000u) && (((opcode >> 8) & 0xFu) != 1u) && (L == 10);
}

/* 期限を 1 周期進める。期限は CCnt 座標(減っていく向き)の dl + frac/256。 */
static inline void c68k_refresh_advance(int32_t *dl, int32_t *frac, int32_t period_q8)
{
    int32_t f = *frac - (period_q8 & 255);
    *dl -= period_q8 >> 8;
    if (f < 0) { f += 256; *dl -= 1; }
    *frac = f;
}

/* Q8 値の 256 による切捨て除算(負の値でも床関数) */
static inline int32_t c68k_refresh_floor256(int64_t x)
{
    return (int32_t)(x >= 0 ? x / 256 : -((-x + 255) / 256));
}

/* 1 命令ぶんの衝突計算(主記憶上の命令のみ呼ぶ)。
 *   L          ストール前の所要 clk(RET 直後の CCnt と命令先頭の差)
 *   nb         この命令のバス回数(フェッチ語数 + データアクセス回数)
 *   branch10   分岐成立の並び(内部2clk→バス[2,6)→バス[6,10))を使うか
 *   carry_in   直前の命令からの占有の持ち越し(この命令の先頭から数えた占有の終わり、Q8、0=なし)
 *   *o_q8      入力: 最初の未処理期限の、命令先頭からの位置(Q8)
 *              出力: 次の未処理期限の位置(命令の終わり[L+stall]より後)
 *   period_q8  周期(Q8、> 0) */
static inline void c68k_refresh_calc(int32_t L, int32_t nb, int branch10, int32_t carry_in,
                                     int32_t *o_q8, int32_t period_q8, c68k_refresh_res *r)
{
    int32_t bs[C68K_REFRESH_MAXBUS], be[C68K_REFRESH_MAXBUS];
    int32_t n = 0, k, j, d, st, occ, lim, stall = 0;

    r->stall = r->hits = r->nego = r->dmax = 0;
    r->carry_q8 = 0;

    if (L < 0) L = 0;
    if (L > C68K_REFRESH_MAXLEN) L = C68K_REFRESH_MAXLEN;
    lim = L * 256;

    if (branch10) {
        bs[0] = 2 * 256; be[0] = 6 * 256;
        bs[1] = 6 * 256; be[1] = 10 * 256;
        n = 2;
    } else {
        if (nb > C68K_REFRESH_MAXBUS) nb = C68K_REFRESH_MAXBUS;
        for (k = 0; k < nb; k++) {
            int32_t s = k * C68K_REFRESH_BUS_Q8;
            if (s >= lim) break;
            bs[k] = s;
            be[k] = (s + C68K_REFRESH_BUS_Q8 < lim) ? s + C68K_REFRESH_BUS_Q8 : lim;
        }
        n = k;
    }

    /* 直前の命令からはみ出した占有: この命令の最初のバスと重なる分だけ遅らせる */
    if (carry_in > 0) {
        if (n > 0) {
            if (bs[0] < carry_in) {
                d = (carry_in - bs[0] + 255) >> 8;
                for (j = 0; j < n; j++) { bs[j] += d * 256; be[j] += d * 256; }
                stall += d;
                if (d > r->dmax) r->dmax = d;
            }
        } else if (carry_in > lim) {
            r->carry_q8 = carry_in - lim;   /* バスの無い命令は素通りして次の命令へ */
        }
    }

    while (*o_q8 <= (L + stall) * 256) {
        int32_t o = *o_q8;
        if (o < 0) { o = 0; r->nego++; }
        r->hits++;
        for (k = 0; k < n && bs[k] < o; k++) ;   /* o 以降で最初に始まるバス */
        st = (k > 0 && o < be[k - 1]) ? be[k - 1] : o;   /* M2 */
        occ = st + C68K_REFRESH_OCC_Q8;
        if (k < n) {
            if (bs[k] < occ) {
                d = (occ - bs[k] + 255) >> 8;
                for (j = k; j < n; j++) { bs[j] += d * 256; be[j] += d * 256; }
                stall += d;
                if (d > r->dmax) r->dmax = d;
            }
        } else {
            int32_t rem = occ - (L + stall) * 256;
            if (rem > r->carry_q8) r->carry_q8 = rem;
        }
        *o_q8 += period_q8;
    }
    r->stall = stall;
}

#endif  /* C68K_REFRESH_H */
