/*  Copyright 2003-2004 Stephane Dallongeville

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file c68kexec.c
    \brief C68K emulation execution and instruction functions.
*/

#include "core.h"
#include "c68k.h"
#include "c68k_refresh.h"

// #define TRACE_WITH_Q68  // Define to use Q68 tracing code to trace insns
                           // (requires Q68 built in, of course)


// exception cycle table (taken from musashi core)
static const int32_t c68k_exception_cycle_table[256] =
{
	  4, //  0: Reset - Initial Stack Pointer
	  4, //  1: Reset - Initial Program Counter
	 50, //  2: Bus Error
	 50, //  3: Address Error
	 34, //  4: Illegal Instruction
	 38, //  5: Divide by Zero
	 40, //  6: CHK
	 34, //  7: TRAPV
	 34, //  8: Privilege Violation
	 34, //  9: Trace
	  4, // 10:
	  4, // 11:
	  4, // 12: RESERVED
	  4, // 13: Coprocessor Protocol Violation
	  4, // 14: Format Error
	 44, // 15: Uninitialized Interrupt
	  4, // 16: RESERVED
	  4, // 17: RESERVED
	  4, // 18: RESERVED
	  4, // 19: RESERVED
	  4, // 20: RESERVED
	  4, // 21: RESERVED
	  4, // 22: RESERVED
	  4, // 23: RESERVED
	 44, // 24: Spurious Interrupt
	 44, // 25: Level 1 Interrupt Autovector
	 44, // 26: Level 2 Interrupt Autovector
	 44, // 27: Level 3 Interrupt Autovector
	 44, // 28: Level 4 Interrupt Autovector
	 44, // 29: Level 5 Interrupt Autovector
	 44, // 30: Level 6 Interrupt Autovector
	 44, // 31: Level 7 Interrupt Autovector
	 34, // 32: TRAP #0
	 34, // 33: TRAP #1
	 34, // 34: TRAP #2
	 34, // 35: TRAP #3
	 34, // 36: TRAP #4
	 34, // 37: TRAP #5
	 34, // 38: TRAP #6
	 34, // 39: TRAP #7
	 34, // 40: TRAP #8
	 34, // 41: TRAP #9
	 34, // 42: TRAP #10
	 34, // 43: TRAP #11
	 34, // 44: TRAP #12
	 34, // 45: TRAP #13
	 34, // 46: TRAP #14
	 34, // 47: TRAP #15
	  4, // 48: FP Branch or Set on Unknown Condition
	  4, // 49: FP Inexact Result
	  4, // 50: FP Divide by Zero
	  4, // 51: FP Underflow
	  4, // 52: FP Operand Error
	  4, // 53: FP Overflow
	  4, // 54: FP Signaling NAN
	  4, // 55: FP Unimplemented Data Type
	  4, // 56: MMU Configuration Error
	  4, // 57: MMU Illegal Operation Error
	  4, // 58: MMU Access Level Violation Error
	  4, // 59: RESERVED
	  4, // 60: RESERVED
	  4, // 61: RESERVED
	  4, // 62: RESERVED
	  4, // 63: RESERVED
	     // 64-255: User Defined
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
};

// global variable
///////////////////

#ifndef C68K_GEN

#ifndef C68K_NO_JUMP_TABLE
#ifndef C68K_CONST_JUMP_TABLE
static void *JumpTable[0x10000];
#endif
#endif

static uint32_t C68k_Initialised = 0;

#endif  // C68K_GEN

// include macro file
//////////////////////

#include "c68kmac.inc"

#ifndef C68K_GEN
# ifdef TRACE_WITH_Q68
#include "../q68/q68.h"
static c68k_struc *TRACE_CPU;
static uint32_t readw(uint32_t address) {
    return TRACE_CPU->Read_Word(address);
}
/* Make our own version of the structure to avoid the overhead of dozens of
 * function calls every instruction */
static struct {
    uint32_t D[8], A[8], PC, SR, USP, SSP, dummy[7];
    void *readb, *readw, *writeb, *writew;
} state = {.readw = readw};
void TRACE(int PC,c68k_struc *CPU,int Opcode,int CCnt) {
    static FILE *f;
    if (!f) f = fopen("c68k.log", "w");
    TRACE_CPU = CPU;
    memcpy(state.D, CPU->D, 16*4);
    state.PC = PC - 2 - CPU->BasePC;
    state.SR = GET_SR;
    if (f) q68_trace((Q68State *)&state, f,
                     CPU->CycleToDo - CCnt, CPU->CycleToDo);
}
# endif  // TRACE_WITH_Q68
#endif  // !C68K_GEN

