#ifndef M68K_H
#define M68K_H

#include <stdint.h>

enum {
    M68K_CPU_TYPE_68000 = 1,
    M68K_REG_PC,
    M68K_REG_USP,
    M68K_REG_MSP,
    M68K_REG_SR,
    M68K_REG_D0,
    M68K_REG_D1,
    M68K_REG_D2,
    M68K_REG_D3,
    M68K_REG_D4,
    M68K_REG_D5,
    M68K_REG_D6,
    M68K_REG_D7,
    M68K_REG_A0,
    M68K_REG_A1,
    M68K_REG_A2,
    M68K_REG_A3,
    M68K_REG_A4,
    M68K_REG_A5,
    M68K_REG_A6,
    M68K_REG_A7
};

#ifdef __cplusplus
extern "C" {
#endif

void m68k_set_cpu_type(int type);
void m68k_init(void);
void m68k_pulse_reset(void);
int m68k_execute(int cycles);
void m68k_set_irq(int irqline);
uint32_t m68k_get_reg(void *context, int regnum);
void m68k_set_reg(int regnum, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif
