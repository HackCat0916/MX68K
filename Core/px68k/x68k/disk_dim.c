#include "common.h"
#include "dosio.h"
#include "fdc.h"
#include "fdd.h"
#include "disk_dim.h"

// DIM Image Header
typedef struct {
	uint8_t	type;
	uint8_t	trkflag[170];
	uint8_t	headerinfo[15];
	uint8_t	date[4];
	uint8_t	time[4];
	uint8_t	comment[61];
	uint8_t	overtrack;
} DIM_HEADER;

// DIM Disk Type
enum {
	DIM_2HD = 0,
	DIM_2HS,
	DIM_2HC,
	DIM_2HDE,
	DIM_2HQ = 9,
};

static const int32_t SctLength[10] = {
	1024*8, 1024*9, 512*15, 1024*9, 0, 0, 0, 0, 0, 512*18
};

static char           DIMFile[4][MAX_PATH];
static int32_t            DIMCur[4] = {0, 0, 0, 0};
static int32_t            DIMTrk[4] = {0, 0, 0, 0};
static unsigned char* DIMImg[4] = {0, 0, 0, 0};

void DIM_Init(void)
{
	int32_t drv;

	for (drv=0; drv<4; drv++) {
		DIMCur[drv] = 0;
		DIMImg[drv] = 0;
		memset(DIMFile[drv], 0, MAX_PATH);
	}
}


void DIM_Cleanup(void)
{
	int32_t drv;
	for (drv=0; drv<4; drv++) DIM_Eject(drv);
}


int32_t DIM_SetFD(int32_t drv, char* filename)
{
	FILEH fp;
	DIM_HEADER* dh;
	uint32_t i, len;
	unsigned char* p;

	strncpy(DIMFile[drv], filename, MAX_PATH);
	DIMFile[drv][MAX_PATH-1] = 0;

	DIMImg[drv] = (unsigned char*)malloc(1024*9*170+sizeof(DIM_HEADER));		// Maximum size
	if ( !DIMImg[drv] ) return FALSE;
	memset(DIMImg[drv], 0xe5, 1024*9*170+sizeof(DIM_HEADER));
	fp = File_Open(DIMFile[drv]);
	if ( !fp ) {
		memset(DIMFile[drv], 0, MAX_PATH);
		FDD_SetReadOnly(drv);
		return FALSE;
	}

	File_Seek(fp, 0, FSEEK_SET);
	if ( File_Read(fp, DIMImg[drv], sizeof(DIM_HEADER))!=sizeof(DIM_HEADER) ) goto dim_set_error;
	dh = (DIM_HEADER*)DIMImg[drv];
	if ( dh->type>9 ) goto dim_set_error;
	len = SctLength[dh->type];
	if ( !len ) goto dim_set_error;
	p = DIMImg[drv]+sizeof(DIM_HEADER);
	for (i=0; i<170; i++) {
		if ( dh->trkflag[i] ) {
			if ( File_Read(fp, p, len)!=len ) goto dim_set_error;
		}
		p += len;
	}
	File_Close(fp);
	if ( !dh->overtrack ) memset(dh->trkflag, 1, 170);
	return TRUE;

dim_set_error:
	File_Close(fp);
	FDD_SetReadOnly(drv);
	return FALSE;
}


int32_t DIM_Eject(int32_t drv)
{
	FILEH fp;
	DIM_HEADER* dh;
	uint32_t i, len;
	unsigned char* p;

	if ( !DIMImg[drv] ) {
		memset(DIMFile[drv], 0, MAX_PATH);
		return FALSE;
	}
	dh = (DIM_HEADER*)DIMImg[drv];
	len = SctLength[dh->type];
	p = DIMImg[drv]+sizeof(DIM_HEADER);
	if ( !FDD_IsReadOnly(drv) ) {
		fp = File_Open(DIMFile[drv]);
		if ( !fp ) goto dim_eject_error;
		File_Seek(fp, 0, FSEEK_SET);
		if ( File_Write(fp, DIMImg[drv], sizeof(DIM_HEADER))!=sizeof(DIM_HEADER) ) goto dim_eject_error;
		for (i=0; i<170; i++) {
			if ( dh->trkflag[i] ) {
				if ( File_Write(fp, p, len)!=len ) goto dim_eject_error;
			}
			p += len;
		}
		File_Close(fp);
	}
	free(DIMImg[drv]);
	DIMImg[drv] = 0;
	memset(DIMFile[drv], 0, MAX_PATH);
	return TRUE;

dim_eject_error:
	free(DIMImg[drv]);
	DIMImg[drv] = 0;
	memset(DIMFile[drv], 0, MAX_PATH);
	return FALSE;
}


