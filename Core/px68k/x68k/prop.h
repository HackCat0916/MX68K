#ifndef _PROP_H_
#define _PROP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t XVIMode;
    int32_t CPU_Emu;
    int32_t MIDI_SW;
    int32_t MIDI_Type;
    int32_t MIDI_Reset;
    int32_t SRAMWarning;
    int32_t Sound_LPF;
    int32_t SoundROMEO;
    char    HDImage[16][4096];
    char    SCSIEXHDImage[16][4096];
    char    SoundFontFile[4096];
} Config_t;

extern Config_t Config;

#ifdef __cplusplus
}
#endif

#endif
