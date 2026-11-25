/* Bridge/mercury_cmdreg.h — 内部専用(m68000_bridge.c と selftest が取り込む)。
 *
 * P1142 (D-150): Mercury Unit コマンドレジスタ($ECC090/$ECC091)の読出し値合成。
 * Core mercury.c は $ECC091 を常に 0、$ECC090 を bit3 以外 0 で返すため、
 * Bridge 側で戻り値だけを差し替える(Core 状態は書き換えない)。
 * 根拠: 実機 CPLD XEiJ:misc/mu4sim/artemis.abl.txt:459-503、
 *       XEiJ:MercuryUnit.java:102、pix68k:src/x68k/mercury.cpp:124-126。
 * 既知の差(実機 ABEL との残差): bit4(!TERR)は参照2件に合わせ固定1、
 * bit2(!EXREQ)は Mcry_IsReady() による近似、$ECC091 は OUT モード時の読み戻しのみ。 */
#ifndef MERCURY_CMDREG_H
#define MERCURY_CMDREG_H

#include <stdint.h>

/* $ECC090(上位バイト)。mode=最後に $ECC090 へ書いた値、lr_phase=P634 の自走位相、
 * ready=Mcry_IsReady()(真=DMA 転送要求中)。 */
static inline uint8_t mcry_cmdreg_hi(uint8_t mode, unsigned lr_phase, int ready) {
    return (uint8_t)(0xF0u                          /* bit7-5 未駆動(1)、bit4 = !TERR(1) */
                   | ((lr_phase & 1u) << 3)         /* bit3 = P634 の自走 LR 位相 */
                   | (ready ? 0u : 0x04u)           /* bit2 = !EXREQ(0 = 転送要求中) */
                   | (mode & 0x03u));               /* bit1-0 = !M256/!ACKIG の読み戻し */
}

/* $ECC091(下位バイト)。最後に書いたコマンド値(Mcry_Status)をそのまま返す。 */
static inline uint8_t mcry_cmdreg_lo(uint8_t status) { return status; }

/* word 読出し: 上位=$ECC090、下位=$ECC091。 */
static inline uint16_t mcry_cmdreg_word(uint8_t hi, uint8_t lo) {
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

#endif /* MERCURY_CMDREG_H */
