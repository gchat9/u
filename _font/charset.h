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
 * The narrow glyphs are slots 0..CHARSET_COUNT-1 of the font.  The first two
 * ranges have no table, a plain offset (the programs rely on this): the
 * printable ASCII, U+0020..U+007E, in slots 0..94, and all of Latin-1,
 * U+00A0..U+00FF (the no-break space to ÿ), in slots 95..190.  U+0080..U+009F
 * are controls and have no glyph.  Then come the codepoints of charset_extra[],
 * in order, in slots 191 on.  A font holds up to 511 narrow glyphs, slots
 * 0..510: slot 511 is not one (see vt.c, which uses it to mark a double-width
 * character in its small cells).  The table holds, in order:
 *
 *   - the ligatures Œ/œ (U+0152/0153), which are not Latin-1;
 *   - light and double box-drawing (what dialog/whiptail/ncurses TUIs
 *     actually draw), square corners only;
 *   - a few block/shade glyphs, four arrows, three punctuation/dingbat marks
 *     (check marks, em dash) and the horizontal ellipsis;
 *   - what command-line programs print besides (typographic quotes, bullets,
 *     arrows, triangles, circles, crosses and the minus sign);
 *   - and then, each only if its option is set in xterm_conf.h (the fonts and
 *     the programs both read it; that is what makes the keyboard layouts of
 *     those options work):
 *       XTERM_GREEK     the unaccented Greek letters (49)
 *       XTERM_LATIN2    the letters of Latin-2 and the Turkish and Romanian
 *                       ones (65; the Latin-1 letters of those languages
 *                       are in the range above)
 *       XTERM_CYRILLIC  the Cyrillic letters of Russian, Ukrainian,
 *                       Belarusian, Bulgarian, Serbian and Macedonian, and
 *                       the numero sign (95)
 *     They are what the keyboard code of those options types, and nothing
 *     more: _font/check_charset.py checks that every character a key can
 *     type with an option is in the font built with it.  They come last, so
 *     that every other glyph has the same slot with and without them.
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

#include "../_lib/xterm_conf.h"

#define CHARSET_ASCII_BASE   0
#define CHARSET_ASCII_LO     0x20
#define CHARSET_ASCII_HI     0x7E
#define CHARSET_ASCII_COUNT  (CHARSET_ASCII_HI - CHARSET_ASCII_LO + 1)     /* 95 */

#define CHARSET_LATIN1_BASE  (CHARSET_ASCII_BASE + CHARSET_ASCII_COUNT)   /* 95 */
#define CHARSET_LATIN1_LO    0xA0
#define CHARSET_LATIN1_HI    0xFF
#define CHARSET_LATIN1_COUNT (CHARSET_LATIN1_HI - CHARSET_LATIN1_LO + 1)  /* 96 */

#define CHARSET_EXTRA_BASE   (CHARSET_LATIN1_BASE + CHARSET_LATIN1_COUNT) /* 191 */
#define CHARSET_EXTRA_MAX    (511 - CHARSET_EXTRA_BASE)                   /* 320 */

/* xterm_glyph_slot() in _lib/xterm.c and vt.c's glyph_of() rely on these two
 * ranges being exactly slots 0..94 and 95..190, with no table; this keeps that
 * true if anyone edits it. */
_Static_assert(CHARSET_ASCII_BASE == 0 && CHARSET_ASCII_LO == 0x20 &&
               CHARSET_ASCII_HI == 0x7E && CHARSET_ASCII_COUNT == 95 &&
               CHARSET_LATIN1_BASE == 95 && CHARSET_LATIN1_LO == 0xA0 &&
               CHARSET_LATIN1_HI == 0xFF && CHARSET_LATIN1_COUNT == 96 &&
               CHARSET_EXTRA_BASE == 191,
              "the ASCII and Latin-1 convention is part of the font format (xterm.c)");


