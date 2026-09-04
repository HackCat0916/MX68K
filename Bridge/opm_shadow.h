/* Bridge/opm_shadow.h — internal only, shared between m68000_bridge.c
 * (writer, via the CPU-write hook) and EmulatorBridge.c (reader/writer,
 * via state save/load). Not part of the Swift-facing contract.
 *
 * P479 (D-41 symptom 1): the OPM (YM2151) chip has no Core-side API to read
 * back its register file (opm.h:88 GetReg is declared but never defined, and
 * class OPM keeps no raw register array), so a state snapshot cannot capture
 * the tone parameters from the chip itself. Instead the Bridge shadows every
 * guest write that reaches the OPM bus ports, saves that shadow inside the
 * state file, and replays it into the chip on load.
 *
 * Known limitation: DMA-driven writes do not pass through the CPU write hook,
 * so they are not shadowed. No X68000 music driver targets the OPM as a DMA
 * destination (the DMAC device assignment has no OPM DREQ equivalent,
 * dmac.c:398-403), so this is a theoretical gap only. */
#ifndef OPM_SHADOW_H
#define OPM_SHADOW_H

#include <stdint.h>

/* Last value written to each OPM register number (index = register number),
 * and a per-register "has ever been written" flag so a load only replays
 * registers the guest actually programmed. */
extern uint8_t g_opm_shadow[256];
extern uint8_t g_opm_written[256];

/* Register 0x19 encodes TWO independent variables in one register number,
 * selected by bit7 (opm.cpp:219-221): bit7=1 -> PMD, bit7=0 -> AMD. A plain
 * 256-entry shadow would keep only whichever was written last, so both halves
 * get their own slot + written flag. */
extern uint8_t g_opm_reg19_pmd, g_opm_reg19_amd;
extern uint8_t g_opm_reg19_pmd_written, g_opm_reg19_amd_written;

/* Register 0x08 (KEYON) encodes the channel number in the data byte itself
 * (bits0-2) and the per-operator gate mask in bits3-6, so a plain 256-entry
 * shadow only keeps the LAST channel's mask (register number is always 0x08,
 * never per-channel). A dedicated per-channel array is required.
 * Bit layout verified against Core/px68k/fmgen/fmgen.cpp:793-799
 * (Channel4::KeyControl): bit0=op0, bit1=op1, bit2=op2, bit3=op3. */
extern uint8_t g_opm_keyon[8];

/* The OPM bus is a two-stage latch (address write, then data write); this is
 * the Bridge-side mirror of the Core-internal CurReg. */
extern uint8_t g_opm_curreg;

/* Clear the whole shadow. Called wherever the real chip is (re)initialised,
 * so shadow and chip stay in sync. Defined in m68000_bridge.c. */
void p479_opm_shadow_reset(void);

#endif /* OPM_SHADOW_H */
