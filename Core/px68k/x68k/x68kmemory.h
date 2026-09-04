#ifndef _WINX68K_MEMORY_H
#define _WINX68K_MEMORY_H

#include "common.h"

#define	Memory_ReadB	cpu_readmem24
#define Memory_ReadW	cpu_readmem24_word
#define Memory_ReadD	cpu_readmem24_dword

#define	Memory_WriteB	cpu_writemem24
#define Memory_WriteW	cpu_writemem24_word
#define Memory_WriteD	cpu_writemem24_dword

extern	uint8_t*	IPL;
extern	uint8_t*	MEM;
extern	uint8_t*	OP_ROM;
extern	uint8_t*	FONT;
extern	uint8_t 	SCSIIPL[0x2000];
extern	uint8_t 	SRAM[0x4000];
extern	uint8_t 	GVRAM[0x80000];
extern	uint8_t 	TVRAM[0x80000];

extern	int32_t	BusErrFlag;
extern	int32_t	BusErrAdr;
extern	int32_t	MemByteAccess;

void Memory_ErrTrace(void);
void Memory_IntErr(int32_t i);

void Memory_Init(void);
uint32_t Memory_ReadB(uint32_t adr);
uint32_t Memory_ReadW(uint32_t adr);
uint32_t Memory_ReadD(uint32_t adr);

uint8_t  dma_readmem24(uint32_t adr);
uint16_t dma_readmem24_word(uint32_t adr);
uint32_t dma_readmem24_dword(uint32_t adr);

void Memory_WriteB(uint32_t adr, uint32_t data);
void Memory_WriteW(uint32_t adr, uint32_t data);
void Memory_WriteD(uint32_t adr, uint32_t data);

void dma_writemem24(uint32_t adr, uint8_t data);
void dma_writemem24_word(uint32_t adr, uint16_t data);
void dma_writemem24_dword(uint32_t adr, uint32_t data);

void cpu_setOPbase24(uint32_t adr);

void Memory_SetSCSIMode();

#endif
