/* pbm.h: dump a gfx page-layout framebuffer as a binary PBM (P4). Host only. */
#pragma once
#include <stdio.h>
#include "gfx.h"

static inline int pbm_write(const char *path, uint8_t fb[GFX_PAGES][GFX_W])
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P4\n%d %d\n", GFX_W, GFX_H);
    for (int y = 0; y < GFX_H; y++) {
        for (int xb = 0; xb < GFX_W / 8; xb++) {
            unsigned char v = 0;
            for (int k = 0; k < 8; k++)
                if (!((fb[y >> 3][xb * 8 + k] >> (y & 7)) & 1)) v |= (unsigned char)(0x80 >> k); /* PBM 1 = black */
            fputc(v, f);
        }
    }
    fclose(f);
    return 0;
}
