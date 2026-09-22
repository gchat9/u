/*
 * mkfont.c — bake DejaVuSansMono.ttf → font.bfnt
 *
 * Build:
 *   curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
 *   cc -O2 -o mkfont mkfont.c -lm
 * Run:
 *   ./mkfont
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_PATH   "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define FONT_PX     26
#define NUM_GLYPHS  256

static void w16(FILE *f, uint16_t v) {          /* write LE uint16 */
    uint8_t b[2] = { v & 0xFF, v >> 8 };
    fwrite(b, 1, 2, f);
}

int main(void)
{
    /* ── load TTF ─────────────────────────────────────────────────── */
    FILE *f = fopen(FONT_PATH, "rb");
    if (!f) { perror(FONT_PATH); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f); rewind(f);
    uint8_t *ttf = malloc(fsz);
    fread(ttf, 1, fsz, f); fclose(f);

    stbtt_fontinfo fi;
    stbtt_InitFont(&fi, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));

    float scale = stbtt_ScaleForPixelHeight(&fi, FONT_PX);

    int asc, desc, lgap;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &lgap);

    uint16_t baseline = (uint16_t)(asc * scale + 0.5f);
    uint16_t cell_h   = (uint16_t)((asc - desc) * scale + 0.5f);

    /* For a monospace font every advance is identical; use 'M'. */
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&fi, 'M', &adv, &lsb);
    uint16_t cell_w = (uint16_t)(adv * scale + 0.5f);

    fprintf(stderr, "cell %u×%u  baseline %u  scale %.4f\n",
            cell_w, cell_h, baseline, (double)scale);

    /* ── write header ─────────────────────────────────────────────── */
    FILE *out = fopen("font.bfnt", "wb");
    if (!out) { perror("font.bfnt"); return 1; }

    fwrite("BFNT", 1, 4, out);
    w16(out, cell_w);
    w16(out, cell_h);
    w16(out, baseline);
    w16(out, 0);              /* first_cp  */
    w16(out, NUM_GLYPHS);
    w16(out, 0);              /* reserved  */

    /* ── render glyphs ────────────────────────────────────────────── */
    /*
     * For each codepoint we produce a cell_w × cell_h alpha bitmap.
     * Control characters and C1 bytes (0x80–0x9F) stay all-zero.
     * We use "max" blending so glyphs that overflow their cell edge
     * don't just get clipped — the brightest value wins.
     */
    uint8_t *cell = calloc(cell_w * cell_h, 1);

    for (int cp = 0; cp < NUM_GLYPHS; cp++) {
        memset(cell, 0, cell_w * cell_h);

        int printable = (cp >= 0x20 && cp <= 0x7E)   /* ASCII        */
                     || (cp >= 0xA0 && cp <= 0xFF);  /* Latin-1 supp */

        if (printable) {
            int gw, gh, xoff, yoff;
            uint8_t *bm = stbtt_GetCodepointBitmap(
                            &fi, scale, scale, cp, &gw, &gh, &xoff, &yoff);
            /*
             * xoff : horizontal pen→bitmap-left offset (usually ≥ 0)
             * yoff : baseline→bitmap-top  offset (usually negative)
             */
            for (int gy = 0; gy < gh; gy++) {
                int ry = (int)baseline + yoff + gy;
                if (ry < 0 || ry >= cell_h) continue;
                for (int gx = 0; gx < gw; gx++) {
                    int rx = xoff + gx;
                    if (rx < 0 || rx >= cell_w) continue;
                    uint8_t v = bm[gy * gw + gx];
                    if (v > cell[ry * cell_w + rx])
                        cell[ry * cell_w + rx] = v;
                }
            }
            stbtt_FreeBitmap(bm, NULL);
        }

        fwrite(cell, 1, cell_w * cell_h, out);
    }

    free(cell); free(ttf); fclose(out);
    fprintf(stderr, "wrote font.bfnt  (%d glyphs × %u × %u = %u bytes data)\n",
            NUM_GLYPHS, cell_w, cell_h,
            (unsigned)(NUM_GLYPHS * cell_w * cell_h));
    return 0;
}
