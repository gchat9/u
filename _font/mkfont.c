/*
 * mkfont.c — bake a monospace TTF into font<STEP>.bfnt, one glyph per slot of
 * the layout defined in charset.h.
 *
 * Normally run via this directory's Makefile (which also fetches
 * stb_truetype.h). By hand:
 *   cc -O2 -o mkfont mkfont.c -lm && ./mkfont STEP OUT.bfnt [narrow.ttf [wide.ttf]]
 * STEP (0..FONT_SIZES-1) picks the size: the cell is 2^((STEP-FONT_BASE)/4) times
 * the size the font was always built at (FONT_PX below), in both directions. So
 * STEP FONT_BASE is that size, STEP FONT_BASE+4 is twice it, and xterm starts at
 * FONT_BASE.
 * The narrow TTF defaults to DejaVu Sans Mono; any monospace font works.
 * The wide TTF supplies the double-width glyphs (charset_wide_extra[]):
 * it defaults to Noto Sans CJK; any font with those characters works
 * (a .ttc is read at index 0).
 *
 * font<STEP>.bfnt layout, all little-endian:
 *   "BFN3", cell_w, cell_h, baseline, num_glyphs, num_wide, num_emoji
 *                                                        (16 bytes)
 *   codepoints of narrow glyphs CHARSET_ASCII_COUNT..num_glyphs-1: u32 each
 *                (the glyphs before those are U+0020..U+007E in order: the
 *                 ASCII range has no table, see charset.h)
 *   codepoints of the wide glyphs: num_wide x u32
 *   num_glyphs   narrow glyphs, cell_w x cell_h bytes of alpha each
 *   num_wide     wide glyphs, (2*cell_w) x cell_h bytes of alpha each
 *   the emoji section, appended by mkemoji (see mkemoji.c); num_emoji is 0
 *   in what mkfont writes:
 *     num_emoji x u32   the codepoint of each emoji
 *     num_emoji records of 45 + cell_w*cell_h bytes each (mkemoji.c)
 * The header is the only record of the size: the cell dimensions are in it,
 * not in the file name. The codepoint tables are what lets a program find a
 * glyph in a font without including charset.h (see xterm.c, xterm_glyph_slot).
 * The file is complete once mkemoji has run on it.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"

#define DEFAULT_TTF  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define DEFAULT_WTTF "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
#define FONT_PX     26
#define FONT_SIZES  8                    /* STEP 0..7; keep equal to xterm.c's */
#define FONT_BASE   2                    /* the size FONT_PX high, as always built */
#define NUM_GLYPHS  CHARSET_COUNT        /* only what charset.h defines */
#define NUM_WIDE    CHARSET_WIDE_COUNT

static void w16(FILE *f, uint16_t v) {          /* write LE uint16 */
    uint8_t b[2] = { v & 0xFF, v >> 8 };
    fwrite(b, 1, 2, f);
}

static void w32(FILE *f, uint32_t v) {          /* write LE uint32 */
    uint8_t b[4] = { v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24 };
    fwrite(b, 1, 4, f);
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
    /* ── arguments ────────────────────────────────────────────────── */
    if (argc < 3) {
        fprintf(stderr, "usage: mkfont STEP OUT.bfnt [narrow.ttf [wide.ttf]]\n");
        return 2;
    }
    int step = atoi(argv[1]);
    if (step < 0 || step >= FONT_SIZES) {
        fprintf(stderr, "mkfont: STEP must be 0..%d\n", FONT_SIZES - 1);
        return 2;
    }
    const char *out_path  = argv[2];
    const char *ttf_path  = argc > 3 ? argv[3] : DEFAULT_TTF;
    const char *wttf_path = argc > 4 ? argv[4] : DEFAULT_WTTF;

    /* ── load TTF ─────────────────────────────────────────────────── */
    uint8_t *ttf = load_file(ttf_path);

    stbtt_fontinfo fi;
    stbtt_InitFont(&fi, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));

    /* The base size's scale and metrics: FONT_PX high, as the font was always built. */
    float base_scale = stbtt_ScaleForPixelHeight(&fi, FONT_PX);

    int asc, desc, lgap;
    stbtt_GetFontVMetrics(&fi, &asc, &desc, &lgap);

    /* For a monospace font every advance is identical; use 'M'. */
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&fi, 'M', &adv, &lsb);
    uint16_t cell_w0 = (uint16_t)(adv * base_scale + 0.5f);

    /* This size: 2^((STEP-FONT_BASE)/4) times the base cell in both directions.
     * The width is scaled from its rounded base value, not from the advance:
     * the advance at twice the size would round to 27 px, not twice 13. */
    float f     = powf(2.0f, (step - FONT_BASE) / 4.0f);
    float scale = base_scale * f;
    uint16_t cell_w   = (uint16_t)(cell_w0 * f + 0.5f);
    uint16_t cell_h   = (uint16_t)((asc - desc) * scale + 0.5f);
    uint16_t baseline = (uint16_t)(asc * scale + 0.5f);

    fprintf(stderr, "step %d: cell %u×%u  baseline %u  scale %.4f\n",
            step, cell_w, cell_h, baseline, (double)scale);

    /* ── write header ─────────────────────────────────────────────── */
    FILE *out = fopen(out_path, "wb");
    if (!out) { perror(out_path); return 1; }

    fwrite("BFN3", 1, 4, out);
    w16(out, cell_w);
    w16(out, cell_h);
    w16(out, baseline);
    w16(out, NUM_GLYPHS);
    w16(out, NUM_WIDE);
    w16(out, 0);              /* num_emoji: mkemoji fills it in and appends them */

    /* ── codepoint tables (see the layout above) ───────────────────── */
    for (int slot = CHARSET_ASCII_COUNT; slot < NUM_GLYPHS; slot++)
        w32(out, charset_codepoint(slot));
    for (int slot = 0; slot < NUM_WIDE; slot++)
        w32(out, charset_wide_codepoint(slot));

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

    unsigned total = 16 + 4 * (NUM_GLYPHS - CHARSET_ASCII_COUNT) + 4 * NUM_WIDE
                   + NUM_GLYPHS * cell_w * cell_h + NUM_WIDE * ww * cell_h;
    fprintf(stderr, "wrote %s  (%d narrow + %d wide glyphs, %u bytes)\n",
            out_path, NUM_GLYPHS, NUM_WIDE, total);
    return 0;
}
