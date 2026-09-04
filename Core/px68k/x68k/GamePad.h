#ifndef _GAMEPAD_H_
#define _GAMEPAD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void GamePad_Write(uint8_t data);
uint8_t GamePad_Read(void);

#ifdef __cplusplus
}
#endif

#endif
