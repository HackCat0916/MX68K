// license:BSD-3-Clause
// copyright-holders:Karl Stenerud, R. Belmont
/* (上の2行は移植元 MAME src/devices/cpu/m68000/m68kfpu.cpp 先頭の表記をそのまま保持している)
 *
 * MX68K作成の移植版(MAME m68kfpu.cpp 由来、上流ファイルそのものではない)——P897。
 * 移植元: https://github.com/mamedev/mame  src/devices/cpu/m68000/m68kfpu.cpp
 *         pinned 4ae561d0019312c0af7a519c2d22f52968e029e1
 *         (gh api repos/mamedev/mame/commits/4ae561d0019312c0af7a519c2d22f52968e029e1 --jq .sha で確認)
 * 本ファイルは上記の改変版である(BSD-3-Clause の条件に従い明示する)。主な改変:
 *   - C++(m68000_musashi_device のメンバ)を C と Musashi の構造体・マクロ(REG_FP/REG_FPCR 等)へ書き換えた。
 *   - 演算は SoftFloat 3e ではなく ThirdParty/softfloat_2a(Hatari経由の SoftFloat 2a)へ接続した。
 *     超越関数は MAME の合成計算ではなく softfloat_fpsp.c の専用関数(floatx80_sin 等)を直接呼ぶ。
 *   - 68040専用部(保留例外・68040フレーム・FSxxx/FDxxx)は移植しない(EC030+6888x では到達しない)。
 *   - fatalerror() は abort せず line-1111 例外へ置き換え、観測カウンタで数える。
 *   - FMOVE.P(パック10進)は未対応で line-1111 例外にする(移植元の変換は binary128 演算に依存し、
 *     softfloat_2a には128bit浮動小数点の演算関数が無いため。将来の拡張点)。
 *   - FSAVE/FRESTORE は選択チップ(68881/68882)に合わせたフレームを扱う(MAME は68881 IDLEのみ)。
 *   - 命令の振り分け(m68040_fpu_op0/op1 の内部)は MAME の m68k_in.lst(ライセンス表記無し)を使わず、
 *     MX68K が命令形式(MC68881/MC68882 User's Manual・MC68030ユーザーズマニュアル)から新規に書いた。
 *
 * Copyright Karl Stenerud, R. Belmont (MAME m68kfpu.cpp)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * 既知の制限(Docs/01 §5-7 にも記載):
 *   - FPU算術例外トラップ(ベクタ48-54)は未実装(MAME も68881/68882では未実装)。FPSRのフラグ更新のみ。
 *   - FSAVE の version byte は実機値が一次資料に無いため MAME の仮値 $1F を使う。
 *   - サイクル数は MAME の値をそのまま使う(MAME 側の出典は未確認、実機タイミングとは乖離しうる)。
 *   - FDBcc($F248-$F24F)・FTRAPcc と FScc 絶対番地形式($F278-$F27F)は、Musashi のジャンプテーブル構築順
 *     (m68kops.c m68ki_build_opcode_table() で cpdbcc/cptrapcc が 040fpu0 を後から上書きする)により
 *     実行時には本ファイルへ到達しない。将来テーブル構築が変わった場合に備えて実装だけは置いてある。
 *
 * m68kcpu.c の中へ m68kcpu.h の後で #include される(単独ではコンパイルしない)。
 * FPU不在(s_mx_fpu_present=0、既定)のときは P869 のスタブと同じく F2xx/F3xx を全て line-1111 例外にする。 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* P869: 観測フック(既定NULL)。Bridge/mx_cpu_musashi.c が実験モデル選択時に登録する。
 * ★型は Bridge/mx_cpu_musashi.c の extern 宣言と一字一句同じにすること */
void (*mx68k_musashi_stub_hook)(int kind, unsigned int ppc, unsigned int ir, unsigned int ext) = 0;
#define MX68K_STUB_FPU_FLINE 1   /* Bridge/mx_cpu_musashi.c・m68kmmu.h の同名定数と一致させること */

#define FPCC_N          0x08000000u
#define FPCC_Z          0x04000000u
#define FPCC_I          0x02000000u
#define FPCC_NAN        0x01000000u

#define FPES_INEXDEC    0x00000100u
#define FPES_INEXACT    0x00000200u
#define FPES_DIVZERO    0x00000400u
#define FPES_UNDERFLOW  0x00000800u
#define FPES_OVERFLOW   0x00001000u
#define FPES_OPERR      0x00002000u
#define FPES_SNAN       0x00004000u
#define FPES_BSUN       0x00008000u

#define FPAE_INEXACT    0x00000008u
#define FPAE_DIVZERO    0x00000010u
#define FPAE_UNDERFLOW  0x00000020u
#define FPAE_OVERFLOW   0x00000040u
#define FPAE_IOP        0x00000080u

/* FPCR/FPSR の書込み可能ビット(それ以外は0として読める) */
#define FPCR_WRITE_MASK 0x0000fff0u
#define FPSR_WRITE_MASK 0x0ffffff8u

#define EXC_ENB_INEXACT 0x00000001u
#define EXC_ENB_UNDFLOW 0x00000002u
#define EXC_ENB_OVRFLOW 0x00000004u

/* FSAVE フレーム(Motorola MC68881/MC68882 User's Manual 1st ed. 1987, §6.4.2 p.6-28、図6-4 p.6-29、図6-5 p.6-30)。
 * 先頭ロング = version byte | size byte | 予約word。size は先頭ロングを含まないバイト数。 */
#define FSAVE_VERSION       0x1Fu   /* 実機値は一次資料に記載無し(「内部で定義」)。MAME の仮値を踏襲 */
#define FSAVE_IDLE_68881    0x18u   /* 28バイト */
#define FSAVE_IDLE_68882    0x38u   /* 60バイト(CU内部状態32Bを含み、APU情報が+$20ずれる) */
#define FSAVE_BUSY_68881    0xB4u   /* 184バイト(FRESTOREで受理のみ) */
#define FSAVE_BUSY_68882    0xD4u   /* 216バイト(同上、MAME未対応だった値) */
#define FSAVE_BIU_FLAGS     0x70000000u   /* MAME と同じ値 */

/* --- MX68K: FPU装着状態と観測カウンタ --- */
static int s_mx_fpu_present;        /* 0=FPU不在(既定)。Bridge から mx68k_m68kfpu_set_config で設定 */
static int s_mx_fpu_model = 68882;  /* 68881 または 68882 */
static float_status s_fpst;         /* 丸めモード・丸め精度・例外フラグ(MAME の大域変数3つに相当) */
static int s_fpu_abort;             /* 命令の途中で line-1111 にした。以後この命令では何も書かない */

enum {
    ST_OP0, ST_OP1, ST_FGEN, ST_FMOVE_OUT, ST_FMOVE_CTL, ST_FMOVEM, ST_FSCC, ST_FDBCC, ST_FTRAP, ST_FBCC,
    ST_FSAVE, ST_FSAVE_NULL, ST_FRESTORE, ST_FRESTORE_NULL, ST_FRESTORE_BADFMT,
    ST_FLINE_PACK, ST_FLINE_UNIMPL, ST_FLINE_FORMAT, ST_PRIV, ST_N
};
static unsigned int s_st[ST_N];

static inline int fx_is_nan(floatx80 a)
{
    return ((a.high & 0x7fff) == 0x7fff) && ((a.low << 1) != 0);
}

static inline int fx_is_signaling_nan(floatx80 a)
{
    uint64_t lo = a.low & ~0x4000000000000000ULL;
    return ((a.high & 0x7fff) == 0x7fff) && ((lo << 1) != 0) && (a.low == lo);
}

static inline floatx80 fx_make(uint16_t high, uint64_t low)
{
    floatx80 r;
    r.high = high;
    r.low = low;
    return r;
}

/* MAME の fatalerror() の置き換え。line-1111 例外を1回だけ起こし、命令の残りを打ち切らせる */
static void fpu_fline(int kind)
{
    if (s_fpu_abort) return;
    s_fpu_abort = 1;
    s_st[kind]++;
    m68ki_exception_1111();
}

static inline floatx80 load_extended_float80(uint ea)
{
    uint d1, d2;
    uint16_t d3;
    floatx80 fp;

    d3 = m68ki_read_16(ea);
    d1 = m68ki_read_32(ea + 4);
    d2 = m68ki_read_32(ea + 8);

    fp.high = d3;
    fp.low = ((uint64_t)d1 << 32) | (d2 & 0xffffffff);
    return fp;
}

