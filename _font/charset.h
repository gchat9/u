/*
 * charset.h — the font's glyph slots, and nothing more.
 *
 * The font is a flat array of fixed-size bitmaps; a glyph "slot" is
 * just an index into it (a uint8_t in the framebuffer, so at most 256),
 * with no inherent meaning of its own. This file is the single place
 * that assigns a Unicode codepoint to each slot, in both directions:
 *
 *   charset_codepoint(slot) -> codepoint   used by mkfont.c to decide
 *                                          what to rasterize into slot
 *   charset_slot(codepoint) -> slot        used by the renderer to map
 *                                          a decoded character to a
 *                                          glyph it can actually draw
 *
 * Layout: ASCII (0x20-0x7E) first, as a plain arithmetic offset -- no
 * table, no lookup, just a range check and a subtraction. That's 95
 * slots. Everything else is in charset_extra[], looked up by linear
 * scan since a binary search would be more code for no measurable
 * benefit at this size, and it is only scanned for characters outside
 * ASCII. The table holds, in order:
 *
 *   - a small set of accented Latin letters (Western European, minus
 *     the ones left out on purpose; no other Latin-1 letters), plus the
 *     sharp s and the Æ/Œ ligatures. Œ/œ are not Latin-1 (U+0152/0153),
 *     so there is no Latin-1 range here at all;
 *   - light and double box-drawing (what dialog/whiptail/ncurses TUIs
 *     actually draw), square corners only;
 *   - a few block/shade glyphs, four arrows, and three punctuation/
 *     dingbat marks (check marks, em dash), the degree sign, the French
 *     guillemets, and the horizontal ellipsis.  The big solidus U+29F8
 *     has no glyph of its own: see charset_slot().
 *
 * Double-width glyphs are a second, separate array of the same kind: a
 * "wide slot" indexes charset_wide_extra[] and a wide bitmap in
 * font.bfnt that is two cells wide (see charset_wide_slot() below).
 *
 * The font file only holds CHARSET_COUNT narrow and CHARSET_WIDE_COUNT
 * wide glyphs (mkfont writes exactly that many; the renderer takes the
 * counts from the file header), so every glyph dropped here is also
 * dropped from font.bfnt. Extending the charset later means appending
 * to charset_extra[] or charset_wide_extra[] (order doesn't matter);
 * the _Static_asserts below keep CHARSET_RESERVE slots of each kind
 * free, to be spent on purpose by lowering that number.
 */
#pragma once

#include <stdint.h>

#define CHARSET_ASCII_BASE   0
#define CHARSET_ASCII_LO     0x20
#define CHARSET_ASCII_HI     0x7E
#define CHARSET_ASCII_COUNT  (CHARSET_ASCII_HI - CHARSET_ASCII_LO + 1)     /* 95 */

#define CHARSET_EXTRA_BASE   (CHARSET_ASCII_BASE + CHARSET_ASCII_COUNT)    /* 95 */
#define CHARSET_EXTRA_MAX    (256 - CHARSET_EXTRA_BASE)                    /* 161 */

