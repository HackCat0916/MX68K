#ifndef _winx68k_dim
#define _winx68k_dim

void DIM_Init(void);
void DIM_Cleanup(void);
int32_t DIM_SetFD(int32_t drive, char* filename);
int32_t DIM_Eject(int32_t drive);
int32_t DIM_Seek(int32_t drv, int32_t trk, FDCID* id);
int32_t DIM_ReadID(int32_t drv, FDCID* id);
int32_t DIM_WriteID(int32_t drv, int32_t trk, unsigned char* buf, int32_t num);
int32_t DIM_Read(int32_t drv, FDCID* id, unsigned char* buf);
int32_t DIM_ReadDiag(int32_t drv, FDCID* id, FDCID* retid, unsigned char* buf);
int32_t DIM_Write(int32_t drv, FDCID* id, unsigned char* buf, int32_t del);
int32_t DIM_GetCurrentID(int32_t drv, FDCID* id);

#endif