static void SetID(int32_t drv, FDCID* id, int32_t c, int32_t h, int32_t r)
{
	int32_t type = DIMImg[drv][0];
	switch (type) {
		case DIM_2HD:				// 1024byte/sct, 8sct/trk
			id->n = 3; break;
		case DIM_2HS:				// 1024byte/sct, 9sct/trk
			if ( (c)||(h)||(r!=1) ) r += 9;
			id->n = 3; break;
		case DIM_2HDE:
			if ( (c)||(h)||(r!=1) ) h += 0x80;
			id->n = 3; break;
		case DIM_2HC:				// 512byte/sct, 15sct/trk
		case DIM_2HQ:				// 512byte/sct, 18sct/trk
			id->n = 2; break;
	}
	id->c = c;
	id->h = h;
	id->r = r;
}


static int32_t IncTrk(int32_t drv, int32_t r)
{
	int32_t type = DIMImg[drv][0];
	switch (type) {
		case DIM_2HD:				// 1024byte/sct, 8sct/trk
			r = (r+1)&7;
			break;
		case DIM_2HS:				// 1024byte/sct, 9sct/trk
			if ( r>8 ) r -= 9;		// 9SCDRV用
		case DIM_2HDE:
			r = (r+1)%9;
			break;
		case DIM_2HC:				// 512byte/sct, 15sct/trk
			r = (r+1)%15;
			break;
		case DIM_2HQ:				// 512byte/sct, 18sct/trk
			r = (r+1)%18;
			break;
	}
	return r;
}


static int32_t GetPos(int32_t drv, FDCID* id)
{
	int32_t ret, c = id->c, h = id->h, r = id->r, n = id->n;
	int32_t type = DIMImg[drv][0];
	switch (type) {
		case DIM_2HD:				// 1024byte/sct, 8sct/trk
			if ( (c<0)||(c>84)||(h<0)||(h>1)||(r<1)||(r>8)||(n!=3) ) return 0;
			ret = SctLength[type]*(c*2+h)+((r-1)<<10);
			ret += sizeof(DIM_HEADER);
			break;
		case DIM_2HS:				// 1024byte/sct, 9sct/trk
			if ( r>9 ) r -= 9;		// 9SCDRV用
			if ( (c<0)||(c>84)||(h<0)||(h>1)||(r<1)||(r>9)||(n!=3) ) return 0;
			ret = SctLength[type]*(c*2+h)+((r-1)<<10);
			ret += sizeof(DIM_HEADER);
			break;
		case DIM_2HDE:
			h &= 1;					// 9SCDRV用
			if ( (c<0)||(c>84)||(h<0)||(h>1)||(r<1)||(r>9)||(n!=3) ) return 0;
			ret = SctLength[type]*(c*2+h)+((r-1)<<10);
			ret += sizeof(DIM_HEADER);
			break;
		case DIM_2HC:				// 512byte/sct, 15sct/trk
			if ( (c<0)||(c>84)||(h<0)||(h>1)||(r<1)||(r>15)||(n!=2) ) return 0;
			ret = SctLength[type]*(c*2+h)+((r-1)<<9);
			ret += sizeof(DIM_HEADER);
			break;
		case DIM_2HQ:				// 512byte/sct, 18sct/trk
			if ( (c<0)||(c>84)||(h<0)||(h>1)||(r<1)||(r>18)||(n!=2) ) return 0;
			ret = SctLength[type]*(c*2+h)+((r-1)<<9);
			ret += sizeof(DIM_HEADER);
			break;
		default:
			ret = 0;
			break;
	}
	return ret;
}


static int32_t CheckTrack(int32_t drv, int32_t trk)
{
	DIM_HEADER* dh = (DIM_HEADER*)DIMImg[drv];
	switch (dh->type) {
		case DIM_2HD:				// 1024byte/sct, 8sct/trk
			if ( ((trk>153)&&(!dh->overtrack))||(!dh->trkflag[trk]) ) return 0;
			break;
		case DIM_2HS:				// 1024byte/sct, 9sct/trk
			if ( ((trk>159)&&(!dh->overtrack))||(!dh->trkflag[trk]) ) return 0;
			break;
		case DIM_2HDE:
			if ( ((trk>159)&&(!dh->overtrack))||(!dh->trkflag[trk]) ) return 0;
			break;
		case DIM_2HC:				// 512byte/sct, 15sct/trk
			if ( ((trk>159)&&(!dh->overtrack))||(!dh->trkflag[trk]) ) return 0;
			break;
		case DIM_2HQ:				// 512byte/sct, 18sct/trk
			if ( ((trk>159)&&(!dh->overtrack))||(!dh->trkflag[trk]) ) return 0;
			break;
		default:
			return FALSE;
	}
	return TRUE;
}


