#include "cia.h"
#include <string.h>

/* ── CIA state ───────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t pra, prb;
    uint8_t ddra, ddrb;
    uint16_t ta, tb;          /* timer current values */
    uint16_t ta_latch, tb_latch;
    uint8_t  icr_mask;        /* interrupt mask register */
    uint8_t  icr_data;        /* interrupt data (flags) */
    uint8_t  cra, crb;
    uint8_t  sp;
    uint32_t tod;
} CIA;

static CIA ciaa, ciab;

/* Keyboard state: circular buffer of pending scancodes */
#define KEY_BUF_SIZE 16
static uint8_t key_buf[KEY_BUF_SIZE];
static int key_head = 0, key_tail = 0;

/* Joystick state (used to populate JOY0DAT / CIA PRA fire buttons) */
static uint16_t joy0dat_val = 0;
static uint16_t joy1dat_val = 0;
static uint8_t  fire0 = 1;  /* active-low; 1 = not pressed */
static uint8_t  fire1 = 1;

/* ── Init ────────────────────────────────────────────────────────────────── */

void ciaa_init(void)
{
    memset(&ciaa, 0, sizeof(ciaa));
    ciaa.pra = 0xFF;   /* all high = no keys, fire released */
    ciaa.prb = 0xFF;
}

void ciab_init(void)
{
    memset(&ciab, 0, sizeof(ciab));
    ciab.pra = 0xFF;
    ciab.prb = 0xFF;
}

/* ── Key queue ───────────────────────────────────────────────────────────── */

void cia_key_event(uint8_t amiga_keycode, int pressed)
{
    /* Amiga key protocol: send ~(scancode << 1 | (pressed ? 0 : 1))
     * and then a handshake.  We simplify: put the raw make/break in buffer. */
    int next = (key_tail + 1) % KEY_BUF_SIZE;
    if (next != key_head) {
        /* encode: bit 7 = 0 for make, 1 for break (inverted from CIA hardware) */
        uint8_t code = (uint8_t)(amiga_keycode & 0x7F) | (pressed ? 0 : 0x80);
        key_buf[key_tail] = code;
        key_tail = next;
        ciaa.icr_data |= 0x08;  /* set SP bit (keyboard) */
    }
}

void cia_joy_update(uint16_t j0dat, uint16_t j1dat, uint8_t f0, uint8_t f1)
{
    joy0dat_val = j0dat;
    joy1dat_val = j1dat;
    fire0 = f0;
    fire1 = f1;
}

/* ── CIA-A register access ───────────────────────────────────────────────── */

uint8_t ciaa_read(int reg)
{
    switch (reg) {
    case CIA_PRA:
        /* Bit 6 = fire button 1 (joy port 2), bit 7 = fire button 2 */
        return (uint8_t)(ciaa.pra & ~((!fire0) ? 0x40 : 0) & ~((!fire1) ? 0x80 : 0));
    case CIA_PRB:  return ciaa.prb;
    case CIA_DDRA: return ciaa.ddra;
    case CIA_DDRB: return ciaa.ddrb;
    case CIA_TALO: return (uint8_t)(ciaa.ta & 0xFF);
    case CIA_TAHI: return (uint8_t)(ciaa.ta >> 8);
    case CIA_TBLO: return (uint8_t)(ciaa.tb & 0xFF);
    case CIA_TBHI: return (uint8_t)(ciaa.tb >> 8);
    case CIA_SP:
        /* Return next key from buffer (inverted Amiga serial protocol) */
        if (key_head != key_tail) {
            uint8_t raw = key_buf[key_head];
            key_head = (key_head + 1) % KEY_BUF_SIZE;
            /* Amiga keyboard serial byte: bits 7..1 = scancode, bit 0 = up/down */
            return (uint8_t)(~((raw & 0x7F) << 1 | ((raw & 0x80) ? 1 : 0)));
        }
        return 0;
    case CIA_ICR:
        {
            uint8_t data = ciaa.icr_data;
            uint8_t v    = data & 0x1F;
            if (v & ciaa.icr_mask) v |= 0x80;
            ciaa.icr_data = 0;  /* reading clears flags */
            return v;
        }
    case CIA_CRA: return ciaa.cra;
    case CIA_CRB: return ciaa.crb;
    default: return 0xFF;
    }
}

