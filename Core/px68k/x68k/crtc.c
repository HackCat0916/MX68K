// ---------------------------------------------------------------------------------------
//  CRTC.C - CRT Controller / Video Controller
// ---------------------------------------------------------------------------------------

#include	"common.h"
#include	"windraw.h"
#include	"winx68k.h"
#include	"tvram.h"
#include	"gvram.h"
#include	"bg.h"
#include	"m68000.h"
#include	"crtc.h"
#include	"crtc_timing.h"
#include	"sysport.h"


static uint16_t FastClearMask[16] = {
	0xffff, 0xfff0, 0xff0f, 0xff00, 0xf0ff, 0xf0f0, 0xf00f, 0xf000,
	0x0fff, 0x0ff0, 0x0f0f, 0x0f00, 0x00ff, 0x00f0, 0x000f, 0x0000
};
	uint8_t	CRTC_Regs[24*2];
	uint8_t	CRTC_Mode = 0;
	int32_t	TextDotX = 768, TextDotY = 512;
	uint16_t	CRTC_VSTART, CRTC_VEND;
	uint16_t	CRTC_HSTART, CRTC_HEND;
	int32_t	TextScrollX = 0, TextScrollY = 0;
	uint32_t	GrphScrollX[4] = {0, 0, 0, 0};		// 配列にしちゃった…
	uint32_t	GrphScrollY[4] = {0, 0, 0, 0};

	uint8_t	CRTC_FastClr = 0;
	uint8_t	CRTC_SispScan = 0;
	uint32_t	CRTC_FastClrLine = 0;
	uint16_t	CRTC_FastClrMask = 0;
	uint16_t	CRTC_IntLine = 0;
	uint8_t	CRTC_VStep = 2;

	uint8_t	VCReg0[2] = {0, 0};
	uint8_t	VCReg1[2] = {0, 0};
	uint8_t	VCReg2[2] = {0, 0};

	uint8_t	CRTC_RCFlag[2] = {0, 0};
	int32_t HSYNC_CLK = 324;

enum { MODES_ACTUAL, MODES_COMPAT, MODE_NORM = 0, MODE_HIGH, MODES };

	int32_t CHANGEAV_TIMING = 0; /* Separate change of geometry from change of refresh rate */
	int32_t VID_MODE = MODE_NORM; /* what framerate we start in */

	//extern int VID_MODE, CHANGEAV_TIMING;


// -----------------------------------------------------------------------
//   1ラスタあたりのCPUクロック数（名目10MHz単位）／1フィールドの予算
// -----------------------------------------------------------------------
// 旧実装は「固定フレーム長 / VLINE_TOTAL」だった。フレーム長が固定なので
// 垂直レジスタを増やすと1ラスタが短くなって打ち消し合い、垂直設定が実時間の
// ラスタ周期に届かなかった（525ライン60Hz設定では真値317に対し344＝約8%誤差。
// mfp.cのGPIP水平位置がこの値から求まるためゲストから見える誤差になる）。
//
// ラスタ周期は物理的に水平タイミング（R00とドットクロック＝R20・HRL）だけで
// 決まり、垂直レジスタには依存しない。フレーム全体のvalid判定で門を作ると、
// レジスタを順に書き換える途中で一時的に不正となり古い値が残ってしまうため、
// ここでは水平パラメータのみを見る（h_totalは常に1以上）。

#define CRTC_TIMING_CLOCK		10000000UL	/* px68k系の名目10MHzタイムベース */

/* 異常なゲスト設定でCPU/音声スケジューリング経路がオーバーフローしないための
 * フレームクロック・クランプ域（30Hz〜130Hz相当）。値は px68k-libretro 個人
 * フォーク（uraraworks/px68k-libretro, emscripten）crtc.c の
 * CRTC_MIN/MAX_FRAME_CLOCKS と同一。 */
#define CRTC_MIN_FRAME_CLOCKS	(10000000L / 130L)
#define CRTC_MAX_FRAME_CLOCKS	(10000000L / 30L)

/* MPX68K は winx68k.cpp（x11層）に static CrtcFieldClock FieldClock10M を置く。
 * MXには x11/ が無くフレームループが Bridge 側にあるため、状態はここ crtc.c に
 * 置き、CRTC_GetFrameClocks() 経由で Bridge から利用する。 */
static CrtcFieldClock CRTC_FieldClock10M;