// P976: 主記憶DRAMリフレッシュの周期スチール(期限が来た命令だけ呼ぶ、通常経路の外)
///////////////////////////////////////////////////////////////////////////////////

#ifndef C68K_GEN
#if defined(__GNUC__) || defined(__clang__)
#define C68K_REFRESH_NOINLINE __attribute__((noinline))
#else
#define C68K_REFRESH_NOINLINE
#endif

// 命令の終わり(ccnt_end、正しい座標の CCnt)までに来た期限を処理し、課金するストールclkを返す。
// CPU->RefreshDL/RefreshFrac(次の期限)と CPU->RefreshCarry を更新する。
static C68K_REFRESH_NOINLINE int32_t c68k_refresh_hit(c68k_struc *CPU, int32_t ins_start,
                                                      int32_t ccnt_end, uint32_t opcode, uintptr_t pc)
{
    int32_t L = ins_start - ccnt_end;
    int32_t period = CPU->RefreshPeriodQ8;
    int32_t carry_in = CPU->RefreshCarry;
    int32_t o, stall = 0;
    int64_t x;

    o = (int32_t)(((int64_t)ins_start - CPU->RefreshDL) * 256 - CPU->RefreshFrac);
    CPU->RefreshCarry = 0;

    // 判定に使うPCは RET 時点(命令の直後)のゲストPC。主記憶 $000000〜$BFFFFF 以外は課金しない
    if ((((uint32_t)(pc - CPU->BasePC)) & 0x00FFFFFFu) < 0x00C00000u) {
        c68k_refresh_res r;
        c68k_refresh_calc(L, (int32_t)CPU->BusCnt, c68k_refresh_is_branch10(opcode, L),
                          carry_in, &o, period, &r);
        stall = r.stall;
        CPU->RefreshHitsDram += (uint32_t)r.hits;
        CPU->RefreshNegO += (uint32_t)r.nego;
        CPU->RefreshStall += (uint32_t)stall;
        if ((uint32_t)r.dmax > CPU->RefreshStallMax) CPU->RefreshStallMax = (uint32_t)r.dmax;
        if (r.carry_q8 > 0) { CPU->RefreshCarry = r.carry_q8; CPU->RefreshCarryN++; }
    } else {
        int32_t lim = (L > 0 ? (L < C68K_REFRESH_MAXLEN ? L : C68K_REFRESH_MAXLEN) : 0) * 256;
        while (o <= lim) { CPU->RefreshHitsOther++; o += period; }
    }

    x = (int64_t)ins_start * 256 - o;
    CPU->RefreshDL = c68k_refresh_floor256(x);
    CPU->RefreshFrac = (int32_t)(x - (int64_t)CPU->RefreshDL * 256);
    return stall;
}

// 時間ジャンプ(C68k_Release_Cycle)・停止中: t(正しい座標の CCnt)までに来た期限を課金せず進める
static C68K_REFRESH_NOINLINE void c68k_refresh_resync(c68k_struc *CPU, int32_t t)
{
    CPU->RefreshCarry = 0;
    while (t <= CPU->RefreshDL) {
        c68k_refresh_advance(&CPU->RefreshDL, &CPU->RefreshFrac, CPU->RefreshPeriodQ8);
        CPU->RefreshResync++;
    }
}

// 実行関数を出るとき: 期限を「現在(ccnt_now)から次の期限までの残り」に戻す(真の座標)
static void c68k_refresh_store(c68k_struc *CPU, int32_t ccnt_now)
{
    while ((int64_t)ccnt_now * 256 < (int64_t)CPU->RefreshDL * 256 + CPU->RefreshFrac) {
        c68k_refresh_advance(&CPU->RefreshDL, &CPU->RefreshFrac, CPU->RefreshPeriodQ8);
        CPU->RefreshResync++;
    }
    CPU->RefreshRemain = ccnt_now - CPU->RefreshDL;
}
#endif  // !C68K_GEN

// main exec function
//////////////////////

int32_t FASTCALL C68k_Exec(c68k_struc *cpu, int32_t cycle)
{
    extern void mx68k_log(const char* msg);
    char buf[128];
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] C68k_Exec entered cpu=%p cycle=%d", (void*)cpu, (int)cycle);
    mx68k_log(buf);
