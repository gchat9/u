/*
 * mkfont.c — bake a monospace TTF into font.bfnt, one glyph per slot of
 * the layout defined in charset.h.
 *
 * Normally run via this directory's Makefile (which also fetches
 * stb_truetype.h). By hand:
 *   cc -O2 -o mkfont mkfont.c -lm && ./mkfont [narrow.ttf [wide.ttf]]
 * The narrow TTF defaults to DejaVu Sans Mono; any monospace font works.
 * The wide TTF supplies the double-width glyphs (charset_wide_extra[]):
 * it defaults to Noto Sans CJK; any font with those characters works
 * (a .ttc is read at index 0).
 *
 * font.bfnt layout, all little-endian:
 *   "BFNT", cell_w, cell_h, baseline, first_cp, num_glyphs, num_wide  (16 bytes)
 *   num_glyphs   narrow glyphs, cell_w x cell_h bytes of alpha each
 *   num_wide     wide glyphs, (2*cell_w) x cell_h bytes of alpha each
 * (num_wide is the header's old "reserved" word, 0 in files without a
 * wide section.)
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"

#define DEFAULT_TTF  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define DEFAULT_WTTF "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
#define FONT_PX     26
#define NUM_GLYPHS  CHARSET_COUNT        /* only what charset.h defines */
#define NUM_WIDE    CHARSET_WIDE_COUNT
#define FONT_MAX    (256 * 1024)         /* keep equal to xterm.c's */

static void w16(FILE *f, uint16_t v) {          /* write LE uint16 */
    uint8_t b[2] = { v & 0xFF, v >> 8 };
    fwrite(b, 1, 2, f);
}

static uint8_t *load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f); rewind(f);
    uint8_t *b = malloc(n);
    if (!b || fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "%s: read error\n", path); exit(1); }
    fclose(f);
    return b;
}

/*
 * Rasterize codepoint cp of font fi into the w x h alpha bitmap `cell`,
 * with the pen at x = pen_x on row `baseline`. "max" blending, so glyphs
 * that overflow their cell edge don't just get clipped -- the brightest
 * value wins. A codepoint the font lacks is reported and left blank
 * rather than silently baked as .notdef.
 */
static void draw_cp(stbtt_fontinfo *fi, float scale, uint32_t cp, uint8_t *cell,
                    int w, int h, int baseline, int pen_x)
{
    if (!stbtt_FindGlyphIndex(fi, (int)cp)) {
        fprintf(stderr, "warning: U+%04X is not in the font\n", (unsigned)cp);
        return;
    }
    int gw, gh, xoff, yoff;
    uint8_t *bm = stbtt_GetCodepointBitmap(fi, scale, scale, (int)cp, &gw, &gh, &xoff, &yoff);
    /*
     * xoff : horizontal pen→bitmap-left offset (usually ≥ 0)
     * yoff : baseline→bitmap-top  offset (usually negative)
     */
    for (int gy = 0; gy < gh; gy++) {
        int ry = baseline + yoff + gy;
        if (ry < 0 || ry >= h) continue;
        for (int gx = 0; gx < gw; gx++) {
            int rx = pen_x + xoff + gx;
            if (rx < 0 || rx >= w) continue;
            uint8_t v = bm[gy * gw + gx];
            if (v > cell[ry * w + rx]) cell[ry * w + rx] = v;
        }
    }
    stbtt_FreeBitmap(bm, NULL);
}

int main(int argc, char **argv)
{
    /* ── load TTF ─────────────────────────────────────────────────── */
    const char *ttf_path  = argc > 1 ? argv[1] : DEFAULT_TTF;
    const char *wttf_path = argc > 2 ? argv[2] : DEFAULT_WTTF;
    uint8_t *ttf = load_file(ttf_path);

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
    w16(out, NUM_WIDE);       /* num_wide  */

    /* ── render glyphs ────────────────────────────────────────────── */
    /*
     * For each glyph slot (0..NUM_GLYPHS-1) we look up which Unicode
     * codepoint it holds (see charset.h -- ASCII, then accented
     * letters, then box-drawing/block/arrow extras) and produce a
     * cell_w × cell_h alpha bitmap for it. A slot with a codepoint of
     * 0 stays all-zero. We use "max" blending so glyphs
     * that overflow their cell edge don't just get clipped — the
     * brightest value wins.
     */
    uint8_t *cell = calloc(cell_w * cell_h, 1);

    for (int slot = 0; slot < NUM_GLYPHS; slot++) {
        memset(cell, 0, cell_w * cell_h);

        uint32_t cp = charset_codepoint(slot);
        if (cp) draw_cp(&fi, scale, cp, cell, cell_w, cell_h, baseline, 0);

        fwrite(cell, 1, cell_w * cell_h, out);
    }

    free(cell);

    /* ── wide glyphs ──────────────────────────────────────────────── */
    /*
     * Two cells wide, same height and baseline. The wide font is scaled
     * to the same pixel em as the narrow one, so its glyphs have the
     * size they'd have next to the narrow text, and each is centred on
     * its advance within the two cells.
     */
    uint8_t *wttf = load_file(wttf_path);
    stbtt_fontinfo wfi;
    stbtt_InitFont(&wfi, wttf, stbtt_GetFontOffsetForIndex(wttf, 0));
    float em_px  = scale / stbtt_ScaleForMappingEmToPixels(&fi, 1.0f);
    float wscale = stbtt_ScaleForMappingEmToPixels(&wfi, em_px);
    int   ww     = 2 * cell_w;
    uint8_t *wcell = calloc(ww * cell_h, 1);

    for (int slot = 0; slot < NUM_WIDE; slot++) {
        memset(wcell, 0, ww * cell_h);
        uint32_t cp = charset_wide_codepoint(slot);
        int wadv, wlsb;
        stbtt_GetCodepointHMetrics(&wfi, (int)cp, &wadv, &wlsb);
        int pen_x = (int)((ww - wadv * wscale) / 2 + 0.5f);
        draw_cp(&wfi, wscale, cp, wcell, ww, cell_h, baseline, pen_x);
        fwrite(wcell, 1, ww * cell_h, out);
    }

    free(wcell); free(wttf); free(ttf); fclose(out);

    unsigned total = 16 + NUM_GLYPHS * cell_w * cell_h + NUM_WIDE * ww * cell_h;
    fprintf(stderr, "wrote font.bfnt  (%d narrow + %d wide glyphs, %u bytes; "
            "+%d of each would be %u)\n", NUM_GLYPHS, NUM_WIDE, total, CHARSET_RESERVE,
            total + CHARSET_RESERVE * (cell_w * cell_h + ww * cell_h));
    if (total + CHARSET_RESERVE * (cell_w * cell_h + ww * cell_h) > FONT_MAX) {
        fprintf(stderr, "error: that would exceed FONT_MAX (%d) in xterm.c\n", FONT_MAX);
        return 1;
    }
    return 0;
}