void CRTC_UpdateHSyncClock(void)
{
	CrtcTiming t;
	unsigned long long num, den;

	CrtcTiming_FromRegs(CRTC_Regs, (SysPort[4] >> 1) & 1, &t);
	CrtcTiming_CyclesPerRaster(&t, CRTC_TIMING_CLOCK, &num, &den);
	if (den && (num / den) > 0)
		HSYNC_CLK = (int32_t)(num / den);
	else	// 退避経路：ドットクロックが異常なときだけ従来式に戻す
		HSYNC_CLK = ((CRTC_Regs[0x29]&0x10)?VSYNC_HIGH:VSYNC_NORM)/(VLINE_TOTAL?VLINE_TOTAL:1);
}

/* 1フィールド分のCPUクロック予算（名目10MHz単位）を返す。呼び出し側の
 * クロック倍率スケーリングは呼び出し側で行う。分数（numerator/denominator/
 * remainder）を保持するため、**1フレームにつき1回だけ**呼ぶこと——呼ぶたびに
 * 端数が進む。active_vline_total には「その予算に対応する走査線数」を返す
 * （レジスタ書換え途中で一時的に不正な組合せになった場合は直前の完全な設定の
 * 値を維持する。MPX68K の WinX68k_FieldCycles10M() と同じ設計）。 */
int32_t CRTC_GetFrameClocks(int32_t *active_vline_total)
{
	CrtcTiming t;
	int active = 0;
	int clocks;

	CrtcTiming_FromRegs(CRTC_Regs, (SysPort[4] >> 1) & 1, &t);
	if (CRTC_FieldClock10M.denominator == 0) {
		CrtcFieldClock_Init(&CRTC_FieldClock10M,
			(CRTC_Regs[0x29] & 0x10) ? VSYNC_HIGH : VSYNC_NORM,
			(VLINE_TOTAL > 0) ? (int)VLINE_TOTAL : 567);
	}
	clocks = CrtcFieldClock_Next(&CRTC_FieldClock10M, &t,
		CRTC_TIMING_CLOCK, (int)VLINE_TOTAL, &active);

	if (clocks < CRTC_MIN_FRAME_CLOCKS)
		clocks = CRTC_MIN_FRAME_CLOCKS;
	else if (clocks > CRTC_MAX_FRAME_CLOCKS)
		clocks = CRTC_MAX_FRAME_CLOCKS;

	if (active_vline_total)
		*active_vline_total = (active > 0) ? (int32_t)active : 567;
	return (int32_t)clocks;
}


// -----------------------------------------------------------------------
//   らすたーこぴー
// -----------------------------------------------------------------------
/* P657: 外部公開を廃止。唯一の実行契機は CRTC_HorizontalFrontPorch()。 */
static void CRTC_RasterCopy(void)
{

	uint32_t line = (((uint32_t)CRTC_Regs[0x2d])<<2);
	uint32_t src = (((uint32_t)CRTC_Regs[0x2c])<<9);
	uint32_t dst = (((uint32_t)CRTC_Regs[0x2d])<<9);

	/* P657: プレーン未選択または転送元==転送先は転送を行わない。
	 * 実機仕様: 「R21(E8002AH)のD03〜D00すべてに"0"を書き込むと、ラスタコピーは
	 * 実行されません」(テクニカルデータブック p.33 / PDF p.45)。 */
	if (!(CRTC_Regs[0x2b] & 0x0f) || src == dst)
		return;

	static const uint32_t off[4] = { 0, 0x20000, 0x40000, 0x60000 };
	int_fast16_t i, bit;

	for (bit = 0; bit < 4; bit++) {
		if (CRTC_Regs[0x2b] & (1 << bit)) {
			memmove(&TVRAM[dst + off[bit]], &TVRAM[src + off[bit]],
			    sizeof(uint32_t) * 128);
		}
	}

	line = (line - TextScrollY) & 0x3ff;
	for (i = 0; i < 4; i++) {
		TextDirtyLine[line] = 1;
		line = (line + 1) & 0x3ff;
	}


	TVRAM_RCUpdate();
}


/*
 * P657: 水平フロントポーチ
 *
 * 動作ポート bit3 はレベル制御のイネーブルである。セットされている間、
 * ラスタコピーは各水平フロントポーチで、その時点の R21/R22 を使って1回実行される。
 */
void CRTC_HorizontalFrontPorch(void)
{
	if (CRTC_Mode & 8)
		CRTC_RasterCopy();
}