#endif
#ifndef C68K_GEN
#if 0
    register c68k_struc *CPU asm ("ebx");
    register uintptr_t PC asm ("esi");
    register int32_t CCnt asm ("edi");
//    register uint32_t Opcode asm ("edi");
//    c68k_struc *CPU;
//    uint32_t PC;
//    int32_t CCnt;
    uint32_t Opcode;
#else
//    register c68k_struc *CPU asm ("r10");
//    register uint32_t PC asm ("r11");
//    register int32_t CCnt asm ("r12");
//    register uint32_t Opcode asm ("r13");
    c68k_struc *CPU;
    uintptr_t PC;
    int32_t CCnt;
    uint32_t Opcode;
#endif
    int32_t InsStart;   // P976: 現命令の先頭の CCnt
    int32_t RefDL;      // P976: 次のリフレッシュ期限(CCnt座標)。無効時 C68K_REFRESH_NEVER、キャリー保留中 C68K_REFRESH_ALWAYS
#endif

#ifndef C68K_GEN

#ifndef C68K_NO_JUMP_TABLE
#ifdef C68K_CONST_JUMP_TABLE
    #include "c68k_ini.inc"
#endif
#else
    C68k_Initialised = 1;
#endif

    CPU = cpu;
    PC = CPU->PC;
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] Status=%08x PC=%p Initialised=%d", (unsigned)CPU->Status, (void*)PC, (int)C68k_Initialised);
    mx68k_log(buf);
#endif

    if (CPU->Status & (C68K_RUNNING | C68K_DISABLE | C68K_FAULTED))
    {
#ifndef C68K_NO_JUMP_TABLE
#ifndef C68K_CONST_JUMP_TABLE
        if (!C68k_Initialised) goto C68k_Init;
#endif
#endif
#ifdef MX68K_C68K_TRACE
        snprintf(buf, sizeof(buf), "[c68k] Early return Status=%08x", (unsigned)CPU->Status);
        mx68k_log(buf);
#endif
        return (CPU->Status | 0x80000000);
    }

    if (cycle <= 0) {
#ifdef MX68K_C68K_TRACE
        snprintf(buf, sizeof(buf), "[c68k] cycle <= 0 return %d", (int)(-cycle));
        mx68k_log(buf);
#endif
        return -cycle;
    }

    CPU->CycleToDo = CCnt = cycle;
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] CycleToDo=%d", (int)CCnt);
    mx68k_log(buf);
#endif

    // P976: 入口の割込み受付は最初の命令の一部(既定の並び)として扱う
    InsStart = CCnt;
    CPU->BusCnt = 0;
    if (CPU->RefreshPeriodQ8) CPU->RefreshDL = CCnt - CPU->RefreshRemain;

#ifndef C68K_DEBUG
    CHECK_INT
#else
    {
        int32_t line, vect;

        line = CPU->IRQLine;

        if ((line == 7) || (line > (int32_t)CPU->flag_I))
        {
            PRE_IO

            /* get vector */
            CPU->IRQLine = 0;
            vect = CPU->Interrupt_CallBack(line);
            if (vect == C68K_INT_ACK_AUTOVECTOR)
                vect = C68K_INTERRUPT_AUTOVECTOR_EX + (line & 7);

            /* adjust CCnt */
            CCnt -= c68k_exception_cycle_table[vect];

            /* swap A7 and USP */
            if (!CPU->flag_S)
            {
                uint32_t tmpSP;

                tmpSP = CPU->USP;
                CPU->USP = CPU->A[7];
                CPU->A[7] = tmpSP;
            }

            /* push PC and SR */
            PUSH_32_F((uint32_t)(PC - CPU->BasePC))
            PUSH_16_F(GET_SR)

            /* adjust SR */
            CPU->flag_S = C68K_SR_S;
            CPU->flag_I = line;

            /* fetch new PC */
            READ_LONG_F(vect * 4, PC)
            SET_PC(PC)

            POST_IO
        }
    }
#endif

    if (CPU->Status & (C68K_HALTED | C68K_WAITING))
    {
        // P976: 停止中も時間は進む(課金なし)
        if (CPU->RefreshPeriodQ8)
        {
            CPU->RefreshCarry = 0;
            c68k_refresh_store(CPU, 0);
        }
        return CPU->CycleToDo;
    }

    RefDL = CPU->RefreshPeriodQ8 ? (CPU->RefreshCarry ? C68K_REFRESH_ALWAYS : CPU->RefreshDL)
                                 : C68K_REFRESH_NEVER;

    CPU->CycleSup = 0;
    CPU->Status |= C68K_RUNNING;
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] Before NEXT, CCnt=%d", (int)CCnt);
    mx68k_log(buf);
