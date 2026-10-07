/* P901: パック10進実数 ⇔ 拡張精度実数の変換(MX68K の独自実装、XEiJ/MAME のコードは参照していない)。
 *
 * 方式: 固定長の多倍長整数(32bit リム)で値を有理数として正確に表し、FPCR の丸めモードで
 * 1回だけ丸める(正確丸め)。実機 68881 の変換は内部67bit演算で正確丸めではない可能性があり、
 * 末桁が実機と異なりうる(既知の制限、Docs/01 3-1 行)。
 *
 * 大きさ: 拡張精度の最大(2^16384)・最小非正規化数(2^-16445)を 10 進へ変換するとき、
 * 分子・分母は最大で約 16,600bit になる。BN_LIMBS は余裕を見て 560 リム(17,920bit)。
 * 10 進→2 進側は指数が3桁(|E|<=999、数字が BCD でなくても 1665 以下)なので必ず収まる。 */
#include <string.h>
#include <math.h>

#include "fpu_packed.h"

#define BN_LIMBS 560

typedef struct {
    int      n;              /* 使用中のリム数(上位の 0 リムは含めない、0 なら値 0) */
    uint32_t w[BN_LIMBS];    /* 下位リムから */
} bn;

static void bn_set_u64(bn *a, uint64_t v)
{
    a->w[0] = (uint32_t)v;
    a->w[1] = (uint32_t)(v >> 32);
    a->n = a->w[1] ? 2 : (a->w[0] ? 1 : 0);
}

static void bn_trim(bn *a)
{
    while (a->n > 0 && a->w[a->n - 1] == 0) a->n--;
}

static int bn_bitlen(const bn *a)
{
    if (a->n == 0) return 0;
    return (a->n - 1) * 32 + (32 - __builtin_clz(a->w[a->n - 1]));
}

static int bn_cmp(const bn *a, const bn *b)
{
    int i;
    if (a->n != b->n) return (a->n > b->n) ? 1 : -1;
    for (i = a->n - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return (a->w[i] > b->w[i]) ? 1 : -1;
    return 0;
}

/* a -= b(a >= b であること) */
static void bn_sub(bn *a, const bn *b)
{
    int64_t borrow = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->w[i] - (i < b->n ? (int64_t)b->w[i] : 0) - borrow;
        borrow = (t < 0) ? 1 : 0;
        a->w[i] = (uint32_t)(t + (borrow << 32));
    }
    bn_trim(a);
}

static void bn_shl(bn *a, int bits)
{
    int limbs = bits / 32, sh = bits % 32, i;
    if (a->n == 0 || bits <= 0) return;
    if (a->n + limbs + 1 > BN_LIMBS) { a->n = 0; return; }   /* 到達しない(容量は冒頭の見積り参照) */
    a->w[a->n + limbs] = 0;
    for (i = a->n - 1; i >= 0; i--) {
        uint32_t v = a->w[i];
        if (sh) {
            a->w[i + limbs + 1] |= v >> (32 - sh);
            a->w[i + limbs] = v << sh;
        } else {
            a->w[i + limbs] = v;
        }
    }
    for (i = 0; i < limbs; i++) a->w[i] = 0;
    a->n += limbs + 1;
    bn_trim(a);
}

static void bn_shr1(bn *a)
{
    int i;
    for (i = 0; i < a->n; i++) {
        a->w[i] >>= 1;
        if (i + 1 < a->n) a->w[i] |= a->w[i + 1] << 31;
    }
    bn_trim(a);
}

static void bn_mul_u64(bn *a, uint64_t m)
{
    unsigned __int128 carry = 0;
    int i;
    for (i = 0; i < a->n; i++) {
        unsigned __int128 t = (unsigned __int128)a->w[i] * m + carry;
        a->w[i] = (uint32_t)t;
        carry = t >> 32;
    }
    while (carry && a->n < BN_LIMBS) {
        a->w[a->n++] = (uint32_t)carry;
        carry >>= 32;
    }
}

static const uint64_t k_pow10_u64[20] = {
    1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL, 100000000ULL,
    1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL, 10000000000000ULL,
    100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL, 100000000000000000ULL,
    1000000000000000000ULL, 10000000000000000000ULL
};

/* a *= 10^p(p >= 0) */
static void bn_mul_pow10(bn *a, int p)
{
    while (p >= 19) { bn_mul_u64(a, k_pow10_u64[19]); p -= 19; }
    if (p > 0) bn_mul_u64(a, k_pow10_u64[p]);
}

static int bn_bit(const bn *a, int i)
{
    if (i < 0 || i / 32 >= a->n) return 0;
    return (int)((a->w[i / 32] >> (i % 32)) & 1u);
}