// -----------------------------------------------------------------------
//   びでおこんとろーるれじすた
// -----------------------------------------------------------------------
// Reg0の色モードは、ぱっと見CRTCと同じだけど役割違うので注意。
// CRTCはGVRAMへのアクセス方法（メモリマップ上での見え方）が変わるのに対し、
// VCtrlは、GVRAM→画面の展開方法を制御する。
// つまり、アクセス方法（CRTC）は16bitモードで、表示は256色モードってな使い
// 方も許されるのれす。
// コットン起動時やYs（電波版）OPなどで使われてまふ。

uint8_t FASTCALL VCtrl_Read(uint32_t adr)
{
	uint8_t ret = 0xff;
	switch(adr & 0x701)
	{
	case 0x400:
		ret = 0x00;
		break;
	case 0x401:
		ret = VCReg0[1] & 0x07;
		break;
	case 0x500:
		VCReg1[0] &= 0x3f;
	case 0x501:
		ret = VCReg1[adr&1];
		break;
	case 0x600:
	case 0x601:
		ret = VCReg2[adr&1];
		break;
	default:
		break;
	}
	return ret;
}


void FASTCALL VCtrl_Write16(uint32_t adr, uint16_t data)
{
	switch(adr & 0x700)
	{
	case 0x400:
	case 0x401:
		data &= 0x003f;
		if ((VCReg0[0]<<8 | VCReg0[1]) != data)
		{
			VCReg0[0] = (data >> 8) & 0xff;
			VCReg0[1] = data & 0xff;
			TVRAM_SetAllDirty();
		}
		break;
	case 0x500:
	case 0x501:
		data &= 0x3fff;
		if ((VCReg1[0]<<8 | VCReg1[1]) != data)
		{
			VCReg1[0] = (data >> 8) & 0xff;
			VCReg1[1] = data & 0xff;
			TVRAM_SetAllDirty();
		}
		break;
	case 0x600:
	case 0x601:
		if ((VCReg2[0]<<8 | VCReg2[1]) != data)
		{
			VCReg2[0] = (data >> 8) & 0xff;
			VCReg2[1] = data & 0xff;
			TVRAM_SetAllDirty();
		}
		break;
	default:
		break;
	}
}

void FASTCALL VCtrl_Write(uint32_t adr, uint8_t data)
{

	switch(adr & 0x701)
	{
	case 0x400:
		VCReg0[0]=0x00;
		break;
	case 0x401:
		data &= 0x3f;
		if (VCReg0[1] != data)
		{
			VCReg0[1] = data;
			TVRAM_SetAllDirty();
		}
		break;
	case 0x500:
		data &= 0x3f;
	case 0x501:
		if (VCReg1[adr&1] != data)
		{
			VCReg1[adr&1] = data;
			TVRAM_SetAllDirty();
		}
		break;
	case 0x600:
	case 0x601:
		if (VCReg2[adr&1] != data)
		{
			VCReg2[adr&1] = data;
			TVRAM_SetAllDirty();
		}
		break;
	default:
		break;
	}
}

// -----------------------------------------------------------------------
//   CRTCれじすた
// -----------------------------------------------------------------------
// レジスタアクセスのコードが汚い ^^;

void CRTC_Init(void)
{
	memset(CRTC_Regs, 0, sizeof(CRTC_Regs));
	/* P657: リセット時に動作ポート由来の状態も明示的にクリアする(PR#2)。
	 * これが無いと、ハードリセット後も前セッションのラスタコピー/高速クリアの
	 * イネーブル状態が残る。 */
	CRTC_Mode = 0;
	CRTC_FastClr = 0;
	CRTC_FastClrLine = 0;
	CRTC_FastClrMask = 0;
	/* P671: 即時トリガ復活に伴い、旧実装フラグもリセット対象に含める
	 * (MPX68K f59c07d と同一)。これが無いと、リセット直前に src だけ書かれて
	 * dst 未更新だった中間状態が残り、リセット後の最初の $E8002D バイト書込みで
	 * 意図しない対が転送されうる。 */
	CRTC_RCFlag[0] = 0;
	CRTC_RCFlag[1] = 0;
	TextScrollX = 0, TextScrollY = 0;
	memset(GrphScrollX, 0, sizeof(GrphScrollX));
	memset(GrphScrollY, 0, sizeof(GrphScrollY));
	/* P641: ハードリセットで分数状態（remainder）を確実にゼロクリアし、
	 * 前セッションの端数を持ち越さない。MPX68K は WinX68k_Reset() で
	 * VLINE_TOTAL=567 を設定した直後に CrtcFieldClock_Init(&FieldClock10M,
	 * VSYNC_HIGH, VLINE_TOTAL) を呼ぶ。MXには x11/winx68k.cpp が無く
	 * リセット経路が CRTC_Init() を通るため、ここで同じ初期値を与える
	 * （VLINE_TOTAL はこの時点でまだ確定していないので 567 を直に書く）。 */
	CrtcFieldClock_Init(&CRTC_FieldClock10M, VSYNC_HIGH, 567);
}