#endif

#ifndef C68K_DEBUG
    PRE_IO
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] NEXT: PRE_IO done");
    mx68k_log(buf);
#endif
    Opcode = FETCH_WORD;
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] NEXT: FETCH_WORD done Opcode=%04x PC=%p", (unsigned)Opcode, (void*)PC);
    mx68k_log(buf);
#endif
    PC += 2;
    TRACE(PC, CPU, Opcode, CCnt);
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] NEXT: before goto Opcode=%04x", (unsigned)Opcode);
    mx68k_log(buf);
    snprintf(buf, sizeof(buf), "[c68k] NEXT: JumpTable[%04x]=%p", (unsigned)Opcode, JumpTable[Opcode]);
    mx68k_log(buf);
#endif
    goto *JumpTable[Opcode];
#else
#ifdef C68K_NO_JUMP_TABLE
    NEXT
#else
    Opcode = FETCH_WORD;
    PC += 2;
    goto *JumpTable[Opcode];
#endif
#endif

#ifdef C68K_NO_JUMP_TABLE
SwitchTable:
    switch(Opcode)
    {
#endif
    #include "c68k_op0.inc"
    #include "c68k_op1.inc"
    #include "c68k_op2.inc"
    #include "c68k_op3.inc"
    #include "c68k_op4.inc"
    #include "c68k_op5.inc"
    #include "c68k_op6.inc"
    #include "c68k_op7.inc"
    #include "c68k_op8.inc"
    #include "c68k_op9.inc"
    #include "c68k_opA.inc"
    #include "c68k_opB.inc"
    #include "c68k_opC.inc"
    #include "c68k_opD.inc"
    #include "c68k_opE.inc"
    #include "c68k_opF.inc"
#ifdef C68K_NO_JUMP_TABLE
    }
#endif

C68k_Exec_End:
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] Exec_End CCnt=%d", (int)CCnt);
    mx68k_log(buf);
#endif
    // P976: 真の時刻 t = CCnt + CycleSup(C68k_Set_IRQ による座標のずれを戻した値)で期限を判定する
    {
        int32_t t = CCnt + CPU->CycleSup;
        if (t <= RefDL)
        {
            if (CPU->RefreshJump || (CPU->Status & C68K_HALTED))
                c68k_refresh_resync(CPU, t);
            else
                CCnt -= c68k_refresh_hit(CPU, InsStart, t, Opcode, PC);
            RefDL = CPU->RefreshCarry ? C68K_REFRESH_ALWAYS : CPU->RefreshDL;
        }
        CPU->RefreshJump = 0;
    }

C68k_Exec_End_Int:
    CHECK_INT
    if ((CCnt += CPU->CycleSup) > 0)
    {
        CPU->CycleSup = 0;
        NEXT;
    }

C68k_Exec_Really_End:
#ifdef MX68K_C68K_TRACE
    snprintf(buf, sizeof(buf), "[c68k] Exec_Really_End");
    mx68k_log(buf);
#endif
    CPU->Status &= ~C68K_RUNNING;
    CPU->PC = PC;
    // P976: 次の期限を「ここからの残り」に戻して次の呼出しへ引き継ぐ(キャリーは CPU->RefreshCarry に残る)
    if (CPU->RefreshPeriodQ8) c68k_refresh_store(CPU, CCnt);

    return (CPU->CycleToDo - CCnt);

    // P976: RET で期限の成立を検出したとき(CCnt > 0 のときだけ goto で来る。素通りでは到達しない)
C68k_Refresh_Hit:
    CCnt -= c68k_refresh_hit(CPU, InsStart, CCnt, Opcode, PC);
    RefDL = CPU->RefreshCarry ? C68K_REFRESH_ALWAYS : CPU->RefreshDL;
    if (CCnt <= 0) goto C68k_Exec_End_Int;
    NEXT

#ifndef C68K_CONST_JUMP_TABLE
#ifndef C68K_NO_JUMP_TABLE
C68k_Init:
    {
        uint32_t i, j;

        #include "c68k_ini.inc"
        
        C68k_Initialised = 1;
    }
    
    return 0;
#endif
#endif
#else
    return 0;
#endif
}