int32_t DIM_Seek(int32_t drv, int32_t trk, FDCID* id)
{
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( (trk<0)||(trk>169) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( DIMTrk[drv]!=trk ) DIMCur[drv] = 0;
	SetID(drv, id, trk>>1, trk&1, DIMCur[drv]+1);
	DIMTrk[drv] = trk;
	return TRUE;
}


int32_t DIM_GetCurrentID(int32_t drv, FDCID* id)
{
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( (DIMTrk[drv]<0)||(DIMTrk[drv]>169) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( !CheckTrack(drv, DIMTrk[drv]) ) return FALSE;
	SetID(drv, id, DIMTrk[drv]>>1,DIMTrk[drv]&1, DIMCur[drv]+1);
	return TRUE;
}


int32_t DIM_ReadID(int32_t drv, FDCID* id)
{
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( (DIMTrk[drv]<0)||(DIMTrk[drv]>169) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( !CheckTrack(drv, DIMTrk[drv]) ) return FALSE;
	SetID(drv, id, DIMTrk[drv]>>1,DIMTrk[drv]&1, DIMCur[drv]+1);
	DIMCur[drv] = IncTrk(drv, DIMCur[drv]);
	return TRUE;
}


int32_t DIM_WriteID(int32_t drv, int32_t trk, unsigned char* buf, int32_t num)
{
#if 0
	int_fast16_t i;
	unsigned char c = buf[num<<2];
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( (trk<0)||(trk>169) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( num!=8 ) return FALSE;
	for (i=0; i<8; i++, buf+=4) {
		if ( (((buf[0]<<1)+buf[1])!=trk)||(buf[2]<1)||(buf[2]>8)||(buf[3]!=3) ) return FALSE;
	}
	DIMTrk[drv] = trk;
	return TRUE;
#else
	(void)drv;
	(void)trk;
	(void)buf;
	(void)num;
	return FALSE;
#endif
}


int32_t DIM_Read(int32_t drv, FDCID* id, unsigned char* buf)
{
	int32_t pos;
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( (((id->c<<1)+(id->h&1))!=DIMTrk[drv]) ) return FALSE;
	if ( !CheckTrack(drv, (id->c<<1)+(id->h&1)) ) return FALSE;
	pos = GetPos(drv, id);
	if ( !pos ) return FALSE;
	memcpy(buf, DIMImg[drv]+pos, (id->n==2)?512:1024);
	DIMCur[drv] = IncTrk(drv, id->r-1);
	return TRUE;
}


int32_t DIM_ReadDiag(int32_t drv, FDCID* id, FDCID* retid, unsigned char* buf)
{
	int32_t pos;
	(void)id;
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( !CheckTrack(drv, DIMTrk[drv]) ) return FALSE;
	SetID(drv, retid, DIMTrk[drv]>>1, DIMTrk[drv]&1, DIMCur[drv]+1);
	pos = GetPos(drv, retid);
	if ( !pos ) return FALSE;
	memcpy(buf, DIMImg[drv]+pos, (retid->n==2)?512:1024);
	DIMCur[drv] = IncTrk(drv, DIMCur[drv]);
	return TRUE;
}


int32_t DIM_Write(int32_t drv, FDCID* id, unsigned char* buf, int32_t del)
{
	int32_t pos;
	(void)del;
	if ( (drv<0)||(drv>3) ) return FALSE;
	if ( !DIMImg[drv] ) return FALSE;
	if ( (((id->c<<1)+(id->h&1))!=DIMTrk[drv]) ) return FALSE;
	if ( !CheckTrack(drv, (id->c<<1)+(id->h&1)) ) return FALSE;
	pos = GetPos(drv, id);
	if ( !pos ) return FALSE;
	memcpy(DIMImg[drv]+pos, buf, (id->n==2)?512:1024);
	DIMCur[drv] = IncTrk(drv, id->r-1);
	return TRUE;
}