static const uint16_t charset_extra[] = {
    0x00C0, 0x00E0, 0x00E1, 0x00C2, 0x00E2, 0x00C4, 0x00E4,   /* À à á Â â Ä ä */
    0x00C7, 0x00E7,                                           /* Ç ç */
    0x00C9, 0x00E9, 0x00C8, 0x00E8, 0x00CA, 0x00EA, 0x00CB, 0x00EB, /* É é È è Ê ê Ë ë */
    0x00CC, 0x00EC, 0x00CE, 0x00EE, 0x00CF, 0x00EF,           /* Ì ì Î î Ï ï */
    0x00D3, 0x00F3, 0x00D2, 0x00F2, 0x00D4, 0x00F4, 0x00D6, 0x00F6, /* Ó ó Ò ò Ô ô Ö ö */
    0x00F1,                                                   /* ñ */
    0x00D9, 0x00F9, 0x00DB, 0x00FB, 0x00DC, 0x00FC,           /* Ù ù Û û Ü ü */
    0x00FF,                                                   /* ÿ */
    0x00DF,                                                   /* ß */
    0x00C6, 0x00E6,                                           /* Æ æ */
    0x0152, 0x0153,                                           /* Œ œ */
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
    0x252C, 0x2534, 0x253C,                            /* light box drawing */
    0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2560, 0x2563,
    0x2566, 0x2569, 0x256C,                            /* double box drawing */
    0x2580, 0x2584, 0x2588, 0x2591, 0x2592, 0x2593,    /* block / shade */
    0x25B2, 0x25BA, 0x25BC, 0x25C4,                    /* arrows: up right down left */
    0x2714, 0x2713, 0x2014,                            /* ✔ ✓ — */
    0x00B0,                                            /* ° */
    0x00AB, 0x00BB,                                    /* « » */
    0x2026,                                            /* … */
};
#define CHARSET_EXTRA_COUNT ((int)(sizeof(charset_extra) / sizeof(charset_extra[0])))

/* Number of glyphs actually baked into font.bfnt (slots 0..COUNT-1). */
#define CHARSET_COUNT (CHARSET_EXTRA_BASE + CHARSET_EXTRA_COUNT)

/* Slots deliberately kept free in each of the two glyph arrays, for
 * future additions. Both are indexed by a uint8_t, so 256 is the hard
 * limit of each. */
#define CHARSET_RESERVE 50

_Static_assert(CHARSET_EXTRA_COUNT <= CHARSET_EXTRA_MAX,
              "charset_extra[] overflows the font's 256 glyph slots -- trim it");
_Static_assert(CHARSET_COUNT + CHARSET_RESERVE <= 256,
              "charset_extra[] has eaten into the reserved narrow glyph slots");

/* slot -> codepoint. Returns 0 for a slot past the end of the charset. */
static inline uint32_t charset_codepoint(int slot)
{
    if (slot >= CHARSET_ASCII_BASE && slot < CHARSET_EXTRA_BASE)
        return (uint32_t)(CHARSET_ASCII_LO + (slot - CHARSET_ASCII_BASE));
    if (slot >= CHARSET_EXTRA_BASE && slot < CHARSET_COUNT)
        return charset_extra[slot - CHARSET_EXTRA_BASE];
    return 0;
}

/* codepoint -> slot. Returns -1 if this charset has no glyph for it
 * (the caller marks the cell XTERM_ATTR_MISSING; see x11_backend.c). */
static inline int charset_slot(uint32_t cp)
{
    /* Hyphen (U+2010) and non-breaking hyphen (U+2011) are drawn with the
     * ASCII hyphen-minus glyph: no slot of their own (man(1) breaks words
     * with U+2010). Copy and paste still see the original character.
     * The big solidus (U+29F8) is likewise drawn with the ASCII slash. */
    if (cp == 0x2010 || cp == 0x2011) cp = '-';
    if (cp == 0x29F8) cp = '/';
    if (cp >= CHARSET_ASCII_LO && cp <= CHARSET_ASCII_HI)
        return CHARSET_ASCII_BASE + (int)(cp - CHARSET_ASCII_LO);
    for (int i = 0; i < CHARSET_EXTRA_COUNT; i++)
        if (charset_extra[i] == cp) return CHARSET_EXTRA_BASE + i;
    return -1;
}

/* Fullwidth forms, U+FF01..U+FF60: CJK-era duplicates of ASCII, all
 * double-width. Programs (readline, ncurses, ...) size lines by that, so
 * every one of them must take two cells whether or not we can draw it. */
static inline int charset_fullwidth(uint32_t cp)
{
    return cp >= 0xFF01 && cp <= 0xFF60;
}

