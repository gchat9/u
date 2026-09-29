/*
 * mkemoji.c — builds emoji.bfnt from Twemoji PNG source art.
 *
 * Run after mkfont.c's ./mkfont (needs font.bfnt to already exist, to
 * read its cell_w/cell_h from -- see below). Expects emoji_src/<hex
 * codepoint>.png for every entry in emoji_charset.h; the exact files
 * used were fetched from https://github.com/jdecked/twemoji
 * (assets/72x72/<codepoint>.png), which is Twemoji artwork, CC-BY 4.0.
 *
 * For each emoji: box-filter downsample from the 72x72 source to
 * 2*cell_w x cell_h (alpha-weighted, so edges blend correctly against
 * transparency instead of picking up dark fringing), threshold alpha
 * to decide transparent vs opaque per pixel, and quantize the opaque
 * pixels to a 15-colour palette via median-cut. Twemoji's art is flat-
 * shaded with few distinct colours per glyph already, so 15 is enough
 * to stay visually close to lossless for this set.
 *
 * Output format (emoji.bfnt):
 *   12-byte header: magic "EMOJ", cell_w, cell_h, count, reserved
 *     (all u16 except the 4-byte magic)
 *   then, per emoji, in emoji_charset[] order (nothing per-record
 *   labels which emoji it is -- exactly like font.bfnt's glyph slots,
 *   the shared header is the only mapping):
 *     45 bytes: 15 RGB triples (palette indices 1-15; index 0 is
 *               always "transparent" and needs no storage)
 *     ceil(2*cell_w*cell_h/2) bytes: packed 4-bit palette indices,
 *               row-major, two pixels per byte (first pixel in the
 *               high nibble)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "emoji_charset.h"

#define NCOLORS         15
#define ALPHA_THRESHOLD 128   /* >= this (of 255) counts as opaque */

typedef struct { uint8_t r, g, b; } RGB8;

/* ---- box filter downsample, alpha-weighted so edges blend against
 * transparency rather than the (invisible) background colour ---- */
static void downsample(const uint8_t *src, int sw, int sh,
                       uint8_t *dst_rgb, uint8_t *dst_a, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy0 = y * sh / dh, sy1 = (y + 1) * sh / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * sw / dw, sx1 = (x + 1) * sw / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            double rs = 0, gs = 0, bs = 0, as = 0; int n = 0;
            for (int sy = sy0; sy < sy1 && sy < sh; sy++) {
                for (int sx = sx0; sx < sx1 && sx < sw; sx++) {
                    const uint8_t *p = src + (size_t)(sy * sw + sx) * 4;
                    double a = p[3] / 255.0;
                    rs += p[0] * a; gs += p[1] * a; bs += p[2] * a; as += a;
                    n++;
                }
            }
            int idx = y * dw + x;
            dst_a[idx] = (uint8_t)(n ? (as / n) * 255.0 : 0);
            if (as > 0.001) {
                dst_rgb[idx*3+0] = (uint8_t)(rs / as);
                dst_rgb[idx*3+1] = (uint8_t)(gs / as);
                dst_rgb[idx*3+2] = (uint8_t)(bs / as);
            } else {
                dst_rgb[idx*3+0] = dst_rgb[idx*3+1] = dst_rgb[idx*3+2] = 0;
            }
        }
    }
}

/* ---- median-cut quantizer: splits the box with the largest channel
 * range, repeatedly, until NCOLORS boxes exist (or fewer, if there
 * simply aren't that many distinct colours) ---- */
typedef struct { int lo, hi; } Box;  /* [lo,hi) range into a shared, freely-reordered pixel array */

static int sort_channel;
static int cmp_rgb(const void *a, const void *b)
{
    const RGB8 *pa = a, *pb = b;
    int va = sort_channel == 0 ? pa->r : sort_channel == 1 ? pa->g : pa->b;
    int vb = sort_channel == 0 ? pb->r : sort_channel == 1 ? pb->g : pb->b;
    return va - vb;
}