void ciaa_write(int reg, uint8_t val)
{
    switch (reg) {
    case CIA_PRA:  ciaa.pra  = val; break;
    case CIA_PRB:  ciaa.prb  = val; break;
    case CIA_DDRA: ciaa.ddra = val; break;
    case CIA_DDRB: ciaa.ddrb = val; break;
    case CIA_TALO: ciaa.ta_latch = (ciaa.ta_latch & 0xFF00) | val; break;
    case CIA_TAHI:
        ciaa.ta_latch = (ciaa.ta_latch & 0x00FF) | ((uint16_t)val << 8);
        if (!(ciaa.cra & 1)) ciaa.ta = ciaa.ta_latch;
        break;
    case CIA_TBLO: ciaa.tb_latch = (ciaa.tb_latch & 0xFF00) | val; break;
    case CIA_TBHI:
        ciaa.tb_latch = (ciaa.tb_latch & 0x00FF) | ((uint16_t)val << 8);
        if (!(ciaa.crb & 1)) ciaa.tb = ciaa.tb_latch;
        break;
    case CIA_ICR:
        if (val & 0x80) ciaa.icr_mask |=  (val & 0x7F);
        else            ciaa.icr_mask &= ~(val & 0x7F);
        break;
    case CIA_CRA:
        ciaa.cra = val;
        if (val & 0x10) { ciaa.ta = ciaa.ta_latch; ciaa.cra &= ~0x10; }
        break;
    case CIA_CRB:
        ciaa.crb = val;
        if (val & 0x10) { ciaa.tb = ciaa.tb_latch; ciaa.crb &= ~0x10; }
        break;
    default: break;
    }
}

/* ── CIA-B register access ───────────────────────────────────────────────── */

uint8_t ciab_read(int reg)
{
    switch (reg) {
    case CIA_PRA:  return ciab.pra;
    case CIA_PRB:  return ciab.prb;
    case CIA_DDRA: return ciab.ddra;
    case CIA_DDRB: return ciab.ddrb;
    case CIA_TALO: return (uint8_t)(ciab.ta & 0xFF);
    case CIA_TAHI: return (uint8_t)(ciab.ta >> 8);
    case CIA_TBLO: return (uint8_t)(ciab.tb & 0xFF);
    case CIA_TBHI: return (uint8_t)(ciab.tb >> 8);
    case CIA_ICR:
        {
            /* ICR read returns all pending flags (bit 7 set if any enabled flag set),
             * then clears them. The mask only affects interrupt delivery, not the read. */
            uint8_t data = ciab.icr_data;
            uint8_t v    = data & 0x1F;          /* raw flags */
            if (v & ciab.icr_mask) v |= 0x80;   /* IR bit: any enabled flag set */
            ciab.icr_data = 0;
            return v;
        }
    case CIA_CRA: return ciab.cra;
    case CIA_CRB: return ciab.crb;
    default: return 0xFF;
    }
}

