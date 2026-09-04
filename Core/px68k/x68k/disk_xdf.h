#ifndef _winx68k_xdf
#define _winx68k_xdf

void XDF_Init(void);
void XDF_Cleanup(void);
int32_t XDF_SetFD(int32_t drive, char* filename);
int32_t XDF_Eject(int32_t drive);
int32_t XDF_Seek(int32_t drv, int32_t trk, FDCID* id);
int32_t XDF_ReadID(int32_t drv, FDCID* id);
int32_t XDF_WriteID(int32_t drv, int32_t trk, uint8_t* buf, int32_t num);
int32_t XDF_Read(int32_t drv, FDCID* id, uint8_t* buf);
int32_t XDF_ReadDiag(int32_t drv, FDCID* id, FDCID* retid, uint8_t* buf);
int32_t XDF_Write(int32_t drv, FDCID* id, uint8_t* buf, int32_t del);
int32_t XDF_GetCurrentID(int32_t drv, FDCID* id);

#endif
