/*	$Id: mmsystem.h,v 1.1.1.1 2003/04/28 18:06:55 nonaka Exp $	*/

#ifndef	MMSYSTEM_H__
#define	MMSYSTEM_H__

#include "windows.h"

#define	WINMMAPI
typedef	uint32_t	MMRESULT;
typedef	HANDLE		HMIDIOUT;
typedef	HMIDIOUT *	LPHMIDIOUT;

typedef	HANDLE		HMIDIIN;
typedef	HMIDIIN *	LPHMIDIIN;

typedef struct midihdr {
	char*			lpData;
	uint32_t			dwBufferLength;
	uint32_t			dwBytesRecorded;
	uint32_t			dwUser;
	uint32_t			dwFlags;
	struct midihdr *	lpNext;
	uint32_t			reserved;
	uint32_t			dwOffset;
	uint32_t			dwReserved[8];
} MIDIHDR, *PMIDIHDR, *NPMIDIHDR, *LPMIDIHDR;

#define MAXPNAMELEN 32
typedef struct midioutcaps_tag {
  uint16_t    wMid;
  uint16_t    wPid;
  int32_t vDriverVersion;
  char    szPname[MAXPNAMELEN];
  uint16_t    wTechnology;
  uint16_t    wVoices;
  uint16_t    wNotes;
  uint16_t    wChannelMask;
  uint32_t   dwSupport;
} MIDIOUTCAPS, *PMIDIOUTCAPS, *NPMIDIOUTCAPS, *LPMIDIOUTCAPS;

typedef struct midiincaps_tag {
  uint16_t    wMid;
  uint16_t    wPid;
  int32_t vDriverVersion;
  char    szPname[MAXPNAMELEN];
  uint32_t   dwSupport;
} MIDIINCAPS, *PMIDIINCAPS, *NPMIDIINCAPS, *LPMIDIINCAPS;

#define MIDBUF_SIZE 200
#define	MMSYSERR_NOERROR	0
#define	MIDIERR_STILLPLAYING	2

#define	MIDI_MAPPER		-1

#define	CALLBACK_NULL		0x00000000L
#define	CALLBACK_WINDOW		0x00010000L
#define	CALLBACK_TASK		0x00020000L
#define	CALLBACK_FUNCTION	0x00030000L

#ifdef __cplusplus
extern "C" {
#endif

WINMMAPI MMRESULT midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh);
WINMMAPI MMRESULT midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh);
WINMMAPI MMRESULT midiOutShortMsg(HMIDIOUT hmo, uint32_t dwMsg);
WINMMAPI MMRESULT midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh);
WINMMAPI MMRESULT midiOutOpen(LPHMIDIOUT phmo, uint32_t uDeviceID, void *dwCallback, void *dwInstance, uint32_t fdwOpen);
WINMMAPI MMRESULT midiOutClose(HMIDIOUT hmo);
WINMMAPI MMRESULT midiOutReset(HMIDIOUT hmo);

WINMMAPI MMRESULT midiInOpen(LPHMIDIIN phmo, uint32_t uDeviceID, void *dwCallback, void *dwInstance, uint32_t fdwOpen);
WINMMAPI MMRESULT midiInClose(HMIDIIN hmo);
WINMMAPI MMRESULT midiInReset(HMIDIIN hmo);
WINMMAPI MMRESULT midiInStart(HMIDIIN hmo);

#ifdef __cplusplus
};
#endif

#endif	/* MMSYSTEM_H__ */