void ciab_write(int reg, uint8_t val)
{
    switch (reg) {
    case CIA_PRA:  ciab.pra  = val; break;
    case CIA_PRB:  ciab.prb  = val; break;
    case CIA_DDRA: ciab.ddra = val; break;
    case CIA_DDRB: ciab.ddrb = val; break;
    case CIA_TALO: ciab.ta_latch = (ciab.ta_latch & 0xFF00) | val; break;
    case CIA_TAHI:
        ciab.ta_latch = (ciab.ta_latch & 0x00FF) | ((uint16_t)val << 8);
        /* Writing TAHI while stopped loads the latch into the counter */
        if (!(ciab.cra & 1)) ciab.ta = ciab.ta_latch;
        /* If one-shot mode with LOAD strobe implied (some games rely on this):
         * fire the timer immediately so polled ICR reads see the TA flag. */
        if ((ciab.cra & 0x08) && !(ciab.cra & 0x01)) {
            ciab.icr_data |= 0x01;   /* TA underflow */
        }
        break;
    case CIA_TBLO: ciab.tb_latch = (ciab.tb_latch & 0xFF00) | val; break;
    case CIA_TBHI:
        ciab.tb_latch = (ciab.tb_latch & 0x00FF) | ((uint16_t)val << 8);
        if (!(ciab.crb & 1)) ciab.tb = ciab.tb_latch;
        break;
    case CIA_ICR:
        if (val & 0x80) ciab.icr_mask |=  (val & 0x7F);
        else            ciab.icr_mask &= ~(val & 0x7F);
        break;
    case CIA_CRA:
        ciab.cra = val;
        /* Bit 4 (LOAD) forces latch → counter immediately */
        if (val & 0x10) { ciab.ta = ciab.ta_latch; ciab.cra &= ~0x10; }
        break;
    case CIA_CRB:
        ciab.crb = val;
        if (val & 0x10) { ciab.tb = ciab.tb_latch; ciab.crb &= ~0x10; }
        break;
    default: break;
    }
}

/* ── Tick ────────────────────────────────────────────────────────────────── */

/* ~228 CPU cycles per scanline */
#define CYCLES_PER_LINE 228

int cia_tick(void)
{
    int irq_mask = 0;

    /* Timer A */
    if ((ciaa.cra & 1) && !(ciaa.cra & 0x20)) {  /* running, not one-shot finished */
        if (ciaa.ta <= CYCLES_PER_LINE) {
            ciaa.icr_data |= 0x01;
            if (ciaa.icr_mask & 0x01) irq_mask |= 1;
            ciaa.ta = ciaa.ta_latch;              /* reload */
            if (ciaa.cra & 8) { ciaa.cra &= ~1; } /* one-shot: stop */
        } else {
            ciaa.ta -= CYCLES_PER_LINE;
        }
    }

    /* Timer B */
    if ((ciaa.crb & 1) && !(ciaa.crb & 0x40)) {
        if (ciaa.tb <= CYCLES_PER_LINE) {
            ciaa.icr_data |= 0x02;
            if (ciaa.icr_mask & 0x02) irq_mask |= 2;
            ciaa.tb = ciaa.tb_latch;
            if (ciaa.crb & 8) { ciaa.crb &= ~1; }
        } else {
            ciaa.tb -= CYCLES_PER_LINE;
        }
    }

    /* Keyboard: if SP bit set and PORTS interrupt enabled → level 2 IRQ */
    if (ciaa.icr_data & 0x08)
        irq_mask |= 4;

    /* ── CIA-B timers ────────────────────────────────────────────────── */
    /* Timer A */
    if ((ciab.cra & 1) && !(ciab.cra & 0x20)) {
        if (ciab.ta <= CYCLES_PER_LINE) {
            ciab.icr_data |= 0x01;
            ciab.ta = ciab.ta_latch;
            if (ciab.cra & 8) { ciab.cra &= ~1; }   /* one-shot: stop */
        } else {
            ciab.ta -= CYCLES_PER_LINE;
        }
    }

    /* Timer B */
    if ((ciab.crb & 1) && !(ciab.crb & 0x40)) {
        if (ciab.tb <= CYCLES_PER_LINE) {
            ciab.icr_data |= 0x02;
            ciab.tb = ciab.tb_latch;
            if (ciab.crb & 8) { ciab.crb &= ~1; }
        } else {
            ciab.tb -= CYCLES_PER_LINE;
        }
    }

    return irq_mask;
}
