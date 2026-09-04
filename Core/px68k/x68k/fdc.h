#ifndef _winx68k_fdc
#define _winx68k_fdc

#include "common.h"

void FDC_Init(void);
uint8_t FASTCALL FDC_Read(uint32_t adr);
void FASTCALL FDC_Write(uint32_t adr, uint8_t data);
int16_t FDC_Flush(void);
void FDC_EPhaseEnd(void);
void FDC_SetForceReady(int32_t n);
/* P686 (D-70): 模擬する FDD 接続台数(2 = 外付けユニット非装着 / 4 = 装着)。 */
void FDC_SetDriveCount(int32_t n);
int32_t FDC_IsDataReady(void);

#endif //_winx68k_fdc

