/*
 No Support for MIDI
*/
#include "prop.h"
#include "winui.h"
#include "midi.h"

uint32_t
midiOutShortMsg(HMIDIOUT hmo, uint32_t dwMsg)
{
	(void)hmo;
	(void)dwMsg;
	return MMSYSERR_NOERROR;
}

uint32_t
midiOutLongMsg(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)pmh;
	(void)cbmh;
	return MMSYSERR_NOERROR;
}

uint32_t
midiOutUnprepareHeader(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)pmh;
	(void)cbmh;
	return MMSYSERR_NOERROR;
}

uint32_t
midiOutPrepareHeader(HMIDIOUT hmo, LPMIDIHDR pmh, uint32_t cbmh)
{
	(void)hmo;
	(void)pmh;
	(void)cbmh;
	return MMSYSERR_NOERROR;
}

uint32_t
mid_outDevList(LPHMIDIOUT phmo)
{
 return 0;
}

uint32_t
mid_inDevList(LPHMIDIOUT phmo)
{
 return 0;
}

uint32_t
midiOutClose(HMIDIOUT hmo)
{
	(void)hmo;
	return MMSYSERR_NOERROR;
}

uint32_t
midiOutReset(HMIDIOUT hmo)
{
	(void)hmo;
	return MMSYSERR_NOERROR;
}

void
midOutChg(uint32_t port_no, uint32_t bank)
{
 return;
}

void
midInChg(uint32_t port_no)
{
 return;
}

