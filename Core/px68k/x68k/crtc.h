#ifndef _winx68k_crtc
#define _winx68k_crtc

#include "common.h"

#define	VSYNC_HIGH	180310L
#define	VSYNC_NORM	162707L

extern	uint8_t		CRTC_Regs[48];
extern	uint8_t		CRTC_Mode;
extern	uint16_t	CRTC_VSTART, CRTC_VEND;
extern	uint16_t	CRTC_HSTART, CRTC_HEND;
extern	int32_t		TextDotX, TextDotY;
extern	int32_t		TextScrollX, TextScrollY;
extern	uint8_t		VCReg0[2];
extern	uint8_t		VCReg1[2];
extern	uint8_t		VCReg2[2];
extern	uint16_t	CRTC_IntLine;
extern	uint8_t		CRTC_FastClr;
extern	uint8_t		CRTC_DispScan;
extern	uint32_t	CRTC_FastClrLine;
extern	uint16_t	CRTC_FastClrMask;
extern	uint8_t		CRTC_VStep;
extern  int32_t		HSYNC_CLK;

/* P641: 現在のCRTCレジスタから HSYNC_CLK（1ラスタあたりのCPUクロック数、
 * 名目10MHz単位）を再計算する。R00・R04・R20 または HRL ビットを変更した
 * 直後に呼ぶこと。 */
void CRTC_UpdateHSyncClock(void);

/* P641: 1フィールド分のCPUクロック予算（名目10MHz単位）を返す。分数状態を
 * 進めるため 1フレームにつき1回だけ呼ぶこと。active_vline_total には
 * その予算に対応する走査線数（レジスタ書換え途中は直前の完全な設定の値）を
 * 返す。NULL 可。 */
int32_t CRTC_GetFrameClocks(int32_t *active_vline_total);

extern	uint32_t	GrphScrollX[];
extern	uint32_t	GrphScrollY[];

void CRTC_Init(void);

/* P657: 水平フロントポーチ到達時に Bridge のフレームループから呼ぶ。
 * CRTC_Mode bit3 が立っていればラスタコピーを1回実行する。
 * CRTC_RasterCopy() は crtc.c 内 static となり外部公開しない。 */
void CRTC_HorizontalFrontPorch(void);

uint8_t  FASTCALL CRTC_Read(uint32_t adr);
uint16_t FASTCALL CRTC_Read16(uint32_t adr);
void FASTCALL CRTC_Write(uint32_t adr, uint8_t data);
void FASTCALL CRTC_Write16(uint32_t adr, uint16_t data, uint8_t ulds);

uint8_t FASTCALL VCtrl_Read(uint32_t adr);
void FASTCALL VCtrl_Write(uint32_t adr, uint8_t data);
void FASTCALL VCtrl_Write16(uint32_t adr, uint16_t data);

#endif