static inline void store_extended_float80(uint ea, floatx80 fpr)
{
    m68ki_write_16(ea + 0, fpr.high);
    m68ki_write_16(ea + 2, 0);
    m68ki_write_32(ea + 4, (fpr.low >> 32) & 0xffffffff);
    m68ki_write_32(ea + 8, fpr.low & 0xffffffff);
}

static void set_condition_codes(floatx80 reg)
{
    REG_FPSR &= ~(FPCC_N | FPCC_Z | FPCC_I | FPCC_NAN);

    if (reg.high & 0x8000)
        REG_FPSR |= FPCC_N;
    if (((reg.high & 0x7fff) == 0) && (reg.low == 0))
        REG_FPSR |= FPCC_Z;
    if (((reg.high & 0x7fff) == 0x7fff) && ((reg.low << 1) == 0))
        REG_FPSR |= FPCC_I;
    if (fx_is_nan(reg))
        REG_FPSR |= FPCC_NAN;
}

static void clear_exception_flags(void)
{
    s_fpst.float_exception_flags = 0;
    /* 演算の開始時に消すのは例外ステータスバイトだけ。累積バイトはFPSRへの直接書込みまで残る */
    REG_FPSR &= ~(FPES_BSUN | FPES_SNAN | FPES_OPERR | FPES_OVERFLOW | FPES_UNDERFLOW | FPES_DIVZERO | FPES_INEXACT | FPES_INEXDEC);
}

/* 例外ステータスバイトを累積例外バイトへ畳み込む(68881/68882 の対応関係) */
static void update_accrued_exceptions(void)
{
    if (REG_FPSR & (FPES_BSUN | FPES_SNAN | FPES_OPERR))
        REG_FPSR |= FPAE_IOP;
    if (REG_FPSR & FPES_OVERFLOW)
        REG_FPSR |= FPAE_OVERFLOW;
    if ((REG_FPSR & FPES_UNDERFLOW) && (REG_FPSR & FPES_INEXACT))
        REG_FPSR |= FPAE_UNDERFLOW;
    if (REG_FPSR & FPES_DIVZERO)
        REG_FPSR |= FPAE_DIVZERO;
    if (REG_FPSR & (FPES_INEXACT | FPES_INEXDEC | FPES_OVERFLOW))
        REG_FPSR |= FPAE_INEXACT;
}

static void sync_exception_flags(floatx80 op1, floatx80 op2, uint enables)
{
    const int snan = fx_is_signaling_nan(op1) || fx_is_signaling_nan(op2);
    if (snan)
        REG_FPSR |= FPES_SNAN;

    /* シグナリングNaN以外の無効演算はオペランドエラー */
    if ((s_fpst.float_exception_flags & float_flag_invalid) && !snan)
        REG_FPSR |= FPES_OPERR;

    if ((enables & EXC_ENB_INEXACT) && (s_fpst.float_exception_flags & float_flag_inexact))
        REG_FPSR |= FPES_INEXACT;
    if ((enables & EXC_ENB_UNDFLOW) && (s_fpst.float_exception_flags & float_flag_underflow))
        REG_FPSR |= FPES_UNDERFLOW;
    if ((enables & EXC_ENB_OVRFLOW) && (s_fpst.float_exception_flags & float_flag_overflow))
        REG_FPSR |= FPES_OVERFLOW;

    update_accrued_exceptions();
}

static int test_condition(int condition)
{
    int n = (REG_FPSR & FPCC_N) != 0;
    int z = (REG_FPSR & FPCC_Z) != 0;
    int nan = (REG_FPSR & FPCC_NAN) != 0;

    /* IEEE非対応の述語は、非順序のとき BSUN を立てる */
    if ((condition & 0x10) && nan)
        REG_FPSR |= FPES_BSUN | FPAE_IOP;

    switch (condition)
    {
        case 0x10: case 0x00: return 0;                       /* False */
        case 0x11: case 0x01: return z;                       /* Equal */
        case 0x12: case 0x02: return !(nan || z || n);        /* Greater Than */
        case 0x13: case 0x03: return z || !(nan || n);        /* Greater or Equal */
        case 0x14: case 0x04: return n && !(nan || z);        /* Less Than */
        case 0x15: case 0x05: return z || (n && !nan);        /* Less Than or Equal */
        case 0x16: case 0x06: return !nan && !z;
        case 0x17: case 0x07: return !nan;
        case 0x18: case 0x08: return nan;
        case 0x19: case 0x09: return nan || z;
        case 0x1a: case 0x0a: return nan || !(n || z);        /* Not Less Than or Equal */
        case 0x1b: case 0x0b: return nan || z || !n;          /* Not Less Than */
        case 0x1c: case 0x0c: return nan || (n && !z);        /* Not Greater or Equal Than */
        case 0x1d: case 0x0d: return nan || z || n;           /* Not Greater Than */
        case 0x1e: case 0x0e: return !z;                      /* Not Equal */
        case 0x1f: case 0x0f: return 1;                       /* True */
        default:
            fpu_fline(ST_FLINE_UNIMPL);   /* MAME: fatalerror(unhandled condition) */
            return 0;
    }
}

static int32_t convert_to_int(floatx80 source, int32_t lowerLimit, int32_t upperLimit)
{
    int32_t result;

    clear_exception_flags();
    result = floatx80_to_int32(source, &s_fpst);
    if (s_fpst.float_exception_flags & float_flag_invalid)
    {
        /* 桁あふれ・NaN: 実機は元の符号側の最大値を格納する(MAME と同じ扱い) */
        result = (source.high & 0x8000) ? lowerLimit : upperLimit;
    }
    sync_exception_flags(source, source, EXC_ENB_INEXACT);
    if (result < lowerLimit)
    {
        result = lowerLimit;
        REG_FPSR |= FPES_OPERR | FPAE_IOP;
    }
    else if (result > upperLimit)
    {
        result = upperLimit;
        REG_FPSR |= FPES_OPERR | FPAE_IOP;
    }
    return result;
}

/* バイト幅の (A7)+ / -(A7) は SP を2ずつ動かす(68000系の規則)。MAME は EA_AY_PI_8/PD_8 のままだった */
static inline uint ea_pi_8(void) { return ((REG_IR & 7) == 7) ? EA_A7_PI_8() : EA_AY_PI_8(); }
static inline uint ea_pd_8(void) { return ((REG_IR & 7) == 7) ? EA_A7_PD_8() : EA_AY_PD_8(); }

