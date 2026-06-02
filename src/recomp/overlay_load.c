/* overlay_load.c — pure overlay loaders (see overlay_load.h).
 *
 * Extracted from the pc_overrides loaders so BOTH the game (native_overlay_load
 * / native_overlay_load_d0 wrappers) and the standalone bank-input dumper share
 * one source of truth, and so the dumper links WITHOUT the generated game code.
 * Pure disk read + ATN! decrunch + relocation on g_mem. No game-state, no SDL,
 * no generated code. */
#include <stdint.h>
#include <string.h>
#include "disk_boot.h"

extern uint8_t *g_mem;

/* main / intro bank: the boot loader's Load(Disk.1 @$1880, $2442E -> $3000) +
 * ATN! decrunch. Mirrors the step in pc_common_bringup. */
void overlay_load_main(void)
{
    disk_boot_load(1, 0x1880u, 0x3000u, 0x2442Eu);
    atn_decrunch(0x3000u);
}

/* gp / title bank. The $6D714 block copy ($6D734 -> $150, $2A08 bytes) needs the
 * boot-decrunch source, so a fresh overlay_load_main() must precede this. Then
 * two title chunks: E1 raw -> $50000, E2 ATN! -> $3330 (decrunch). */
void overlay_load_title(void)
{
    memcpy(g_mem + 0x150u, g_mem + 0x6D734u, 0x2A08u);
    disk_boot_load(1, 0x0F3780u, 0x00050000u, 0x1880u);    /* E1: raw */
    disk_boot_load(1, 0x026270u, 0x00003330u, 0x2E2E4u);   /* E2: ATN! crunched */
    atn_decrunch(0x00003330u);
    /* Dest-pointer stack the original loader builds at $100/$104 (big-endian). */
    g_mem[0x100] = 0x00; g_mem[0x101] = 0x05; g_mem[0x102] = 0x00; g_mem[0x103] = 0x00;
    g_mem[0x104] = 0x00; g_mem[0x105] = 0x00; g_mem[0x106] = 0x33; g_mem[0x107] = 0x30;
}

/* gpl / gameplay bank. $6D714 block copy first (fresh overlay_load_main() must
 * precede), then 3 ATN! chunks (reloc table -> $6E000, code/data -> $3330,
 * code -> $577000), then the two-pass pointer relocation the loader runs. */
void overlay_load_gameplay(void)
{
    static const struct { uint32_t off, dst, len; } ch[3] = {
        { 0x0548A0u, 0x06E000u, 0x001D1Au },
        { 0x0565BAu, 0x003330u, 0x012404u },
        { 0x0689BEu, 0x577000u, 0x012A1Cu },
    };
    memcpy(g_mem + 0x150u, g_mem + 0x6D734u, 0x2A08u);
    for (int i = 0; i < 3; i++) { disk_boot_load(1, ch[i].off, ch[i].dst, ch[i].len);
                                  atn_decrunch(ch[i].dst); }
    g_mem[0x100]=0x00; g_mem[0x101]=0x06; g_mem[0x102]=0xE0; g_mem[0x103]=0x00;
    g_mem[0x104]=0x00; g_mem[0x105]=0x00; g_mem[0x106]=0x33; g_mem[0x107]=0x30;
    g_mem[0x108]=0x00; g_mem[0x109]=0x57; g_mem[0x10A]=0x70; g_mem[0x10B]=0x00;

    #define RD32(a) (((uint32_t)g_mem[(a)]<<24)|((uint32_t)g_mem[(a)+1]<<16)\
                    |((uint32_t)g_mem[(a)+2]<<8)|g_mem[(a)+3])
    #define WR32(a,v) do{uint32_t _v=(v),_a=(a); g_mem[_a]=_v>>24; g_mem[_a+1]=_v>>16;\
                        g_mem[_a+2]=_v>>8; g_mem[_a+3]=_v;}while(0)
    /* Two relocation passes (loader $204-$23C): one table at a0=$6E000, all
     * writing into a4=$577000. pass1 base=$577000, pass2 base=$3330; each = 2
     * blocks of [count, base-adjust, offsets...]; *(a4+off) += running base. */
    { uint32_t a0 = 0x06E000u, a4 = 0x577000u;
      uint32_t base[2] = { a4, 0x00003330u };
      for (int pass = 0; pass < 2; pass++) {
          uint32_t d1 = base[pass];
          for (int blk = 0; blk < 2; blk++) {
              uint32_t d7 = RD32(a0); a0 += 4;
              d1 += RD32(a0); a0 += 4;
              for (uint32_t k = 0; k <= d7; k++) { uint32_t off = RD32(a0); a0 += 4;
                                                   WR32(a4 + off, RD32(a4 + off) + d1); } } } }
    #undef RD32
    #undef WR32
}

/* credits / end-game bank ($150 d0=3): one Disk.3 chunk -> $3330 (ATN!), then
 * jmp via mem[$100] = $3330. Self-contained (no $6D734 block copy). */
void overlay_load_credits(void)
{
    disk_boot_load(3, 0x000C7100u, 0x00003330u, 0x0001888Cu);
    atn_decrunch(0x00003330u);
    g_mem[0x100] = 0x00; g_mem[0x101] = 0x00;
    g_mem[0x102] = 0x33; g_mem[0x103] = 0x30;
}