/* bit 0..i-1 のいずれかが 1 か */
static int bn_any_below(const bn *a, int i)
{
    int k, limbs = i / 32;
    if (i <= 0) return 0;
    for (k = 0; k < limbs && k < a->n; k++)
        if (a->w[k]) return 1;
    if ((i % 32) && limbs < a->n && (a->w[limbs] & ((1u << (i % 32)) - 1u))) return 1;
    return 0;
}

/* 商を q へ、余りを num へ(den > 0)。商が 127bit を超えるなら -1 */
static int bn_divmod(bn *num, const bn *den, unsigned __int128 *q)
{
    int bnum = bn_bitlen(num), bden = bn_bitlen(den), sh, i;
    bn d;
    *q = 0;
    if (bnum < bden) return 0;
    sh = bnum - bden;
    if (sh > 126) return -1;
    d = *den;
    bn_shl(&d, sh);
    for (i = sh; i >= 0; i--) {
        if (bn_cmp(num, &d) >= 0) {
            bn_sub(num, &d);
            *q |= (unsigned __int128)1 << i;
        }
        bn_shr1(&d);
    }
    return 0;
}

/* 丸めで絶対値を 1 増やすか。half: 0=半分未満、1=ちょうど半分、2=半分超、inexact=余りが非0 */
static int round_up(int rnd, int sign, int half, int inexact, int lsb)
{
    switch (rnd & 3) {
    case 0:  return (half == 2) || (half == 1 && lsb);   /* RN: 最近接偶数 */
    case 1:  return 0;                                   /* RZ */
    case 2:  return sign && inexact;                     /* RM */
    default: return !sign && inexact;                    /* RP */
    }
}

int mx_packed_to_ext(const uint32_t in[3], int rnd, uint16_t *hi, uint64_t *lo)
{
    int sm = (int)(in[0] >> 31) & 1;
    int se = (int)(in[0] >> 30) & 1;
    uint64_t frac = ((uint64_t)in[1] << 32) | in[2];
    uint64_t mant10;
    int e10, i;
    bn v;
    uint64_t m;
    int bexp, inexact, half, guard, sticky;

    /* ±∞/NaN: 指数の下位8bit が $FF(FLOAT3.X の判定と同じ) */
    if (((in[0] >> 16) & 0xFFu) == 0xFFu) {
        *hi = (uint16_t)((sm << 15) | 0x7FFF);
        if (frac == 0) {
            *lo = 0x8000000000000000ULL;
        } else {
            *lo = frac | 0x8000000000000000ULL;
            if ((*lo << 1) == 0) *lo |= 0x4000000000000000ULL;
        }
        return 0;
    }

    /* 指数(3桁、4桁目 bit79-76 は入力では無視)と 17 桁の仮数。数字が 0-9 でなくてもそのまま重み付けする */
    e10 = (int)((in[0] >> 24) & 0xF) * 100 + (int)((in[0] >> 20) & 0xF) * 10 + (int)((in[0] >> 16) & 0xF);
    mant10 = in[0] & 0xF;
    for (i = 15; i >= 0; i--) mant10 = mant10 * 10 + ((frac >> (4 * i)) & 0xF);

    if (mant10 == 0) {
        *hi = (uint16_t)(sm << 15);
        *lo = 0;
        return 0;
    }
    if (se) e10 = -e10;
    e10 -= 16;   /* 暗黙の小数点は整数1桁の直後 */

    bn_set_u64(&v, mant10);
    if (e10 >= 0) {
        int L;
        bn_mul_pow10(&v, e10);
        L = bn_bitlen(&v);
        if (L <= 64) {
            m = 0;
            for (i = 0; i < L; i++) m |= (uint64_t)bn_bit(&v, i) << (i + 64 - L);
            guard = 0;
            sticky = 0;
        } else {
            m = 0;
            for (i = 0; i < 64; i++) m |= (uint64_t)bn_bit(&v, L - 64 + i) << i;
            guard = bn_bit(&v, L - 65);
            sticky = bn_any_below(&v, L - 65);
        }
        bexp = L - 1;
    } else {
        /* 値 = mant10 / 10^(-e10)。商が 66-67bit になるよう分子を 2^s 倍してから割る */
        bn den;
        unsigned __int128 q;
        int s, qb;
        bn_set_u64(&den, 1);
        bn_mul_pow10(&den, -e10);
        s = bn_bitlen(&den) - bn_bitlen(&v) + 66;
        if (s < 0) s = 0;
        bn_shl(&v, s);
        if (bn_divmod(&v, &den, &q) < 0) return 0;   /* 到達しない(商は 67bit 以下) */
        qb = 128 - (int)((q >> 64) ? __builtin_clzll((uint64_t)(q >> 64)) : 64 + __builtin_clzll((uint64_t)q));
        m = (uint64_t)(q >> (qb - 64));
        guard = (int)((q >> (qb - 65)) & 1);
        sticky = ((q & (((unsigned __int128)1 << (qb - 65)) - 1)) != 0) || (v.n != 0);
        bexp = qb - 1 - s;
    }

    inexact = guard || sticky;
    half = guard ? (sticky ? 2 : 1) : 0;
    if (round_up(rnd, sm, half, inexact, (int)(m & 1))) {
        m++;
        if (m == 0) { m = 0x8000000000000000ULL; bexp++; }
    }
    /* 範囲: |指数| <= 1665 なので拡張精度の正規化数に必ず収まる(桁あふれ・非正規化は起きない) */
    *hi = (uint16_t)((sm << 15) | (uint16_t)(bexp + 16383));
    *lo = m;
    return inexact ? MX_PACKED_INEXACT : 0;
}

