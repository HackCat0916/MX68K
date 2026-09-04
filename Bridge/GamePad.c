#include <stdint.h>
#include <stdatomic.h>
#include "GamePad.h"

/* PPI PortC が駆動する strobe(セレクト線)。Core は 0x00/0xff の全バイトを渡す。
 * PPI_Init の PortC=0x0b は bits4/5 = low なので初期値 0 が整合。 */
static uint8_t pad_strobe[2] = { 0, 0 };

/* パッドのボタン状態。負論理(0=押下)・未接続 = 0xff。
 * strobe low 側 = 方向+TRG1/TRG2、high 側 = TRG3-TRG8。
 * UI スレッドから GamePad_SetState が書き、エミュスレッドの GamePad_Read が読む。 */
static _Atomic uint8_t pad_btn0[2] = { 0xff, 0xff };
static _Atomic uint8_t pad_btn1[2] = { 0xff, 0xff };

void GamePad_Write(int32_t port, uint8_t data)
{
    if (port >= 0 && port < 2) {
        pad_strobe[port] = data;
    }
}

uint8_t GamePad_Read(int32_t port)
{
    if (port < 0 || port >= 2) {
        return 0xff;
    }
    uint8_t s  = pad_strobe[port];
    uint8_t b0 = atomic_load_explicit(&pad_btn0[port], memory_order_relaxed);
    uint8_t b1 = atomic_load_explicit(&pad_btn1[port], memory_order_relaxed);
    return (uint8_t)(((~s) & b0) | (s & b1));
}

void GamePad_SetState(int32_t port, uint8_t btn0)
{
    if (port >= 0 && port < 2) {
        atomic_store_explicit(&pad_btn0[port], btn0, memory_order_relaxed);
    }
}

/* P500: strobe high 側(pad_btn1)の setter。読出し側(GamePad_Read)は既に
 * 2 バンク多重を実装済みのため、書込み経路の追加のみで多ボタンパッドが成立する。
 * 通常の 2 ボタン利用ではこの関数を呼ばない(pad_btn1 は 0xff 初期値のまま = idle)。 */
void GamePad_SetState1(int32_t port, uint8_t btn1)
{
    if (port >= 0 && port < 2) {
        atomic_store_explicit(&pad_btn1[port], btn1, memory_order_relaxed);
    }
}