uint8_t FASTCALL CRTC_Read(uint32_t adr)
{
	if(adr & 0x01){
		return ((CRTC_Read16(adr & 0xfffffe)) & 0xff);
	}
	else{
		return ((CRTC_Read16(adr & 0xfffffe)>>8) & 0xff);
	}
}

uint16_t FASTCALL CRTC_Read16(uint32_t adr)
{
  uint16_t ret = 0xffff;

	if (adr<0xe803ff) {
		int32_t reg = adr&0x3e;
		if ( (reg>=0x28)&&(reg<=0x2b) ) return ((CRTC_Regs[reg]<<8) | CRTC_Regs[reg+1]);
		else return 0;
	} else if ( (adr&0xfffffe)==0xe80480 ) {
// FastClearの注意点：
// FastClrビットに1を書き込むと、その時点ではReadBackしても1は見えない。
// 1書き込み後の最初の垂直帰線期間で1が立ち、消去を開始する。
// 1垂直同期期間で消去がおわり、0に戻る……らしひ（PITAPAT）
		if (CRTC_FastClr)
			ret = CRTC_Mode | 0x02;
		else
			ret = CRTC_Mode & 0xfd;
	}

	return (uint16_t)ret;
}

void FASTCALL CRTC_Write(uint32_t adr, uint8_t data)
{
	if(adr & 0x01){
	  CRTC_Write16(adr&0x00fffffe,(uint16_t)data,0x01);//奇数アドレス
	}
	else{
	  CRTC_Write16(adr&0x00fffffe,(uint16_t)(data<<8),0x02);//偶数アドレス
	}
}

