#include "status.h"

/* Bridge-side declarations: StatBar_SetFDD/ParamFDD are platform stubs.
   fdd.c calls these; definitions live here, not in Core/px68k/x68k/status.h. */
void StatBar_SetFDD(int32_t drive, const char* filename);
void StatBar_ParamFDD(int32_t drive, int32_t state, int32_t emask, int32_t blink);

/* P160: FDD access (read/seek) activity indicator. Core fdd.c calls
 * StatBar_ParamFDD with state==2 when a drive is the actively-selected drive
 * (FDD_SetAccess on the $E94007 drive-select write). Record the frame of the last
 * such event per drive; mx68k_fdd_accessing() reports "accessing" for a short decay
 * window so brief per-operation pulses stay visible in the status bar. */
extern int g_mx68k_frame_num;   /* frame counter (defined in EmulatorBridge.c) */
/* P684: FDD 2 台 → 4 台。Core fdd.c は元から drive 0-3 の範囲チェックを持ち
 * (fdd.c:91,113,133,149 の `drive>3`)、ドライブ 2/3 に対しても
 * StatBar_ParamFDD() を呼んでいる。本配列が 2 要素だった間、その呼び出しは
 * 下の `drive > 1` ガードで捨てられていた。 */
static int s_fdd_present[4]     = { 0, 0, 0, 0 };
static int s_fdd_last_access[4] = { -100000, -100000, -100000, -100000 };
static int s_fdd_selected       = 0;   /* P181: last actively-selected drive (fdd.Access) */

void StatBar_SetFDD(int32_t drive, const char* filename)
{
    (void)drive;
    (void)filename;
}

void StatBar_ParamFDD(int32_t drive, int32_t state, int32_t emask, int32_t blink)
{
    (void)emask;
    (void)blink;
    if (drive < 0 || drive > 3) return;   /* P684: 2 台 → 4 台 */
    s_fdd_present[drive] = (state != 0);
    if (state == 2) s_fdd_selected = drive;   /* P181: track selected drive; do NOT light on mere select */
}

/* P181: called from the DMA_Exec(0) site when DMAC ch0 (FDD-dedicated) is actively
 * transferring = real disk Read/Write. Lights the currently-selected drive's lamp. */
void mx68k_fdd_note_rw(void);
void mx68k_fdd_note_rw(void)
{
    s_fdd_last_access[s_fdd_selected] = g_mx68k_frame_num;
}

int mx68k_fdd_accessing(int drive);
int mx68k_fdd_accessing(int drive)
{
    if (drive < 0 || drive > 3) return 0;   /* P684: 2 台 → 4 台 */
    if (!s_fdd_present[drive]) return 0;
    /* Accessed within the last ~12 frames (~200 ms @ 60 fps) = show as active. */
    return (g_mx68k_frame_num - s_fdd_last_access[drive]) < 12;
}

/* P443 (D-7): media-present accessor. Core fdd.c calls StatBar_ParamFDD(drive, 0, ...)
 * from FDD_EjectFD() — including guest-initiated ejects — so s_fdd_present[] already
 * tracks the real media state. Exposed here so mx68k_get_status() can hand it to the
 * Swift layer, which reconciles its own fdd0Path/fdd1Path against it.
 * s_fdd_present[] (not FDD_IsReady()) is the source: FDD_IsReady() returns false during
 * the post-insert SetDelay grace window, which the 1 Hz status poll could otherwise
 * sample and clear a freshly-inserted path by mistake. */
int mx68k_fdd_media_present(int drive);
int mx68k_fdd_media_present(int drive)
{
    if (drive < 0 || drive > 3) return 0;   /* P684: 2 台 → 4 台 */
    return s_fdd_present[drive];
}

/* P201: HD BUSY access indicator. Core sasi.c calls StatBar_HDD((SASI_Phase)?2:0)
 * when the SASI bus is active (busy=2 / idle=0). status.c's empty stub is excluded
 * from the build (pbxproj Compile Sources) so this definition takes effect.
 * Record the frame of the last busy pulse; mx68k_hdd_accessing() reports "busy" for
 * a short decay window (same ~12-frame window as the FDD lamp) so brief pulses stay
 * visible in the status bar. */
static int s_hdd_last_access = -100000;

void StatBar_HDD(int32_t sw)
{
    if (sw != 0) s_hdd_last_access = g_mx68k_frame_num;   /* busy pulse */
}

int mx68k_hdd_accessing(void);
int mx68k_hdd_accessing(void)
{
    return (g_mx68k_frame_num - s_hdd_last_access) < 12;   /* ~200 ms decay */
}
