#pragma once
/*
 * amiga/cia.h  –  CIA-A and CIA-B emulation.
 *
 * CIA-A ($BFE001, odd bytes):
 *   PRA  = keyboard / parallel port
 *   PRB  = parallel port
 *   TALO/TAHI = timer A
 *   TBLO/TBHI = timer B
 *   ICR  = interrupt control
 *
 * CIA-B ($BFD000, even bytes):
 *   PRA  = disk drive select / motor
 *   PRB  = disk data / direction
 */

#include <stdint.h>

/* Register indices (offset >> 8, masked to 0xF) */
#define CIA_PRA   0
#define CIA_PRB   1
#define CIA_DDRA  2
#define CIA_DDRB  3
#define CIA_TALO  4
#define CIA_TAHI  5
#define CIA_TBLO  6
#define CIA_TBHI  7
#define CIA_TOD_LOW  8
#define CIA_TOD_MID  9
#define CIA_TOD_HI   10
#define CIA_UNUSED   11
#define CIA_SP    12
#define CIA_ICR   13
#define CIA_CRA   14
#define CIA_CRB   15

void     ciaa_init(void);
uint8_t  ciaa_read(int reg);
void     ciaa_write(int reg, uint8_t val);

void     ciab_init(void);
uint8_t  ciab_read(int reg);
void     ciab_write(int reg, uint8_t val);

/* Called from input.c to inject a key scancode (Amiga raw code). */
void     cia_key_event(uint8_t amiga_keycode, int pressed);

/* Called from input.c to set joystick register values. */
void     cia_joy_update(uint16_t joy0dat, uint16_t joy1dat, uint8_t fire0, uint8_t fire1);

/* Tick both CIA timers by one scanline's worth of cycles (~228).
 * Returns mask of CIA-A interrupts that fired (bit 0 = timer A, bit 1 = timer B). */
int      cia_tick(void);
