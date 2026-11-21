/*
 * charset.h — the font's glyph set, as mkfont.c bakes it in: the one place
 * that says which codepoint each glyph of a font is.
 *
 * This is build input. Each font file (font<STEP>.bfnt) records, in its own
 * tables, the codepoint of every glyph in it (see the layout in mkfont.c), and
 * xterm reads that, so what is drawn does not depend on this file: the glyph
 * set can change by rebuilding the fonts, with no program rebuilt.
 *
 * The one program-side use is vt.c's small cells (VT_CELL_COMPACT): a grid
 * whose characters are all in this file's tables (and emoji_charset.h's) is
 * stored at 2 bytes a cell, one with any other character at 8.  That only
 * decides how much memory a window takes, never what it shows, so a font and
 * a program built from different versions of this file still agree on every
 * pixel; but a window that prints characters the program's copy lacks costs
 * more, so build the programs and the fonts with the same options (below),
 * and put in this file what programs actually print (the glyphs are cheap, a
 * window that goes full-size is not).
 *
 * The narrow glyphs are slots 0..CHARSET_COUNT-1 of the font: the printable
 * ASCII, U+0020..U+007E, in slots 0..94 (the ASCII range below, a plain
 * offset and no table: the programs rely on this), then the codepoints of
 * charset_extra[], in order, in slots 95 on.  A font holds up to 511 narrow
 * glyphs, slots 0..510: slot 511 is not one (see vt.c, which uses it to mark a
 * double-width character in its small cells).  The table holds, in order:
 *
 *   - a small set of accented Latin letters (Western European, minus
 *     the ones left out on purpose; no other Latin-1 letters), plus the
 *     sharp s and the Æ/Œ ligatures. Œ/œ are not Latin-1 (U+0152/0153),
 *     so there is no Latin-1 range here at all;
 *   - light and double box-drawing (what dialog/whiptail/ncurses TUIs
 *     actually draw), square corners only;
 *   - a few block/shade glyphs, four arrows, and three punctuation/
 *     dingbat marks (check marks, em dash), the degree sign, the French
 *     guillemets, and the horizontal ellipsis;
 *   - what command-line programs print besides (typographic quotes, bullets,
 *     arrows, triangles, circles, crosses, the no-break space and a few signs).
 *
 * Which character is drawn with which glyph, where one character is drawn with
 * another's glyph (the hyphens, the big solidus, the fullwidth forms), is the
 * renderer's and lives with it (xterm_screen.c). Whether a character is
 * double-width is not this file's either (width.h).
 *
 * Double-width glyphs are a second array of the same kind: charset_wide_extra[]
 * holds the codepoints of the wide glyphs, which mkfont bakes as two-cell
 * bitmaps (wide slot i is the i-th entry), after the narrow ones; there is room
 * for 256.  The colour emoji of emoji_charset.h (255 at most) are numbered after
 * them, 256 on, by vt.c.
 *
 * The font file holds CHARSET_COUNT narrow and CHARSET_WIDE_COUNT wide glyphs,
 * so every glyph dropped here is also dropped from the fonts. Extending the
 * charset later means appending to charset_extra[] or charset_wide_extra[]
 * (order doesn't matter); the _Static_asserts below say when a table is full.
 */
#pragma once

#include <stdint.h>

#define CHARSET_ASCII_BASE   0
#define CHARSET_ASCII_LO     0x20
#define CHARSET_ASCII_HI     0x7E
#define CHARSET_ASCII_COUNT  (CHARSET_ASCII_HI - CHARSET_ASCII_LO + 1)     /* 95 */

#define CHARSET_EXTRA_BASE   (CHARSET_ASCII_BASE + CHARSET_ASCII_COUNT)    /* 95 */
#define CHARSET_EXTRA_MAX    (511 - CHARSET_EXTRA_BASE)                    /* 416 */

/* xterm_glyph_slot() in _lib/xterm.c relies on the ASCII range being exactly
 * slots 0..94, U+0020..U+007E; this keeps that true if anyone edits it. */
_Static_assert(CHARSET_ASCII_BASE == 0 && CHARSET_ASCII_LO == 0x20 &&
               CHARSET_ASCII_HI == 0x7E && CHARSET_ASCII_COUNT == 95,
              "the ASCII convention is part of the font format (xterm.c)");


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
    /* what current command-line programs print and a font without it draws as
     * a box: a no-break space (tree, ls -l), typographic quotes and the bullet
     * (man, git, package managers), the four arrows, the open triangles
     * (htop, vim, ncdu), circles, crosses, and the middle dot, multiplication
     * and minus signs */
    0x00A0, 0x00B7, 0x00D7, 0x2212,                    /*   · × −  */
    0x2018, 0x2019, 0x201C, 0x201D, 0x2022,            /* ‘ ’ “ ” •  */
    0x2190, 0x2191, 0x2192, 0x2193,                    /* ← ↑ → ↓  */
    0x25B3, 0x25B7, 0x25BD, 0x25C1, 0x25CB, 0x25CF,    /* △ ▷ ▽ ◁ ○ ●  */
    0x2717, 0x2718,                                    /* ✗ ✘ */
};
#define CHARSET_EXTRA_COUNT ((int)(sizeof(charset_extra) / sizeof(charset_extra[0])))

/* Number of glyphs actually baked into font<STEP>.bfnt (slots 0..COUNT-1). */
#define CHARSET_COUNT (CHARSET_EXTRA_BASE + CHARSET_EXTRA_COUNT)

/* A font holds slots 0..510 of narrow glyphs: see the top. */
_Static_assert(CHARSET_EXTRA_COUNT <= CHARSET_EXTRA_MAX,
              "charset_extra[] overflows the font's 511 narrow glyph slots -- trim it");

/* slot -> codepoint. Returns 0 for a slot past the end of the charset. */
static inline uint32_t charset_codepoint(int slot)
{
    if (slot >= CHARSET_ASCII_BASE && slot < CHARSET_EXTRA_BASE)
        return (uint32_t)(CHARSET_ASCII_LO + (slot - CHARSET_ASCII_BASE));
    if (slot >= CHARSET_EXTRA_BASE && slot < CHARSET_COUNT)
        return charset_extra[slot - CHARSET_EXTRA_BASE];
    return 0;
}

/* ── Double-width glyphs ────────────────────────────────────────────
 *
 * Characters drawn from their own two-cell bitmap, rather than by
 * centring a narrow twin or as an emoji. They have their own slot
 * numbering (0..CHARSET_WIDE_COUNT-1), stored after the narrow glyphs
 * in font<STEP>.bfnt. Add to the end, as above.
 */
static const uint16_t charset_wide_extra[] = {
    0x30C4,                                            /* ツ */
};
#define CHARSET_WIDE_COUNT ((int)(sizeof(charset_wide_extra) / sizeof(charset_wide_extra[0])))

_Static_assert(CHARSET_WIDE_COUNT <= 256,
              "charset_wide_extra[] overflows the font's 256 wide glyph slots -- trim it");

/* wide slot -> codepoint (0 past the end), for mkfont.c */
static inline uint32_t charset_wide_codepoint(int slot)
{
    return slot >= 0 && slot < CHARSET_WIDE_COUNT ? charset_wide_extra[slot] : 0;
}