void FASTCALL CRTC_Write16(uint32_t adr, uint16_t data, uint8_t ulds)
{
	uint8_t reg = (uint8_t)(adr&0x3e);
	int32_t old_vidmode = VID_MODE;

	// 0xe80000 ~ 0xe81fff
	if (adr<0xe80400)
	{
		if ( reg>=0x30 ) return;
		uint16_t wrtB4 = (((uint16_t)CRTC_Regs[reg & 0x3e]<<8)+CRTC_Regs[(reg & 0x3e) + 1]);//保存
		if(ulds & 0x02){
		  CRTC_Regs[reg & 0x3e] = (data >> 8) & 0xff;
		}
		if(ulds & 0x01){
		  CRTC_Regs[(reg & 0x3e) +1] = data & 0xff;
		}
		if (data == wrtB4) return; //no change
		TVRAM_SetAllDirty();
		switch(reg)
		{
		case 0x00:
			CRTC_Regs[0x00] &= 0x00;//not use
		case 0x01:
			HLINE_TOTAL = (((uint16_t)CRTC_Regs[0]<<8)+CRTC_Regs[1]) * 8;
			// P641: R00 (h_total) はラスタ周期そのもの。旧式の固定フレーム長／
			// VLINE_TOTAL はこれを完全に無視していた。
			CRTC_UpdateHSyncClock();
			break;
		case 0x02:
			CRTC_Regs[0x02] &= 0x00;//not use
		case 0x03:
			break;
		case 0x04:
			CRTC_Regs[0x04] &= 0x00;//not use
		case 0x05:
			CRTC_HSTART = (((uint16_t)CRTC_Regs[0x4]<<8)+CRTC_Regs[0x5]);
			if(CRTC_HEND>CRTC_HSTART){ TextDotX = (CRTC_HEND-CRTC_HSTART)*8; }//設定途中対策
			BG_HAdjust = (int32_t)(BG_Regs[0x0d]-(CRTC_HSTART+4))*8;		// 水平方向は解像度による1/2はいらない？（Tetris）
			WinDraw_ChangeSize();
			break;
		case 0x06:
			CRTC_Regs[0x06] &= 0x00;//not use
		case 0x07:
			CRTC_HEND = (((uint16_t)CRTC_Regs[0x6]<<8)+CRTC_Regs[0x7]);
			if(CRTC_HEND>CRTC_HSTART){ TextDotX = (CRTC_HEND-CRTC_HSTART)*8; }//設定途中対策
			WinDraw_ChangeSize();
			break;
		case 0x08:
			CRTC_Regs[0x08] &= 0x03;
		case 0x09:
			VLINE_TOTAL = (((uint16_t)CRTC_Regs[8]<<8)+CRTC_Regs[9]);
			CRTC_UpdateHSyncClock();
			break;
		case 0x0a:
			CRTC_Regs[0x0a] &= 0x03;
		case 0x0b:
			break;
		case 0x0c:
			CRTC_Regs[0x0c] &= 0x03;
		case 0x0d:
			CRTC_VSTART = (((uint16_t)CRTC_Regs[0xc]<<8)+CRTC_Regs[0xd]);
			BG_VLINE = (int32_t)(BG_Regs[0x0f]-CRTC_VSTART)/((BG_Regs[0x11]&4)?1:2);	// BGとその他がずれてる時の差分
			if(CRTC_VEND>CRTC_VSTART){ TextDotY = CRTC_VEND-CRTC_VSTART; }//設定途中対策
			if ((CRTC_Regs[0x29]&0x14)==0x10)
			{
				TextDotY/=2;
				CRTC_VStep = 1;
			}
			else if ((CRTC_Regs[0x29]&0x14)==0x04)
			{
				TextDotY*=2;
				CRTC_VStep = 4;
			}
			else
				CRTC_VStep = 2;
			WinDraw_ChangeSize();
			break;
		case 0x0e:
			CRTC_Regs[0x0e] &= 0x03;
		case 0x0f:
			CRTC_VEND = (((uint16_t)CRTC_Regs[0xe]<<8)+CRTC_Regs[0xf]);
			if(CRTC_VEND>CRTC_VSTART){ TextDotY = CRTC_VEND-CRTC_VSTART; }//設定途中対策
			if ((CRTC_Regs[0x29]&0x14)==0x10)
			{
				TextDotY/=2;
				CRTC_VStep = 1;
			}
			else if ((CRTC_Regs[0x29]&0x14)==0x04)
			{
				TextDotY*=2;
				CRTC_VStep = 4;
			}
			else
				CRTC_VStep = 2;
			WinDraw_ChangeSize();
			break;
		case 0x28:
			TVRAM_SetAllDirty();
			//break;
		case 0x29:
			CRTC_Regs[0x29] &= 0x1f;
			CRTC_UpdateHSyncClock();
			VID_MODE = !!(CRTC_Regs[0x29]&0x10);
			if(CRTC_VEND>CRTC_VSTART){ TextDotY = CRTC_VEND-CRTC_VSTART; }//設定途中対策
			if ((CRTC_Regs[0x29]&0x14)==0x10)
			{
				TextDotY/=2;
				CRTC_VStep = 1;
			}
			else if ((CRTC_Regs[0x29]&0x14)==0x04)
			{
				TextDotY*=2;
				CRTC_VStep = 4;
			}
			else
				CRTC_VStep = 2;
			if (VID_MODE != old_vidmode)
			{
				old_vidmode = VID_MODE;
				CHANGEAV_TIMING=1;
			}
			WinDraw_ChangeSize();
			break;
		case 0x10:
			CRTC_Regs[0x10] &= 0x00;
			break;
		case 0x12:
			CRTC_Regs[0x12] &= 0x03;
		case 0x13:
			CRTC_IntLine = (((uint16_t)CRTC_Regs[0x12]<<8)+CRTC_Regs[0x13]);
			break;
		case 0x14:
			CRTC_Regs[0x14] &= 0x03;
		case 0x15:
			TextScrollX = (((uint32_t)CRTC_Regs[0x14]<<8)+CRTC_Regs[0x15]);
			break;
		case 0x16:
			CRTC_Regs[0x16] &= 0x03;
		case 0x17:
			TextScrollY = (((uint32_t)CRTC_Regs[0x16]<<8)+CRTC_Regs[0x17]);
			break;
		case 0x18:
			CRTC_Regs[0x18] &= 0x03;
		case 0x19:
			GrphScrollX[0] = (((uint32_t)CRTC_Regs[0x18]<<8)+CRTC_Regs[0x19]);
			break;
		case 0x1a:
			CRTC_Regs[0x1a] &= 0x03;
		case 0x1b:
			GrphScrollY[0] = (((uint32_t)CRTC_Regs[0x1a]<<8)+CRTC_Regs[0x1b]);
			break;
		case 0x1c:
			CRTC_Regs[0x1c] &= 0x01;
		case 0x1d:
			GrphScrollX[1] = (((uint32_t)CRTC_Regs[0x1c]<<8)+CRTC_Regs[0x1d]);
			break;
		case 0x1e:
			CRTC_Regs[0x1e] &= 0x01;
		case 0x1f:
			GrphScrollY[1] = (((uint32_t)CRTC_Regs[0x1e]<<8)+CRTC_Regs[0x1f]);
			break;
		case 0x20:
			CRTC_Regs[0x20] &= 0x01;
		case 0x21:
			GrphScrollX[2] = (((uint32_t)CRTC_Regs[0x20]<<8)+CRTC_Regs[0x21]);
			break;
		case 0x22:
			CRTC_Regs[0x22] &= 0x01;
		case 0x23:
			GrphScrollY[2] = (((uint32_t)CRTC_Regs[0x22]<<8)+CRTC_Regs[0x23]);
			break;
		case 0x24:
			CRTC_Regs[0x24] &= 0x01;
		case 0x25:
			GrphScrollX[3] = (((uint32_t)CRTC_Regs[0x24]<<8)+CRTC_Regs[0x25]);
			break;
		case 0x26:
			CRTC_Regs[0x26] &= 0x01;
		case 0x27:
			GrphScrollY[3] = (((uint32_t)CRTC_Regs[0x26]<<8)+CRTC_Regs[0x27]);
			break;
		case 0x2a:
			CRTC_Regs[0x2a] &= 0x03;
		case 0x2b:
			break;
		case 0x2c:				// CRTC動作ポートのラスタコピーをONにしておいて（しておいたまま）、
		case 0x2d:				// Src/Dstだけ次々変えていくのも許されるらしい（ドラキュラとか）
			/* P671 (D-69): 互換トリガの復活(MPX68K f59c07d と同一設計、
			 * "Keep the legacy R22 completion trigger")。
			 * 転送は水平フロントポーチでも反復されるが(CRTC_HorizontalFrontPorch)、
			 * IOCS のように RC を ON にしたまま src/dst を次々更新するソフトでは、
			 * 1水平期間内に発行された対の一部が次の値で上書きされて失われる。
			 * 書込み時点でも1回転送することで、発行された対の取りこぼしを防ぐ。
			 * MX固有の ulds 判定は温存。 */
			if(ulds & 0x02){
			  CRTC_RCFlag[reg-0x2c] = 1;
			}
			if(ulds & 0x01){
			  CRTC_RCFlag[reg-0x2c+1] = 1;
			}
			if ((CRTC_Mode & 8) && (CRTC_RCFlag[1]))
			{
				CRTC_RasterCopy();
				CRTC_RCFlag[0] = 0;
				CRTC_RCFlag[1] = 0;
			}
			break;
		default:
			break;
		}
	}
	else if (adr==0xe80480)
	{
	  if(ulds & 0x01){// access 0xe80481
					// CRTC動作ポート
		/* P657/P671: 動作ポートの bit3 はレベル制御のイネーブルであり、
		 * ON の間は水平フロントポーチごとに転送が反復される。
		 * それに加えて、P671 では bit3 がセットされた時点でも1回転送する
		 * (MPX68K f59c07d の "compatibility trigger" と同一)。
		 * MX固有の &0x0f マスクは温存(bit3 はマスク内なので影響しない)。 */
		CRTC_Mode = ((data & 0x0f)|(CRTC_Mode&2));
		if (CRTC_Mode & 8)
		{
			CRTC_RasterCopy();
			CRTC_RCFlag[0] = 0;
			CRTC_RCFlag[1] = 0;
		}
		if (CRTC_Mode&2)		// 高速クリア
		{
			CRTC_FastClrLine = x68k_vline;
						// この時点のマスクが有効らしい（クォース）
			CRTC_FastClrMask = FastClearMask[CRTC_Regs[0x2b]&15];
		}
	  }
	}

}