int mx_ext_to_packed(uint16_t hi, uint64_t lo, int k, int rnd, uint32_t out[3])
{
    int sign = (hi >> 15) & 1;
    int ex = hi & 0x7FFF;
    int flags = 0;
    int e2, X, n = 1, p = 0, iter, inexact = 0, ax, se;
    uint64_t q64 = 0, d17, ipart, fpart;
    uint64_t bcd = 0;
    int i;

    out[0] = out[1] = out[2] = 0;

    if (ex == 0x7FFF) {
        out[0] = ((uint32_t)sign << 31) | 0x7FFF0000u;   /* SE=1、YY=11、指数=$FFF */
        if ((lo << 1) != 0) {                            /* NaN: 仮数をそのまま写す */
            out[1] = (uint32_t)(lo >> 32);
            out[2] = (uint32_t)lo;
        }
        return 0;
    }
    if (k > 17) { flags |= MX_PACKED_OPERR; k = 17; }
    if (lo == 0) {
        out[0] = (uint32_t)sign << 31;
        return flags;
    }

    e2 = (ex == 0 ? 1 : ex) - 16383 - 63;   /* 値 = lo × 2^e2(非正規化・非正規形もこの式) */
    X = (int)floor(log10((double)lo) + (double)e2 * 0.30102999566398119521);

    for (iter = 0; iter < 8; iter++) {
        bn num, den, twice;
        unsigned __int128 q;
        int c;

        n = (k > 0) ? k : (X + 1 - k);
        if (n < 1) n = 1;
        if (n > 17) n = 17;
        p = X - n + 1;   /* 10 進の整数 D = 値 / 10^p を n 桁で求める */

        bn_set_u64(&num, lo);
        bn_set_u64(&den, 1);
        if (e2 >= 0) bn_shl(&num, e2); else bn_shl(&den, -e2);
        if (p >= 0) bn_mul_pow10(&den, p); else bn_mul_pow10(&num, -p);

        if (bn_divmod(&num, &den, &q) < 0 || q >= (unsigned __int128)k_pow10_u64[n]) { X++; continue; }
        q64 = (uint64_t)q;
        if (q64 < k_pow10_u64[n - 1]) { X--; continue; }

        /* num は余り。2×余り と分母を比べて半分未満/ちょうど/超を決める */
        inexact = (num.n != 0);
        twice = num;
        bn_shl(&twice, 1);
        c = bn_cmp(&twice, &den);
        if (round_up(rnd, sign, c > 0 ? 2 : (c == 0 ? 1 : 0), inexact, (int)(q64 & 1))) {
            q64++;
            if (q64 == k_pow10_u64[n]) { q64 = k_pow10_u64[n - 1]; X++; }
        }
        break;
    }
    if (inexact) flags |= MX_PACKED_INEXACT;

    /* 17 桁へ右詰め(末尾は 0)。整数部1桁+小数部16桁 */
    d17 = q64 * k_pow10_u64[17 - n];
    ipart = d17 / k_pow10_u64[16];
    fpart = d17 % k_pow10_u64[16];
    for (i = 0; i < 16; i++) {
        bcd |= (fpart % 10) << (4 * i);
        fpart /= 10;
    }

    se = (X < 0);
    ax = se ? -X : X;
    if (ax > 999) flags |= MX_PACKED_OPERR;   /* 4 桁目は bit79-76 へ */
    out[0] = ((uint32_t)sign << 31) | ((uint32_t)se << 30)
           | ((uint32_t)((ax / 100) % 10) << 24) | ((uint32_t)((ax / 10) % 10) << 20) | ((uint32_t)(ax % 10) << 16)
           | ((uint32_t)((ax / 1000) % 10) << 12)
           | (uint32_t)ipart;
    out[1] = (uint32_t)(bcd >> 32);
    out[2] = (uint32_t)bcd;
    return flags;
}
