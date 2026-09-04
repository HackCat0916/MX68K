#ifndef _GAMEPAD_H_
#define _GAMEPAD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void GamePad_Write(int32_t port, uint8_t data);
uint8_t GamePad_Read(int32_t port);

/* ボタン状態(strobe low 側 = 方向+TRG1/TRG2)。0 = 押下・未接続 = 0xff。 */
void GamePad_SetState(int32_t port, uint8_t btn0);

/* P500: ボタン状態(strobe high 側 = TRG3-TRG8 相当の多ボタンパッド用第2バンク)。
 * 0 = 押下。標準 2 ボタンパッドでは呼ばなくてよい(既定 0xff = idle)。 */
void GamePad_SetState1(int32_t port, uint8_t btn1);

#ifdef __cplusplus
}
#endif

#endif
