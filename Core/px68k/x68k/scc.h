#ifndef _winx68k_scc
#define _winx68k_scc

#include "common.h"

void SCC_IntCheck(void);
void SCC_Init(void);
uint8_t FASTCALL SCC_Read(uint32_t adr);
void FASTCALL SCC_Write(uint32_t adr, uint8_t data);

extern uint8_t MouseX;
extern uint8_t MouseY;
extern uint8_t MouseSt;

#endif
