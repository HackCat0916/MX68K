#ifndef D68K__HEADER
#define D68K__HEADER

/* ======================================================================== */
/* ========================= LICENSING & COPYRIGHT ======================== */
/* ======================================================================== */
/*
 *                                DEBABELIZER
 *                                Version 2.3
 *
 * A portable Motorola M680x0 disassembler.
 * Copyright 1999 Karl Stenerud.  All rights reserved.
 *
 * This code is freeware and may be freely used as long as this copyright
 * notice remains unaltered in the source code and any binary files
 * containing this code in compiled form.
 *
 * The latest version of this code can be obtained at:
 * (home page pending)
 */


/* ======================================================================== */
/* ============================= INSTRUCTIONS ============================= */
/* ======================================================================== */
/* 1. edit d68kconf.h and modify according to your needs.
 * 2. Implement in your host program the functions defined in
 *    "FUNCTIONS CALLED BY THE DISASSEMBLER" located later in this file.
 * 3. Your first call to m68k_disassemble will initialize the disassembler.
 */


/* ======================================================================== */
/* ================= FUNCTIONS CALLED BY THE DISASSEMBLER ================= */
/* ======================================================================== */

/* You will have to implement these functions */

/* read memory */
int32_t m68k_read_memory_32 (int32_t address);
int32_t m68k_read_memory_16 (int32_t address);
int32_t m68k_read_memory_8  (int32_t address);



/* ======================================================================== */
/* ====================== FUNCTIONS TO ACCESS THE CPU ===================== */
/* ======================================================================== */

/* Disassemble 1 instructionat pc.  Stores disassembly in str_buff and returns
 * the size of the instruction in bytes.
 */
int32_t m68k_disassemble(char* str_buff, int32_t pc);

/* Disassemble 1 instruction at pc.  Return disassembly as string pointer */
char* m68k_disassemble_quick(int32_t pc);


/* ======================================================================== */
/* ============================= CONFIGURATION ============================ */
/* ======================================================================== */

/* Import the configuration for this build */
#include "d68kconf.h"



/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */

#endif /* D68K__HEADER */
