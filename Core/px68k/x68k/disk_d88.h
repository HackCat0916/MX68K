#ifndef _winx68k_d88
#define _winx68k_d88

typedef struct {			// Header Part (size:2B0h)
	uint8_t	fd_name[17];		// Disk Name
	uint8_t	reserved1[9]; 		// Reserved
	uint8_t	protect;		// Write Protect bit:4
	uint8_t	fd_type;		// Disk Format
	uint32_t	fd_size;		// Disk Size
	uint32_t	trackp[164];		// Track_pointer
} D88_HEADER;

void D88_Init(void);
void D88_Cleanup(void);
int32_t D88_SetFD(int32_t drive, char* filename);
int32_t D88_Eject(int32_t drive);
int32_t D88_Seek(int32_t drv, int32_t trk, FDCID* id);
int32_t D88_ReadID(int32_t drv, FDCID* id);
int32_t D88_WriteID(int32_t drv, int32_t trk, unsigned char* buf, int32_t num);
int32_t D88_Read(int32_t drv, FDCID* id, unsigned char* buf);
int32_t D88_ReadDiag(int32_t drv, FDCID* id, FDCID* retid, unsigned char* buf);
int32_t D88_Write(int32_t drv, FDCID* id, unsigned char* buf, int32_t del);
int32_t D88_GetCurrentID(int32_t drv, FDCID* id);

#endif