static const uint16_t charset_extra[] = {
    0x0152, 0x0153,                                           /* Œ œ */
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
    0x252C, 0x2534, 0x253C,                            /* light box drawing */
    0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2560, 0x2563,
    0x2566, 0x2569, 0x256C,                            /* double box drawing */
    0x2580, 0x2584, 0x2588, 0x2591, 0x2592, 0x2593,    /* block / shade */
    0x25B2, 0x25BA, 0x25BC, 0x25C4,                    /* arrows: up right down left */
    0x2714, 0x2713, 0x2014,                            /* ✔ ✓ — */
    0x2026,                                            /* … */
    /* what current command-line programs print and a font without it draws as
     * a box: typographic quotes and the bullet
     * (man, git, package managers), the four arrows, the open triangles
     * (htop, vim, ncdu), circles, crosses and the minus sign */
    0x2212,                                            /* − */
    0x2018, 0x2019, 0x201C, 0x201D, 0x2022,            /* ‘ ’ “ ” •  */
    0x2190, 0x2191, 0x2192, 0x2193,                    /* ← ↑ → ↓  */
    0x25B3, 0x25B7, 0x25BD, 0x25C1, 0x25CB, 0x25CF,    /* △ ▷ ▽ ◁ ○ ●  */
    0x2717, 0x2718,                                    /* ✗ ✘ */
#if XTERM_GREEK
    /* Α..Ρ Σ..Ω and α..ω with the final sigma: the unaccented Greek letters */
    0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397, 0x0398,
    0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F, 0x03A0,
    0x03A1, 0x03A3, 0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9,
    0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8,
    0x03B9, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0,
    0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7, 0x03C8,
    0x03C9,
#endif
#if XTERM_LATIN2
    /* the letters of the Latin-2 keysyms (and the spacing breve, ogonek, caron,
     * double acute and dot above), the Turkish letters of Latin-3 (Ğ ğ İ ı) and
     * the Romanian ones with the comma below (Ș ș Ț ț).  The Latin-1 letters
     * that Czech, Slovak and Hungarian use (Á Í Ú Ý í ú ý) are in the Latin-1
     * range above */
    0x0102, 0x0103, 0x0104, 0x0105, 0x0106, 0x0107, 0x010C, 0x010D,
    0x010E, 0x010F, 0x0110, 0x0111, 0x0118, 0x0119, 0x011A, 0x011B,
    0x011E, 0x011F, 0x0130, 0x0131, 0x0139, 0x013A, 0x013D, 0x013E,
    0x0141, 0x0142, 0x0143, 0x0144, 0x0147, 0x0148, 0x0150, 0x0151,
    0x0154, 0x0155, 0x0158, 0x0159, 0x015A, 0x015B, 0x015E, 0x015F,
    0x0160, 0x0161, 0x0162, 0x0163, 0x0164, 0x0165, 0x016E, 0x016F,
    0x0170, 0x0171, 0x0179, 0x017A, 0x017B, 0x017C, 0x017D, 0x017E,
    0x0218, 0x0219, 0x021A, 0x021B, 0x02C7, 0x02D8, 0x02D9, 0x02DB,
    0x02DD,
#endif
#if XTERM_CYRILLIC
    /* Ё..я and ё..џ, Ґ ґ and №: the Cyrillic keysyms */
    0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408,
    0x0409, 0x040A, 0x040B, 0x040C, 0x040E, 0x040F, 0x0410, 0x0411,
    0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419,
    0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421,
    0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429,
    0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430, 0x0431,
    0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439,
    0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441,
    0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449,
    0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F, 0x0451, 0x0452,
    0x0453, 0x0454, 0x0455, 0x0456, 0x0457, 0x0458, 0x0459, 0x045A,
    0x045B, 0x045C, 0x045E, 0x045F, 0x0490, 0x0491, 0x2116,
#endif
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
    if (slot >= CHARSET_ASCII_BASE && slot < CHARSET_LATIN1_BASE)
        return (uint32_t)(CHARSET_ASCII_LO + (slot - CHARSET_ASCII_BASE));
    if (slot >= CHARSET_LATIN1_BASE && slot < CHARSET_EXTRA_BASE)
        return (uint32_t)(CHARSET_LATIN1_LO + (slot - CHARSET_LATIN1_BASE));
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
