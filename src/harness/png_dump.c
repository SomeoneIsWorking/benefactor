/* png_dump.c — write an ARGB framebuffer (region) straight to a PNG file.
 *
 * Backs the REPL `shot` command so inspecting a frame is one command instead
 * of fbw + an ad-hoc Python decode every time. Uses the zlib already vendored
 * for PUAE (deps/libz). Integer `scale` (nearest) for zoomed crops. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

static void put32(uint8_t *p, uint32_t v)
{ p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put32(hdr, len); memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    uint32_t crc = crc32(0, (const uint8_t *)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t cb[4]; put32(cb, crc);
    return fwrite(cb, 1, 4, f) == 4 ? 0 : -1;
}

/* Write the [x0,y0,w,h] region of `argb` (row stride `stride` pixels) as an
 * RGB PNG at integer scale `scale`. Returns 0 on success. */
int png_dump_region(const char *path, const uint32_t *argb, int stride,
                    int x0, int y0, int w, int h, int scale)
{
    if (!path || !argb || w <= 0 || h <= 0 || scale < 1) return -1;
    int W = w * scale, H = h * scale;

    /* Raw scanlines: filter byte 0 + RGB triples. */
    size_t raw_len = (size_t)H * (1 + (size_t)W * 3);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return -1;
    uint8_t *p = raw;
    for (int y = 0; y < H; y++) {
        *p++ = 0;
        const uint32_t *src = argb + (size_t)(y0 + y / scale) * stride + x0;
        for (int x = 0; x < W; x++) {
            uint32_t v = src[x / scale];
            *p++ = (v >> 16) & 0xFF; *p++ = (v >> 8) & 0xFF; *p++ = v & 0xFF;
        }
    }

    uLongf zcap = compressBound(raw_len);
    uint8_t *z = malloc(zcap);
    int rc = -1;
    if (z && compress2(z, &zcap, raw, raw_len, 6) == Z_OK) {
        FILE *f = fopen(path, "wb");
        if (f) {
            static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};
            uint8_t ihdr[13];
            put32(ihdr, (uint32_t)W); put32(ihdr + 4, (uint32_t)H);
            ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
            rc = fwrite(sig, 1, 8, f) == 8
              && write_chunk(f, "IHDR", ihdr, 13) == 0
              && write_chunk(f, "IDAT", z, (uint32_t)zcap) == 0
              && write_chunk(f, "IEND", NULL, 0) == 0 ? 0 : -1;
            fclose(f);
        }
    }
    free(z); free(raw);
    return rc;
}