static uint8_t READ_EA_8(int ea)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);

    switch (mode)
    {
        case 0: return REG_D[reg] & 0xff;
        case 2: return m68ki_read_8(REG_A[reg]);
        case 3: return m68ki_read_8(ea_pi_8());
        case 4: return m68ki_read_8(ea_pd_8());
        case 5: return m68ki_read_8(EA_AY_DI_8());
        case 6: return m68ki_read_8(EA_AY_IX_8());
        case 7:
            switch (reg)
            {
                case 0: return m68ki_read_8(EA_AW_8());
                case 1: return m68ki_read_8(EA_AL_8());
                case 2: return m68ki_read_8(EA_PCDI_8());
                case 3: return m68ki_read_8(EA_PCIX_8());
                case 4: return OPER_I_8();
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
    return 0;
}

static uint16_t READ_EA_16(int ea)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);

    switch (mode)
    {
        case 0: return (uint16_t)(REG_D[reg] & 0xffff);
        case 2: return m68ki_read_16(REG_A[reg]);
        case 3: return m68ki_read_16(EA_AY_PI_16());
        case 4: return m68ki_read_16(EA_AY_PD_16());
        case 5: return m68ki_read_16(EA_AY_DI_16());
        case 6: return m68ki_read_16(EA_AY_IX_16());
        case 7:
            switch (reg)
            {
                case 0: return m68ki_read_16(EA_AW_16());
                case 1: return m68ki_read_16(EA_AL_16());
                case 2: return m68ki_read_16(EA_PCDI_16());
                case 3: return m68ki_read_16(EA_PCIX_16());
                case 4: return OPER_I_16();
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
    return 0;
}

static uint32_t READ_EA_32(int ea)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);

    switch (mode)
    {
        case 0: return REG_D[reg];
        case 1: return REG_A[reg];
        case 2: return m68ki_read_32(REG_A[reg]);
        case 3: return m68ki_read_32(EA_AY_PI_32());
        case 4: return m68ki_read_32(EA_AY_PD_32());
        case 5: return m68ki_read_32(EA_AY_DI_32());
        case 6: return m68ki_read_32(EA_AY_IX_32());
        case 7:
            switch (reg)
            {
                case 0: return m68ki_read_32(EA_AW_32());
                case 1: return m68ki_read_32(EA_AL_32());
                case 2: return m68ki_read_32(EA_PCDI_32());
                case 3: return m68ki_read_32(EA_PCIX_32());
                case 4: return OPER_I_32();
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
    return 0;
}

static uint64_t read_64(uint ea)
{
    uint32_t h1 = m68ki_read_32(ea + 0);
    uint32_t h2 = m68ki_read_32(ea + 4);
    return ((uint64_t)h1 << 32) | (uint64_t)h2;
}

static uint64_t READ_EA_64(int ea)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);
    uint addr;

    switch (mode)
    {
        case 2: return read_64(REG_A[reg]);
        case 3:
            addr = REG_A[reg];
            REG_A[reg] += 8;
            return read_64(addr);
        case 4:
            REG_A[reg] -= 8;
            return read_64(REG_A[reg]);
        case 5: return read_64(EA_AY_DI_32());
        case 6: return read_64(EA_AY_IX_32());
        case 7:
            switch (reg)
            {
                case 0: return read_64(EA_AW_32());
                case 1: return read_64(EA_AL_32());
                case 2: return read_64(EA_PCDI_32());
                case 3: return read_64(EA_PCIX_32());
                case 4:
                {
                    uint32_t h1 = OPER_I_32();
                    uint32_t h2 = OPER_I_32();
                    return ((uint64_t)h1 << 32) | (uint64_t)h2;
                }
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
    return 0;
}

/* 制御モード<ea>の番地を求める(拡張語を読む)。FMOVEMのレジスタループより前に1命令1回だけ呼ぶこと。
 * (An)+・-(An)・#<data> は転送ごとの副作用があるので READ_EA_FPE/WRITE_EA_FPE 側で処理する */
static uint GET_EA_FPE(int mode, int reg)
{
    switch (mode)
    {
        case 2: return REG_A[reg];
        case 3:
        case 4: return 0;
        case 5: return EA_AY_DI_32();
        case 6: return EA_AY_IX_32();
        case 7:
            switch (reg)
            {
                case 0: return EA_AW_32();
                case 1: return EA_AL_32();
                case 2: return EA_PCDI_32();
                case 3: return EA_PCIX_32();
                case 4: return 0;
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
    return 0;
}

static floatx80 READ_EA_FPE(int mode, int reg, uint address)
{
    floatx80 fpr = fx_make(0, 0);

    switch (mode)
    {
        case 2:
        case 5:
        case 6:
            fpr = load_extended_float80(address);
            break;
        case 3:
        {
            uint ea = REG_A[reg];
            REG_A[reg] += 12;
            fpr = load_extended_float80(ea);
            break;
        }
        case 4:
            REG_A[reg] -= 12;
            fpr = load_extended_float80(REG_A[reg]);
            break;
        case 7:
            switch (reg)
            {
                case 0: case 1: case 2: case 3:
                    fpr = load_extended_float80(address);
                    break;
                case 4:
                    fpr = load_extended_float80(REG_PC);
                    REG_PC += 12;
                    break;
                default:
                    fpu_fline(ST_FLINE_UNIMPL);
                    break;
            }
            break;
        default:
            fpu_fline(ST_FLINE_UNIMPL);
            break;
    }
    return fpr;
}

static void WRITE_EA_8(int ea, uint8_t data)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);

    switch (mode)
    {
        case 0:
            REG_D[reg] = (REG_D[reg] & 0xffffff00) | data;
            return;
        case 2: m68ki_write_8(REG_A[reg], data); return;
        case 3: m68ki_write_8(ea_pi_8(), data); return;
        case 4: m68ki_write_8(ea_pd_8(), data); return;
        case 5: m68ki_write_8(EA_AY_DI_8(), data); return;
        case 6: m68ki_write_8(EA_AY_IX_8(), data); return;
        case 7:
            switch (reg)
            {
                case 0: m68ki_write_8(EA_AW_8(), data); return;
                case 1: m68ki_write_8(EA_AL_8(), data); return;
                case 2: m68ki_write_8(EA_PCDI_16(), data); return;
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
}

static void WRITE_EA_16(int ea, uint16_t data)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);

    switch (mode)
    {
        case 0:
            REG_D[reg] = (REG_D[reg] & 0xffff0000) | data;
            return;
        case 2: m68ki_write_16(REG_A[reg], data); return;
        case 3: m68ki_write_16(EA_AY_PI_16(), data); return;
        case 4: m68ki_write_16(EA_AY_PD_16(), data); return;
        case 5: m68ki_write_16(EA_AY_DI_16(), data); return;
        case 6: m68ki_write_16(EA_AY_IX_16(), data); return;
        case 7:
            switch (reg)
            {
                case 0: m68ki_write_16(EA_AW_16(), data); return;
                case 1: m68ki_write_16(EA_AL_16(), data); return;
                case 2: m68ki_write_16(EA_PCDI_16(), data); return;
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
}

static void WRITE_EA_32(int ea, uint32_t data)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);

    switch (mode)
    {
        case 0: REG_D[reg] = data; return;
        case 1: REG_A[reg] = data; return;
        case 2: m68ki_write_32(REG_A[reg], data); return;
        case 3: m68ki_write_32(EA_AY_PI_32(), data); return;
        case 4: m68ki_write_32(EA_AY_PD_32(), data); return;
        case 5: m68ki_write_32(EA_AY_DI_32(), data); return;
        case 6: m68ki_write_32(EA_AY_IX_32(), data); return;
        case 7:
            switch (reg)
            {
                case 0: m68ki_write_32(EA_AW_32(), data); return;
                case 1: m68ki_write_32(EA_AL_32(), data); return;
                case 2: m68ki_write_32(EA_PCDI_32(), data); return;
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
}

static void write_64(uint ea, uint64_t data)
{
    m68ki_write_32(ea + 0, (uint32_t)(data >> 32));
    m68ki_write_32(ea + 4, (uint32_t)(data));
}

static void WRITE_EA_64(int ea, uint64_t data)
{
    int mode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);
    uint addr;

    switch (mode)
    {
        case 2: write_64(REG_A[reg], data); return;
        case 3:
            addr = REG_A[reg];
            REG_A[reg] += 8;
            write_64(addr, data);
            return;
        case 4:
            REG_A[reg] -= 8;
            write_64(REG_A[reg], data);
            return;
        case 5: write_64(EA_AY_DI_32(), data); return;
        case 6: write_64(EA_AY_IX_32(), data); return;
        case 7:
            switch (reg)
            {
                case 0: write_64(EA_AW_32(), data); return;
                case 1: write_64(EA_AL_32(), data); return;
                case 2: write_64(EA_PCDI_32(), data); return;
                default: break;
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
}

static void WRITE_EA_FPE(int mode, int reg, floatx80 fpr, uint address)
{
    switch (mode)
    {
        case 2:
        case 5:
        case 6:
            store_extended_float80(address, fpr);
            return;
        case 3:
        {
            uint ea = REG_A[reg];
            store_extended_float80(ea, fpr);
            REG_A[reg] += 12;
            return;
        }
        case 4:
            REG_A[reg] -= 12;
            store_extended_float80(REG_A[reg], fpr);
            return;
        case 7:
            switch (reg)
            {
                case 0:
                case 1:
                    store_extended_float80(address, fpr);
                    return;
                default: break;   /* PC相対・即値は書込み先として不正 */
            }
            break;
        default: break;
    }
    fpu_fline(ST_FLINE_UNIMPL);
}

/* FPCR の丸めモード・丸め精度を s_fpst へ反映する(MAME apply_fpcr_rounding と同じ対応)。
 * 精度フィールド(MODE byte bit7-6)の未定義値3は80bit扱い(MAME と同じ。Hatari fpp は64bit扱い)——P897 Fix Plan
 * の決定「MAME移植版の既定を採用」。リセット後の FPCR=0 では拡張精度(80bit)になる。 */
static void apply_fpcr_rounding(void)
{
    switch ((REG_FPCR >> 6) & 3)
    {
        case 0: s_fpst.floatx80_rounding_precision = 80; break;   /* Extend (X) */
        case 1: s_fpst.floatx80_rounding_precision = 32; break;   /* Single (S) */
        case 2: s_fpst.floatx80_rounding_precision = 64; break;   /* Double (D) */
        case 3: s_fpst.floatx80_rounding_precision = 80; break;   /* Undefined */
    }

    /* Hatari の列挙値の並びは RN,RM,RP,RZ = 0,1,2,3(FPCR の RN,RZ,RM,RP = 0,1,2,3 とは異なる) */
    switch ((REG_FPCR >> 4) & 3)
    {
        case 0: s_fpst.float_rounding_mode = float_round_nearest_even; break;   /* To Nearest (RN) */
        case 1: s_fpst.float_rounding_mode = float_round_to_zero; break;        /* To Zero (RZ) */
        case 2: s_fpst.float_rounding_mode = float_round_down; break;           /* To Minus Infinity (RM) */
        case 3: s_fpst.float_rounding_mode = float_round_up; break;             /* To Plus Infinity (RP) */
    }
}

/* FMOVECR の未定義番号 $01-$0A に 68882 実機が返す値(XEiJ 0.26.01.08 EFPBox.java:276-288 の実機ダンプ値)。
   XEiJ・WinUAE ともコードは移植せず、事実データ(実機ダンプ値のビット列)のみを参照する */
static const struct { uint16_t high; uint64_t low; } k_fmovecr_undef_68882[10] = {
    { 0x4001, 0xfe00068200000000ULL },   /* $01 */
    { 0x4001, 0xffc0050380000000ULL },   /* $02 */
    { 0x2000, 0x7fffffff00000000ULL },   /* $03(実機は FPSR N も立てる、未対応) */
    { 0x0000, 0xffffffffffffffffULL },   /* $04 */
    { 0x3c00, 0xfffffffffffff800ULL },   /* $05 */
    { 0x3f80, 0xffffff0000000000ULL },   /* $06 */
    { 0x0001, 0xf65d8d9c00000000ULL },   /* $07(実機は FPSR N も立てる、未対応) */
    { 0x7fff, 0x401e000000000000ULL },   /* $08(NaN のビット列) */
    { 0x43f3, 0xe000000000000000ULL },   /* $09 */
    { 0x4072, 0xc000000000000000ULL },   /* $0A */
};

/* FMOVECR の定数ROM。未定義の番号: 68882 は実機ダンプ値・68881 は 0(XEiJ 0.26.01.08 準拠)、
   $10-$2F は両モデルとも 0、$40-$7F は F-line(0 を返し呼出側が line-1111)。出典: XEiJ EFPBox.java:238-294, 21081-21085 */
static int fmovecr_constant(int offset, floatx80 *out)
{
    switch (offset)
    {
        case 0x00: *out = fx_make(0x4000, 0xc90fdaa22168c235ULL); return 1;   /* Pi */
        case 0x0b: *out = fx_make(0x3ffd, 0x9a209a84fbcff798ULL); return 1;   /* log10(2) */
        case 0x0c: *out = fx_make(0x4000, 0xadf85458a2bb4a9bULL); return 1;   /* e */
        case 0x0d: *out = fx_make(0x3fff, 0xb8aa3b295c17f0bcULL); return 1;   /* log2(e) */
        case 0x0e: *out = fx_make(0x3ffd, 0xde5bd8a937287195ULL); return 1;   /* log10(e) */
        case 0x0f: *out = int32_to_floatx80(0); return 1;                     /* 0.0 */
        case 0x30: *out = fx_make(0x3ffe, 0xb17217f7d1cf79acULL); return 1;   /* ln(2) */
        case 0x31: *out = fx_make(0x4000, 0x935d8dddaaa8ac17ULL); return 1;   /* ln(10) */
        case 0x32: *out = int32_to_floatx80(1); return 1;                     /* 10^0 */
        case 0x33: *out = int32_to_floatx80(10); return 1;                    /* 10^1 */
        case 0x34: *out = int32_to_floatx80(10 * 10); return 1;               /* 10^2 */
        case 0x35: *out = int32_to_floatx80(1000 * 10); return 1;             /* 10^4 */
        case 0x36: *out = int32_to_floatx80(10000000 * 10); return 1;         /* 10^8 */
        case 0x37: *out = fx_make(0x4034, 0x8e1bc9bf04000000ULL); return 1;   /* 10^16 */
        case 0x38: *out = fx_make(0x4069, 0x9dc5ada82b70b59eULL); return 1;   /* 10^32 */
        case 0x39: *out = fx_make(0x40d3, 0xc2781f49ffcfa6d5ULL); return 1;   /* 10^64 */
        case 0x3a: *out = fx_make(0x41a8, 0x93ba47c980e98ce0ULL); return 1;   /* 10^128 */
        case 0x3b: *out = fx_make(0x4351, 0xaa7eebfb9df9de8eULL); return 1;   /* 10^256 */
        case 0x3c: *out = fx_make(0x46a3, 0xe319a0aea60e91c7ULL); return 1;   /* 10^512 */
        case 0x3d: *out = fx_make(0x4d48, 0xc976758681750c17ULL); return 1;   /* 10^1024 */
        case 0x3e: *out = fx_make(0x5a92, 0x9e8b3b5dc53d5de5ULL); return 1;   /* 10^2048 */
        case 0x3f: *out = fx_make(0x7525, 0xc46052028a20979bULL); return 1;   /* 10^4096 */
        default:
            if (offset >= 0x01 && offset <= 0x0a)
            {
                if (s_mx_fpu_model == 68882)
                    *out = fx_make(k_fmovecr_undef_68882[offset - 1].high, k_fmovecr_undef_68882[offset - 1].low);
                else
                    *out = int32_to_floatx80(0);
                return 1;
            }
            if (offset >= 0x10 && offset <= 0x2f)
            {
                *out = int32_to_floatx80(0);
                return 1;
            }
            return 0;   /* $40-$7F: 実機も F-line */
    }
}

static void fpgen_rm_reg(uint16_t w2)
{
    int ea = REG_IR & 0x3f;
    int rm = (w2 >> 14) & 0x1;
    int src = (w2 >> 10) & 0x7;
    int dst = (w2 >> 7) & 0x7;
    int opmode = w2 & 0x7f;
    floatx80 source;
    floatx80 dstCopy;

    s_st[ST_FGEN]++;

    /* 演算命令は例外ハンドラ用に自身の番地を記録する */
    REG_FPIAR = REG_PPC;

    /* オペランドを読む前に opmode を検査する(未定義の符号化と68040専用の丸め形式は
     * 68881/68882 では、EAの副作用より前に line-1111 になる) */
    if (!(rm && src == 7))
    {
        int valid;
        if (opmode & 0x40)
        {
            valid = 0;   /* 68040専用(FSxxx/FDxxx)。EC030+6888x では常に無効 */
        }
        else
        {
            const uint64_t VALID_6888X = 0xfffffffff777f75fULL;   /* 6888x の有効 opmode のマスク */
            valid = (int)((VALID_6888X >> opmode) & 1);
        }
        if (!valid)
        {
            fpu_fline(ST_FLINE_FORMAT);
            return;
        }
    }

    if (rm)
    {
        switch (src)
        {
            case 0:   /* Long-Word Integer */
            {
                int32_t d = (int32_t)READ_EA_32(ea);
                if (s_fpu_abort) return;
                source = int32_to_floatx80(d);
                break;
            }
            case 1:   /* Single-precision Real */
            {
                uint32_t d = READ_EA_32(ea);
                if (s_fpu_abort) return;
                source = float32_to_floatx80(d, &s_fpst);
                break;
            }
            case 2:   /* Extended-precision Real */
            {
                int imode = (ea >> 3) & 0x7;
                int reg = (ea & 0x7);
                uint address = GET_EA_FPE(imode, reg);
                if (s_fpu_abort) return;
                source = READ_EA_FPE(imode, reg, address);
                if (s_fpu_abort) return;
                break;
            }
            case 3:   /* Packed-decimal Real: 未対応(EAを読む前に line-1111、将来の拡張点) */
                fpu_fline(ST_FLINE_PACK);
                return;
            case 4:   /* Word Integer */
            {
                int16_t d = (int16_t)READ_EA_16(ea);
                if (s_fpu_abort) return;
                source = int32_to_floatx80((int32_t)d);
                break;
            }
            case 5:   /* Double-precision Real */
            {
                uint64_t d = READ_EA_64(ea);
                if (s_fpu_abort) return;
                source = float64_to_floatx80(d, &s_fpst);
                break;
            }
            case 6:   /* Byte Integer */
            {
                int8_t d = (int8_t)READ_EA_8(ea);
                if (s_fpu_abort) return;
                source = int32_to_floatx80((int32_t)d);
                break;
            }
            case 7:   /* FMOVECR: 定数ROMからの読み込み(通常の opmode は無効なのでここで完結) */
            default:
                if (!fmovecr_constant(w2 & 0x7f, &source))
                {
                    fpu_fline(ST_FLINE_UNIMPL);
                    return;
                }
                REG_FP[dst] = source;
                set_condition_codes(REG_FP[dst]);
                USE_CYCLES(4);
                return;
        }
    }
    else
    {
        source = REG_FP[src];
    }

    dstCopy = REG_FP[dst];
    if (opmode != 0)
        clear_exception_flags();

    switch (opmode)
    {
        case 0x00:   /* FMOVE */
            REG_FP[dst] = source;
            set_condition_codes(REG_FP[dst]);
            USE_CYCLES(56);
            break;
        case 0x01:   /* FINT */
            REG_FP[dst] = floatx80_round_to_int(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(78);
            break;
        case 0x02:   /* FSINH(Hatari の専用関数。MAME は etox からの合成) */
            REG_FP[dst] = floatx80_sinh(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(687);
            break;
        case 0x03:   /* FINTRZ */
            REG_FP[dst] = floatx80_round_to_int_toward_zero(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(78);
            break;
        case 0x04:   /* FSQRT */
            REG_FP[dst] = floatx80_sqrt(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(109);
            break;
        case 0x06:   /* FLOGNP1 */
            REG_FP[dst] = floatx80_lognp1(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(594);
            break;
        case 0x08:   /* FETOXM1(Hatari の専用関数) */
            REG_FP[dst] = floatx80_etoxm1(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_UNDFLOW);
            USE_CYCLES(568);
            break;
        case 0x09:   /* FTANH(Hatari の専用関数) */
            REG_FP[dst] = floatx80_tanh(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(661);
            break;
        case 0x0a:   /* FATAN */
            REG_FP[dst] = floatx80_atan(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(426);
            break;
        case 0x0c:   /* FASIN(Hatari の専用関数) */
            REG_FP[dst] = floatx80_asin(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(581);
            break;
        case 0x0d:   /* FATANH(Hatari の専用関数) */
            REG_FP[dst] = floatx80_atanh(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(693);
            break;
        case 0x0e:   /* FSIN(Hatari は関数内で引数を範囲縮小するので MAME の再試行は不要) */
            REG_FP[dst] = floatx80_sin(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(414);
            break;
        case 0x0f:   /* FTAN */
            REG_FP[dst] = floatx80_tan(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(496);
            break;
        case 0x10:   /* FETOX */
            REG_FP[dst] = floatx80_etox(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_UNDFLOW);
            USE_CYCLES(520);
            break;
        case 0x11:   /* FTWOTOX */
            REG_FP[dst] = floatx80_twotox(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_UNDFLOW);
            USE_CYCLES(590);
            break;
        case 0x12:   /* FTENTOX */
            REG_FP[dst] = floatx80_tentox(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_UNDFLOW);
            USE_CYCLES(590);
            break;
        case 0x14:   /* FLOGN */
            REG_FP[dst] = floatx80_logn(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(548);
            break;
        case 0x15:   /* FLOG10 */
            REG_FP[dst] = floatx80_log10(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(604);
            break;
        case 0x16:   /* FLOG2 */
            REG_FP[dst] = floatx80_log2(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(604);
            break;
        case 0x18:   /* FABS */
            REG_FP[dst] = source;
            REG_FP[dst].high &= 0x7fff;
            set_condition_codes(REG_FP[dst]);
            USE_CYCLES(58);
            break;
        case 0x19:   /* FCOSH(Hatari の専用関数) */
            REG_FP[dst] = floatx80_cosh(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW);
            USE_CYCLES(607);
            break;
        case 0x1a:   /* FNEG */
            REG_FP[dst] = source;
            REG_FP[dst].high ^= 0x8000;
            set_condition_codes(REG_FP[dst]);
            USE_CYCLES(58);
            break;
        case 0x1c:   /* FACOS(Hatari の専用関数) */
            REG_FP[dst] = floatx80_acos(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(604);
            break;
        case 0x1d:   /* FCOS */
            REG_FP[dst] = floatx80_cos(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, EXC_ENB_INEXACT);
            USE_CYCLES(414);
            break;
        case 0x1e:   /* FGETEXP(MAME はホスト double 経由。Hatari の専用関数で置き換え) */
            REG_FP[dst] = floatx80_getexp(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, 0);   /* 例外になりうるのは NaN・無限大だけ */
            USE_CYCLES(68);
            break;
        case 0x1f:   /* FGETMAN */
            REG_FP[dst] = floatx80_getman(source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, source, 0);
            USE_CYCLES(54);
            break;
        case 0x20:   /* FDIV(MAME fpu_div の非040経路) */
        {
            floatx80 result = floatx80_div(dstCopy, source, &s_fpst);
            if (s_fpst.float_exception_flags & float_flag_divbyzero)
                REG_FPSR |= FPES_DIVZERO;
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            set_condition_codes(result);
            REG_FP[dst] = result;
            USE_CYCLES(128);
            break;
        }
        case 0x21:   /* FMOD */
        {
            uint64_t quotient = 0;
            flag qsign = 0;
            REG_FP[dst] = floatx80_mod(dstCopy, source, &quotient, &qsign, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            REG_FPSR &= 0xff00ffff;
            REG_FPSR |= (uint)(quotient & 0x7f) << 16;
            /* 符号ビットは剰余ではなく商の符号 */
            if ((dstCopy.high ^ source.high) & 0x8000)
                REG_FPSR |= 0x00800000;
            USE_CYCLES(95);
            break;
        }
        case 0x22:   /* FADD */
            REG_FP[dst] = floatx80_add(dstCopy, source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(76);
            break;
        case 0x23:   /* FMUL */
            REG_FP[dst] = floatx80_mul(dstCopy, source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(96);
            break;
        case 0x24:   /* FSGLDIV(MAME は f32 演算で代用。Hatari の専用関数で置き換え) */
            REG_FP[dst] = floatx80_sgldiv(dstCopy, source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(94);
            break;
        case 0x25:   /* FREM */
        {
            uint64_t quotient = 0;
            flag qsign = 0;
            REG_FP[dst] = floatx80_rem(dstCopy, source, &quotient, &qsign, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(dstCopy, source, EXC_ENB_UNDFLOW);
            REG_FPSR &= 0xff00ffff;
            REG_FPSR |= (uint)(quotient & 0x7f) << 16;
            if ((dstCopy.high ^ source.high) & 0x8000)
                REG_FPSR |= 0x00800000;
            USE_CYCLES(125);
            break;
        }
        case 0x26:   /* FSCALE */
            REG_FP[dst] = floatx80_scale(dstCopy, source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(66);
            break;
        case 0x27:   /* FSGLMUL(Hatari の専用関数) */
            REG_FP[dst] = floatx80_sglmul(dstCopy, source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(94);
            break;
        case 0x28: case 0x29: case 0x2a: case 0x2b:
        case 0x2c: case 0x2d: case 0x2e: case 0x2f:   /* FSUB */
            REG_FP[dst] = floatx80_sub(dstCopy, source, &s_fpst);
            set_condition_codes(REG_FP[dst]);
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            USE_CYCLES(76);
            break;
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:   /* FSINCOS */
        {
            floatx80 cosine;
            floatx80 sine = floatx80_sincos(source, &cosine, &s_fpst);
            /* FPs と FPc が同じレジスタなら sin の結果が残る */
            REG_FP[w2 & 7] = cosine;
            REG_FP[dst] = sine;
            set_condition_codes(REG_FP[dst]);   /* CC は sin の結果で立てる */
            sync_exception_flags(source, dstCopy, EXC_ENB_INEXACT | EXC_ENB_UNDFLOW);
            USE_CYCLES(474);
            break;
        }
        case 0x38: case 0x39: case 0x3c: case 0x3d:   /* FCMP */
            /* 減算ではなく比較: 同符号の無限大は等しく、quiet NaN はオペランドエラーにしない。
             * I は常に消え、等しいとき N はデスティネーションの符号を表す */
            REG_FPSR &= ~(FPCC_N | FPCC_Z | FPCC_I | FPCC_NAN);
            if (fx_is_nan(dstCopy) || fx_is_nan(source))
            {
                REG_FPSR |= FPCC_NAN;
            }
            else if (floatx80_eq(dstCopy, source, &s_fpst))
            {
                REG_FPSR |= FPCC_Z;
                if (dstCopy.high & 0x8000)
                    REG_FPSR |= FPCC_N;
            }
            else if (floatx80_lt(dstCopy, source, &s_fpst))
            {
                REG_FPSR |= FPCC_N;
            }
            sync_exception_flags(source, dstCopy, 0);
            USE_CYCLES(58);
            break;
        case 0x3a: case 0x3b: case 0x3e: case 0x3f:   /* FTST */
            set_condition_codes(source);
            sync_exception_flags(source, dstCopy, 0);
            USE_CYCLES(56);
            break;
        default:   /* VALID_6888X で弾いているので到達しない(MAME: fatalerror) */
            fpu_fline(ST_FLINE_UNIMPL);
            break;
    }
}

static void fmove_reg_mem(uint16_t w2)
{
    int ea = REG_IR & 0x3f;
    int src = (w2 >> 7) & 0x7;
    int dst = (w2 >> 10) & 0x7;

    s_st[ST_FMOVE_OUT]++;

    /* パック10進(静的/動的Kファクタ)は未対応。EAを読む前に line-1111(将来の拡張点) */
    if (dst == 3 || dst == 7)
    {
        fpu_fline(ST_FLINE_PACK);
        return;
    }

    REG_FPIAR = REG_PPC;

    switch (dst)
    {
        case 0:   /* Long-Word Integer */
        {
            int32_t d = convert_to_int(REG_FP[src], INT32_MIN, INT32_MAX);
            WRITE_EA_32(ea, (uint32_t)d);
            break;
        }
        case 1:   /* Single-precision Real */
        {
            uint32_t d;
            clear_exception_flags();
            d = floatx80_to_float32(REG_FP[src], &s_fpst);
            sync_exception_flags(REG_FP[src], REG_FP[src], EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            WRITE_EA_32(ea, d);
            break;
        }
        case 2:   /* Extended-precision Real */
        {
            int mode = (ea >> 3) & 0x7;
            int reg = (ea & 0x7);
            uint address = GET_EA_FPE(mode, reg);
            if (s_fpu_abort) return;
            WRITE_EA_FPE(mode, reg, REG_FP[src], address);
            break;
        }
        case 4:   /* Word Integer */
        {
            int16_t value = (int16_t)convert_to_int(REG_FP[src], INT16_MIN, INT16_MAX);
            WRITE_EA_16(ea, (uint16_t)value);
            break;
        }
        case 5:   /* Double-precision Real */
        {
            uint64_t d;
            clear_exception_flags();
            d = floatx80_to_float64(REG_FP[src], &s_fpst);
            sync_exception_flags(REG_FP[src], REG_FP[src], EXC_ENB_INEXACT | EXC_ENB_OVRFLOW | EXC_ENB_UNDFLOW);
            WRITE_EA_64(ea, d);
            break;
        }
        case 6:   /* Byte Integer */
        {
            int8_t value = (int8_t)convert_to_int(REG_FP[src], INT8_MIN, INT8_MAX);
            WRITE_EA_8(ea, (uint8_t)value);
            break;
        }
        default:
            break;
    }
    if (s_fpu_abort) return;
    USE_CYCLES(12);
}

static void fmove_fpcr(uint16_t w2)
{
    int ea = REG_IR & 0x3f;
    int dir = (w2 >> 13) & 0x1;
    int regsel = (w2 >> 10) & 0x7;
    int reg = ea & 7;
    int mode = (ea >> 3) & 0x7;
    uint address = 0;

    s_st[ST_FMOVE_CTL]++;

    switch (mode)
    {
        case 0:   /* Dn */
        case 1:   /* An */
        case 3:   /* (An)+ */
            break;
        case 2:   /* (An) */
            address = REG_A[reg];
            break;
        case 4:   /* -(An): 総量を先に引き、昇順の番地へ転送する(FPCR が最下位) */
            REG_A[reg] -= 4 * (uint)__builtin_popcount((unsigned int)regsel);
            address = REG_A[reg];
            break;
        case 5:
            address = EA_AY_DI_32();
            break;
        case 6:
            address = EA_AY_IX_32();
            break;
        case 7:
            switch (reg)
            {
                case 0: address = EA_AW_32(); break;
                case 1: address = EA_AL_32(); break;
                case 2: address = EA_PCDI_32(); break;
                case 3: address = EA_PCIX_32(); break;
                case 4:   /* #<data> */
                    if (regsel & 4)
                    {
                        REG_FPCR = OPER_I_32() & FPCR_WRITE_MASK;
                        apply_fpcr_rounding();
                    }
                    if (regsel & 2) REG_FPSR = OPER_I_32() & FPSR_WRITE_MASK;
                    if (regsel & 1) REG_FPIAR = OPER_I_32();
                    USE_CYCLES(30);
                    return;
                default:
                    fpu_fline(ST_FLINE_UNIMPL);
                    return;
            }
            break;
    }

    switch (mode)
    {
        case 0:
        case 1:
        case 3:
            if (dir)   /* 制御レジスタ → <ea> */
            {
                if (regsel & 4) WRITE_EA_32(ea, REG_FPCR);
                if (regsel & 2) WRITE_EA_32(ea, REG_FPSR);
                if (regsel & 1) WRITE_EA_32(ea, REG_FPIAR);
            }
            else       /* <ea> → 制御レジスタ */
            {
                if (regsel & 4) REG_FPCR = READ_EA_32(ea) & FPCR_WRITE_MASK;
                if (regsel & 2) REG_FPSR = READ_EA_32(ea) & FPSR_WRITE_MASK;
                if (regsel & 1) REG_FPIAR = READ_EA_32(ea);
            }
            break;
        default:
            if (dir)
            {
                if (regsel & 4) { m68ki_write_32(address, REG_FPCR); address += 4; }
                if (regsel & 2) { m68ki_write_32(address, REG_FPSR); address += 4; }
                if (regsel & 1) { m68ki_write_32(address, REG_FPIAR); address += 4; }
            }
            else
            {
                if (regsel & 4) { REG_FPCR = m68ki_read_32(address) & FPCR_WRITE_MASK; address += 4; }
                if (regsel & 2) { REG_FPSR = m68ki_read_32(address) & FPSR_WRITE_MASK; address += 4; }
                if (regsel & 1) { REG_FPIAR = m68ki_read_32(address); address += 4; }
            }
            break;
    }

    if ((regsel & 4) && dir == 0)
        apply_fpcr_rounding();

    USE_CYCLES(30);
}

static void fmovem(uint16_t w2)
{
    int i;
    int ea = REG_IR & 0x3f;
    int dir = (w2 >> 13) & 0x1;
    int mode = (w2 >> 11) & 0x3;
    int reglist = w2 & 0xff;
    int imode = (ea >> 3) & 0x7;
    int reg = (ea & 0x7);
    uint address;

    s_st[ST_FMOVEM]++;

    if (dir)   /* FPレジスタ → メモリ */
    {
        switch (mode)
        {
            case 1:   /* 動的リスト・プリデクリメント */
                reglist = REG_D[(reglist >> 4) & 7];
                /* fallthrough */
            case 0:   /* 静的リスト・プリデクリメント */
                address = GET_EA_FPE(imode, reg);
                if (s_fpu_abort) return;
                /* プリデクリメントのリストは bit0 が FP0。転送は FP7 から降順の番地へ(FP0 が最下位番地) */
                for (i = 7; i >= 0; i--)
                {
                    if (reglist & (1 << i))
                    {
                        WRITE_EA_FPE(imode, reg, REG_FP[i], address);
                        if (s_fpu_abort) return;
                        address += 12;
                        USE_CYCLES(2);
                    }
                }
                break;
            case 3:   /* 動的リスト・ポストインクリメント/制御モード */
                reglist = REG_D[(reglist >> 4) & 7];
                /* fallthrough */
            case 2:   /* 静的リスト・ポストインクリメント/制御モード */
                address = GET_EA_FPE(imode, reg);
                if (s_fpu_abort) return;
                /* こちらのリストは bit7 が FP0。転送は FP0 から昇順の番地へ */
                for (i = 0; i < 8; i++)
                {
                    if (reglist & (0x80 >> i))
                    {
                        WRITE_EA_FPE(imode, reg, REG_FP[i], address);
                        if (s_fpu_abort) return;
                        address += 12;
                        USE_CYCLES(2);
                    }
                }
                break;
        }
    }
    else       /* メモリ → FPレジスタ */
    {
        switch (mode)
        {
            case 3:
                reglist = REG_D[(reglist >> 4) & 7];
                /* fallthrough */
            case 2:
                address = GET_EA_FPE(imode, reg);
                if (s_fpu_abort) return;
                for (i = 0; i < 8; i++)
                {
                    if (reglist & (0x80 >> i))
                    {
                        floatx80 v = READ_EA_FPE(imode, reg, address);
                        if (s_fpu_abort) return;
                        REG_FP[i] = v;
                        address += 12;
                        USE_CYCLES(2);
                    }
                }
                break;
            default:   /* MAME: fatalerror(mode unimplemented) */
                fpu_fline(ST_FLINE_UNIMPL);
                break;
        }
    }
}

/* FScc。条件語の後に<ea>の拡張語が続く */
static void fscc(int mode, int reg)
{
    int cond;
    uint8_t v;

    s_st[ST_FSCC]++;
    m68ki_cpu.fpu_just_reset = 0;

    cond = test_condition(OPER_I_16() & 0x3f);
    if (s_fpu_abort) return;
    v = cond ? 0xff : 0x00;

    switch (mode)
    {
        case 0: REG_D[reg] = (REG_D[reg] & 0xffffff00) | v; break;
        case 2: m68ki_write_8(REG_A[reg], v); break;
        case 3: m68ki_write_8(ea_pi_8(), v); break;
        case 4: m68ki_write_8(ea_pd_8(), v); break;
        case 5: m68ki_write_8(EA_AY_DI_8(), v); break;
        case 6: m68ki_write_8(EA_AY_IX_8(), v); break;
        case 7:
            if (reg == 0) { m68ki_write_8(EA_AW_8(), v); break; }
            if (reg == 1) { m68ki_write_8(EA_AL_8(), v); break; }
            fpu_fline(ST_FLINE_FORMAT);
            break;
        default:
            fpu_fline(ST_FLINE_FORMAT);
            break;
    }
}

/* FDBcc(★現状はジャンプテーブル上 cpdbcc に奪われ到達しない、ファイル先頭の注記参照) */
static void fdbcc(void)
{
    int condition;

    s_st[ST_FDBCC]++;
    m68ki_cpu.fpu_just_reset = 0;

    condition = OPER_I_16() & 0x3f;
    if (!test_condition(condition))
    {
        uint *r_dst;
        uint res;
        if (s_fpu_abort) return;
        r_dst = &REG_D[REG_IR & 7];
        res = MASK_OUT_ABOVE_16(*r_dst - 1);
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | res;
        if (res != 0xffff)
        {
            uint offset = OPER_I_16();
            REG_PC -= 2;
            m68ki_trace_t0();
            m68ki_branch_16(offset);
        }
        else
        {
            REG_PC += 2;
        }
    }
    else
    {
        REG_PC += 2;
    }
    USE_CYCLES(7);
}

/* FBcc.W/.L。分岐先は拡張語の番地 + 変位。
 * MAME は m68ki_branch_16(offset-2) だが、offset=-32768 で16bitに切り詰められるため32bit加算にした */
static void fbcc(int is32)
{
    int32_t offset;
    int condition = REG_IR & 0x3f;

    s_st[ST_FBCC]++;
    m68ki_cpu.fpu_just_reset = 0;

    if (is32)
        offset = (int32_t)OPER_I_32() - 4;
    else
        offset = (int32_t)(int16_t)OPER_I_16() - 2;

    if (test_condition(condition))
    {
        m68ki_trace_t0();
        m68ki_branch_32((uint)offset);
    }
    if (s_fpu_abort) return;
    USE_CYCLES(7);
}

/* FTRAPcc(★現状はジャンプテーブル上 cptrapcc に奪われ到達しない、ファイル先頭の注記参照) */
static void ftrap(void)
{
    uint16_t w2;

    s_st[ST_FTRAP]++;
    m68ki_cpu.fpu_just_reset = 0;

    w2 = OPER_I_16();
    /* トラップ時に次の命令の番地が積まれるよう、判定より先にオペランドを読み飛ばす */
    switch (REG_IR & 0x7)
    {
        case 2: OPER_I_16(); break;   /* word operand */
        case 3: OPER_I_32(); break;   /* long word operand */
        default: break;               /* no operand */
    }

    if (test_condition(w2 & 0x3f))
        m68ki_exception_trap(EXCEPTION_TRAPV);
}

/* NULLフレームでの FRESTORE はFPUを初期化する: FP0-7=NaN、FPCR/FPSR/FPIAR=0
 * (一次資料 §6.4.2.1 p.6-31、FSAVE記述 p.4-88) */
static void do_frestore_null(void)
{
    int i;

    REG_FPCR = 0;
    REG_FPSR = 0;
    REG_FPIAR = 0;
    apply_fpcr_rounding();
    for (i = 0; i < 8; i++)
        REG_FP[i] = fx_make(0x7fff, 0xffffffffffffffffULL);

    /* リセット直後/NULL復元後にFPU命令が未実行なら、FSAVE は NULL フレームを書く */
    m68ki_cpu.fpu_just_reset = 1;
}

/* FSAVE で書くフレームを words へ組み立て、ロング語数を返す */
static int build_fsave_frame(uint32_t *words)
{
    int n, i;

    if (m68ki_cpu.fpu_just_reset)
    {
        words[0] = 0;   /* NULL フレーム(先頭ロングのみ、version=0) */
        s_st[ST_FSAVE_NULL]++;
        return 1;
    }
    if (s_mx_fpu_model == 68881)
    {
        n = 7;          /* 28バイト */
        words[0] = (FSAVE_VERSION << 24) | (FSAVE_IDLE_68881 << 16);
    }
    else
    {
        n = 15;         /* 60バイト */
        words[0] = (FSAVE_VERSION << 24) | (FSAVE_IDLE_68882 << 16);
    }
    for (i = 1; i < n - 1; i++)
        words[i] = 0;
    words[n - 1] = FSAVE_BIU_FLAGS;   /* BIU flags(68881: +$18、68882: +$38) */
    return n;
}

static void fpu_fsave(void)
{
    int mode = (REG_IR >> 3) & 7;
    int reg = REG_IR & 7;
    uint32_t words[15];
    uint addr;
    int n, i;

    /* 制御・可変モードと -(An) のみ。それ以外の<ea>は line-1111(MC68030 UM §10.2.3.3.1) */
    switch (mode)
    {
        case 2: addr = REG_A[reg]; break;
        case 4: addr = 0; break;
        case 5: addr = EA_AY_DI_32(); break;
        case 6: addr = EA_AY_IX_32(); break;
        case 7:
            if (reg == 0) { addr = EA_AW_32(); break; }
            if (reg == 1) { addr = EA_AL_32(); break; }
            fpu_fline(ST_FLINE_FORMAT);
            return;
        default:
            fpu_fline(ST_FLINE_FORMAT);
            return;
    }
    if (!FLAG_S)
    {
        s_st[ST_PRIV]++;
        m68ki_exception_privilege_violation();
        return;
    }

    s_st[ST_FSAVE]++;
    n = build_fsave_frame(words);
    if (mode == 4)
    {
        REG_A[reg] -= (uint)(4 * n);
        addr = REG_A[reg];
    }
    for (i = 0; i < n; i++)
        m68ki_write_32(addr + 4 * (uint)i, words[i]);
}

static void fpu_frestore(void)
{
    int mode = (REG_IR >> 3) & 7;
    int reg = REG_IR & 7;
    uint addr;
    uint32_t fmt;
    uint total;

    /* 制御モードと (An)+ のみ */
    switch (mode)
    {
        case 2:
        case 3: addr = REG_A[reg]; break;
        case 5: addr = EA_AY_DI_32(); break;
        case 6: addr = EA_AY_IX_32(); break;
        case 7:
            switch (reg)
            {
                case 0: addr = EA_AW_32(); break;
                case 1: addr = EA_AL_32(); break;
                case 2: addr = EA_PCDI_32(); break;
                case 3: addr = EA_PCIX_32(); break;
                default:
                    fpu_fline(ST_FLINE_FORMAT);
                    return;
            }
            break;
        default:
            fpu_fline(ST_FLINE_FORMAT);
            return;
    }
    if (!FLAG_S)
    {
        s_st[ST_PRIV]++;
        m68ki_exception_privilege_violation();
        return;
    }

    s_st[ST_FRESTORE]++;
    fmt = m68ki_read_32(addr);
    if ((fmt & 0xff000000u) == 0)
    {
        s_st[ST_FRESTORE_NULL]++;
        do_frestore_null();
        total = 4;
    }
    else
    {
        uint size = (fmt >> 16) & 0xff;
        m68ki_cpu.fpu_just_reset = 0;
        if (size == FSAVE_IDLE_68881 || size == FSAVE_IDLE_68882 || size == FSAVE_BUSY_68881 || size == FSAVE_BUSY_68882)
        {
            /* P897移植忠実性レビュー指摘: BUSYフレームはサイズ分スキップするのみで
             * 内部状態(演算継続中のコンテキスト)は復元しない。本エミュレーションは
             * FPU命令を単一ステップで完結させる設計のためFSAVEがBUSYフレームを
             * 生成することは無く、現状は実害なし——ただし外部で生成されたBUSY
             * フレームをFRESTOREした場合、継続演算の再開はできない(既知の制限)。 */
            total = 4 + size;
        }
        else
        {
            /* 未知の size(実機はフォーマットエラー例外)。MAME と同じく先頭ロングだけ進める */
            s_st[ST_FRESTORE_BADFMT]++;
            total = 4;
        }
    }
    if (mode == 3)
        REG_A[reg] = addr + total;
}

/* F2xx(cpID=1、type 000-011)。FPU装着の検査は拡張語を読む前のここ(とop1)だけで行う */
void m68040_fpu_op0(void)
{
    s_fpu_abort = 0;
    if (!s_mx_fpu_present)
    {
        if (mx68k_musashi_stub_hook) mx68k_musashi_stub_hook(MX68K_STUB_FPU_FLINE, REG_PPC, REG_IR, 0);
        m68ki_exception_1111();   /* 積まれるPCは REG_PPC(命令先頭)なので、ここまでの読み出し語数に依存しない */
        return;
    }
    s_st[ST_OP0]++;

    switch ((REG_IR >> 6) & 7)
    {
        case 0:   /* 一般形式: 命令語2 の bit15-13 で振り分け */
        {
            uint16_t w2;
            m68ki_cpu.fpu_just_reset = 0;
            w2 = OPER_I_16();
            switch ((w2 >> 13) & 0x7)
            {
                case 0:   /* FP → FP の演算 */
                case 2:   /* <ea> → FP の演算(FMOVECR を含む) */
                    fpgen_rm_reg(w2);
                    break;
                case 3:   /* FMOVE FP → <ea> */
                    fmove_reg_mem(w2);
                    break;
                case 4:   /* FMOVE(M) <ea> → 制御レジスタ */
                case 5:   /* FMOVE(M) 制御レジスタ → <ea> */
                    fmove_fpcr(w2);
                    break;
                case 6:   /* FMOVEM <ea> → FP0-7 */
                case 7:   /* FMOVEM FP0-7 → <ea> */
                    fmovem(w2);
                    break;
                default:  /* 001: 未定義 */
                    fpu_fline(ST_FLINE_FORMAT);
                    break;
            }
            break;
        }
        case 1:   /* FScc / FDBcc / FTRAPcc(<ea> のモードで区別) */
        {
            int mode = (REG_IR >> 3) & 7;
            int reg = REG_IR & 7;
            if (mode == 1)
                fdbcc();
            else if (mode == 7 && reg >= 2 && reg <= 4)
                ftrap();
            else
                fscc(mode, reg);
            break;
        }
        case 2:   /* FBcc.W(FNOP = FBF.W #0 を含む) */
            fbcc(0);
            break;
        case 3:   /* FBcc.L */
            fbcc(1);
            break;
        default:  /* F2xx では到達しない */
            fpu_fline(ST_FLINE_FORMAT);
            break;
    }
}

/* F3xx(cpID=1、type 100-111)。100=FSAVE、101=FRESTORE、110/111=line-1111 */
void m68040_fpu_op1(void)
{
    s_fpu_abort = 0;
    if (!s_mx_fpu_present)
    {
        if (mx68k_musashi_stub_hook) mx68k_musashi_stub_hook(MX68K_STUB_FPU_FLINE, REG_PPC, REG_IR, 0);
        m68ki_exception_1111();
        return;
    }
    s_st[ST_OP1]++;

    switch ((REG_IR >> 6) & 7)
    {
        case 4: fpu_fsave(); break;
        case 5: fpu_frestore(); break;
        default: fpu_fline(ST_FLINE_FORMAT); break;
    }
}

/* ======================================================================
 * MX68K: Bridge/mx_cpu_musashi.c から呼ぶ口(宣言は mx_cpu_musashi.c 側の extern)
 * ====================================================================== */

static void fpu_status_init(void)
{
    memset(&s_fpst, 0, sizeof s_fpst);
    set_float_detect_tininess(float_tininess_before_rounding, &s_fpst);
    set_special_flags(addsub_swap_inf, &s_fpst);   /* 68881/68882 向け(Hatari fpp_softfloat.c と同じ設定) */
    apply_fpcr_rounding();
}

/* present: 1=FPU装着。model: 68881 または 68882(それ以外は68882扱い)。CPU非実行区間からだけ呼ぶ */
void mx68k_m68kfpu_set_config(int present, int model)
{
    s_mx_fpu_present = present ? 1 : 0;
    s_mx_fpu_model = (model == 68881) ? 68881 : 68882;
}

int mx68k_m68kfpu_get_present(void) { return s_mx_fpu_present; }
int mx68k_m68kfpu_get_model(void) { return s_mx_fpu_model; }

/* m68k_pulse_reset() の後に呼ぶ。FPU装着時だけ NULL フレームの FRESTORE と同じ状態にする */
void mx68k_m68kfpu_reset(void)
{
    if (!s_mx_fpu_present) return;
    fpu_status_init();
    do_frestore_null();
}

/* [P897-FPU] 定期行用の観測カウンタ(累計)。分母 op0/op1 と、line-1111 へ回った内訳を併記する */
int mx68k_m68kfpu_format_stats(char *out, size_t n)
{
    return snprintf(out, n,
                    "op0=%u op1=%u fgen=%u fmove_out=%u fmove_ctl=%u fmovem=%u fscc=%u fdbcc=%u ftrap=%u fbcc=%u "
                    "fsave=%u fsave_null=%u frestore=%u frestore_null=%u frestore_badfmt=%u "
                    "fline_pack=%u fline_unimpl=%u fline_format=%u priv=%u",
                    s_st[ST_OP0], s_st[ST_OP1], s_st[ST_FGEN], s_st[ST_FMOVE_OUT], s_st[ST_FMOVE_CTL],
                    s_st[ST_FMOVEM], s_st[ST_FSCC], s_st[ST_FDBCC], s_st[ST_FTRAP], s_st[ST_FBCC],
                    s_st[ST_FSAVE], s_st[ST_FSAVE_NULL], s_st[ST_FRESTORE], s_st[ST_FRESTORE_NULL],
                    s_st[ST_FRESTORE_BADFMT], s_st[ST_FLINE_PACK], s_st[ST_FLINE_UNIMPL], s_st[ST_FLINE_FORMAT],
                    s_st[ST_PRIV]);
}