/* What we can draw of them: U+FF01..U+FF5E are exactly ASCII 0x21..0x7E
 * at a fixed offset of 0xFEE0, so a fullwidth punctuation mark is drawn
 * with its ASCII twin's existing glyph, centred in the two-cell area --
 * no new font data at all. Returns that ASCII code, or 0 for anything
 * not drawn this way: the fullwidth letters and digits (which draw as a
 * placeholder box) and U+FF5F/FF60. */
static inline uint32_t charset_fullwidth_alias(uint32_t cp)
{
    if (cp < 0xFF01 || cp > 0xFF5E) return 0;
    uint32_t a = cp - 0xFEE0, l = a | 0x20;
    return (a >= '0' && a <= '9') || (l >= 'a' && l <= 'z') ? 0 : a;
}

/* ── Double-width glyphs ────────────────────────────────────────────
 *
 * Characters drawn from their own two-cell bitmap, rather than by
 * centring a narrow twin (above) or as an emoji. They live in a second
 * array of glyphs, with its own slot numbering (0..CHARSET_WIDE_COUNT-1),
 * stored after the narrow glyphs in font.bfnt; a cell holding one is
 * flagged XTERM_ATTR_WIDE (see xterm.h). Add to the end, as above.
 */
static const uint16_t charset_wide_extra[] = {
    0x30C4,                                            /* ツ */
};
#define CHARSET_WIDE_COUNT ((int)(sizeof(charset_wide_extra) / sizeof(charset_wide_extra[0])))

_Static_assert(CHARSET_WIDE_COUNT + CHARSET_RESERVE <= 256,
              "charset_wide_extra[] has eaten into the reserved wide glyph slots");

/* wide slot -> codepoint (0 past the end), for mkfont.c */
static inline uint32_t charset_wide_codepoint(int slot)
{
    return slot >= 0 && slot < CHARSET_WIDE_COUNT ? charset_wide_extra[slot] : 0;
}

/* codepoint -> wide slot, or -1 if we have no two-cell bitmap for it */
static inline int charset_wide_slot(uint32_t cp)
{
    for (int i = 0; i < CHARSET_WIDE_COUNT; i++)
        if (charset_wide_extra[i] == cp) return i;
    return -1;
}

/* CJK and other East Asian wide characters outside the emoji blocks, as
 * [lo,hi] runs; generated by gen_wide_ranges.py (`./gen_wide_ranges.py
 * cjk`, second table of its output). Like libc's wcwidth() it takes the
 * CJK blocks whole -- unassigned and "ambiguous" code points included --
 * but not combining marks (width 0), so a run counts as wide whether or
 * not we can draw a given character in it, and cell width agrees with
 * wcwidth(). Includes the fullwidth signs at U+FFE0..FFE6; the fullwidth
 * forms U+FF01..FF60 are charset_fullwidth(). */
static const uint32_t charset_cjk_ranges[][2] = {
    {0x1100,0x115F}, {0x2329,0x232A}, {0x2E80,0x3029}, {0x302E,0x303E}, {0x3041,0x3098},
    {0x309B,0xA4CF}, {0xA960,0xA97F}, {0xAC00,0xD7A3}, {0xF900,0xFAFF}, {0xFE10,0xFE19},
    {0xFE30,0xFE6F}, {0xFFE0,0xFFE6}, {0x16FE0,0x1B2FB}, {0x20000,0x3FFFD},
};
#define CHARSET_CJK_RANGES ((int)(sizeof(charset_cjk_ranges) / sizeof(charset_cjk_ranges[0])))

/* Is cp double-width because it is a CJK / fullwidth character? (Emoji
 * are emoji_wide(), in emoji_charset.h.) The >= 0x1100 test keeps the
 * scan off the hot path for ordinary text. */
static inline int charset_wide(uint32_t cp)
{
    if (cp < 0x1100) return 0;
    if (charset_fullwidth(cp)) return 1;
    for (int i = 0; i < CHARSET_CJK_RANGES; i++)
        if (cp >= charset_cjk_ranges[i][0] && cp <= charset_cjk_ranges[i][1]) return 1;
    return 0;
}