static int channel_range(const RGB8 *px, int lo, int hi, int ch)
{
    int mn = 255, mx = 0;
    for (int i = lo; i < hi; i++) {
        int v = ch == 0 ? px[i].r : ch == 1 ? px[i].g : px[i].b;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    return mx - mn;
}

static int quantize(RGB8 *pixels, int n, RGB8 out_palette[NCOLORS])
{
    Box boxes[NCOLORS];
    int nboxes = 1;
    boxes[0].lo = 0; boxes[0].hi = n;

    while (nboxes < NCOLORS) {
        int best = -1, bestrange = 0, bestch = 0;
        for (int i = 0; i < nboxes; i++) {
            if (boxes[i].hi - boxes[i].lo < 2) continue;
            for (int ch = 0; ch < 3; ch++) {
                int r = channel_range(pixels, boxes[i].lo, boxes[i].hi, ch);
                if (r > bestrange) { bestrange = r; best = i; bestch = ch; }
            }
        }
        if (best < 0) break;  /* nothing left worth splitting */
        sort_channel = bestch;
        qsort(&pixels[boxes[best].lo],
              (size_t)(boxes[best].hi - boxes[best].lo), sizeof(RGB8), cmp_rgb);
        int mid = (boxes[best].lo + boxes[best].hi) / 2;
        boxes[nboxes].lo = mid;
        boxes[nboxes].hi = boxes[best].hi;
        boxes[best].hi = mid;
        nboxes++;
    }

    for (int i = 0; i < nboxes; i++) {
        long sr = 0, sg = 0, sb = 0;
        int cnt = boxes[i].hi - boxes[i].lo;
        for (int j = boxes[i].lo; j < boxes[i].hi; j++)
            { sr += pixels[j].r; sg += pixels[j].g; sb += pixels[j].b; }
        out_palette[i].r = (uint8_t)(sr / cnt);
        out_palette[i].g = (uint8_t)(sg / cnt);
        out_palette[i].b = (uint8_t)(sb / cnt);
    }
    return nboxes;
}

static int nearest(RGB8 c, const RGB8 *pal, int npal)
{
    int best = 0, bestd = INT_MAX;
    for (int i = 0; i < npal; i++) {
        int dr = c.r - pal[i].r, dg = c.g - pal[i].g, db = c.b - pal[i].b;
        int d = dr*dr + dg*dg + db*db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

int main(void)
{
    /* Read cell_w/cell_h from font.bfnt's own header rather than
     * hardcoding them here -- they're computed from the TTF at mkfont
     * build time, not fixed constants, so this is the only way
     * emoji.bfnt is guaranteed to match whatever font was actually
     * built, with nothing to keep manually in sync. */
    FILE *fin = fopen("font.bfnt", "rb");
    if (!fin) { fprintf(stderr, "font.bfnt not found -- run ./mkfont first\n"); return 1; }
    uint8_t fhdr[16];
    if (fread(fhdr, 1, sizeof fhdr, fin) != sizeof fhdr) { fprintf(stderr, "font.bfnt too short\n"); return 1; }
    fclose(fin);
    uint16_t cell_w, cell_h;
    memcpy(&cell_w, fhdr + 4, 2);
    memcpy(&cell_h, fhdr + 6, 2);
    int gw = cell_w * 2, gh = cell_h;

    FILE *out = fopen("emoji.bfnt", "wb");
    if (!out) { perror("emoji.bfnt"); return 1; }

    uint8_t hdr[12] = { 'E','M','O','J' };
    uint16_t count = (uint16_t)EMOJI_COUNT, rsv = 0;
    memcpy(hdr + 4, &cell_w, 2);
    memcpy(hdr + 6, &cell_h, 2);
    memcpy(hdr + 8, &count, 2);
    memcpy(hdr + 10, &rsv, 2);
    fwrite(hdr, 1, sizeof hdr, out);

    uint8_t *rgb    = malloc((size_t)gw * gh * 3);
    uint8_t *alpha  = malloc((size_t)gw * gh);
    RGB8    *opaque = malloc(sizeof(RGB8) * (size_t)gw * gh);
    uint8_t *packed = malloc(((size_t)gw * gh + 1) / 2);

    for (int i = 0; i < EMOJI_COUNT; i++) {
        if (!emoji_wide(emoji_charset[i])) {   /* would be drawn one cell wide by vt.c */
            fprintf(stderr, "U+%X is in emoji_charset[] but not emoji_wide_ranges[]\n", emoji_charset[i]);
            return 1;
        }
        char path[64];
        snprintf(path, sizeof path, "emoji_src/%x.png", emoji_charset[i]);
        int sw, sh, comp;
        uint8_t *src = stbi_load(path, &sw, &sh, &comp, 4);
        if (!src) { fprintf(stderr, "failed to load %s\n", path); return 1; }

        downsample(src, sw, sh, rgb, alpha, gw, gh);
        stbi_image_free(src);

        int nopaque = 0;
        for (int p = 0; p < gw * gh; p++) {
            if (alpha[p] >= ALPHA_THRESHOLD) {
                opaque[nopaque].r = rgb[p*3+0];
                opaque[nopaque].g = rgb[p*3+1];
                opaque[nopaque].b = rgb[p*3+2];
                nopaque++;
            }
        }

        RGB8 palette[NCOLORS] = {{0,0,0}};
        int ncolors = nopaque > 0 ? quantize(opaque, nopaque, palette) : 0;

        uint8_t palbuf[NCOLORS * 3] = {0};
        for (int c = 0; c < ncolors; c++) {
            palbuf[c*3+0] = palette[c].r;
            palbuf[c*3+1] = palette[c].g;
            palbuf[c*3+2] = palette[c].b;
        }
        fwrite(palbuf, 1, sizeof palbuf, out);

        memset(packed, 0, ((size_t)gw * gh + 1) / 2);
        for (int p = 0; p < gw * gh; p++) {
            int index = 0;  /* transparent */
            if (ncolors > 0 && alpha[p] >= ALPHA_THRESHOLD) {
                RGB8 c = { rgb[p*3+0], rgb[p*3+1], rgb[p*3+2] };
                index = 1 + nearest(c, palette, ncolors);
            }
            int byte = p / 2;
            if (p % 2 == 0) packed[byte] = (uint8_t)(index << 4);
            else            packed[byte] |= (uint8_t)(index & 0x0F);
        }
        fwrite(packed, 1, ((size_t)gw * gh + 1) / 2, out);

        fprintf(stderr, "emoji[%2d] cp=%05x colours=%2d\n", i, emoji_charset[i], ncolors);
    }

    free(rgb); free(alpha); free(opaque); free(packed);
    fclose(out);
    fprintf(stderr, "wrote emoji.bfnt (%d emoji, %dx%d each)\n", EMOJI_COUNT, gw, gh);
    return 0;
}
