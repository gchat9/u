/*
 * charset.h — the font's 256 glyph slots, and nothing more.
 *
 * The font is a flat array of 256 fixed-size bitmaps; a glyph "slot"
 * (0-255) is just an index into it, with no inherent meaning of its
 * own. This file is the single place that assigns a Unicode codepoint
 * to each slot, in both directions:
 *
 *   charset_codepoint(slot) -> codepoint   used by mkfont.c to decide
 *                                          what to rasterize into slot
 *   charset_slot(codepoint) -> slot        used by the renderer to map
 *                                          a decoded character to a
 *                                          glyph it can actually draw
 *
 * Layout: ASCII (0x20-0x7E) first, then the Latin-1 supplement
 * (0xA0-0xFF), both as plain arithmetic offsets -- no table, no
 * lookup, just a range check and a subtraction. That's 191 of the 256
 * slots. The remaining 65 hold hand-picked extras: light and double
 * box-drawing (what dialog/whiptail/ncurses TUIs actually draw),
 * rounded corners, a few block/shade glyphs, and four arrows -- looked
 * up by linear scan over a small array, since a binary search would be
 * more code for no measurable benefit at this size (65 entries, and
 * only actually scanned for characters outside ASCII/Latin-1, i.e.
 * essentially never in normal text). Extending the charset later means
 * appending to charset_extra[] (order doesn't matter) up to
 * CHARSET_EXTRA_MAX total.
 */
#pragma once

#include <stdint.h>

#define CHARSET_ASCII_BASE   0
#define CHARSET_ASCII_LO     0x20
#define CHARSET_ASCII_HI     0x7E
#define CHARSET_ASCII_COUNT  (CHARSET_ASCII_HI - CHARSET_ASCII_LO + 1)     /* 95 */

#define CHARSET_LATIN1_BASE  (CHARSET_ASCII_BASE + CHARSET_ASCII_COUNT)    /* 95 */
#define CHARSET_LATIN1_LO    0xA0
#define CHARSET_LATIN1_HI    0xFF
#define CHARSET_LATIN1_COUNT (CHARSET_LATIN1_HI - CHARSET_LATIN1_LO + 1)   /* 96 */

#define CHARSET_EXTRA_BASE   (CHARSET_LATIN1_BASE + CHARSET_LATIN1_COUNT)  /* 191 */
#define CHARSET_EXTRA_MAX    (256 - CHARSET_EXTRA_BASE)                   /* 65  */

static const uint16_t charset_extra[] = {
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,
    0x252C, 0x2534, 0x253C,                            /* light box drawing */
    0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2560, 0x2563,
    0x2566, 0x2569, 0x256C,                            /* double box drawing */
    0x256D, 0x256E, 0x256F, 0x2570,                    /* rounded corners */
    0x2580, 0x2584, 0x2588, 0x2591, 0x2592, 0x2593,    /* block / shade */
    0x25B2, 0x25BA, 0x25BC, 0x25C4,                    /* arrows: up right down left */
};
#define CHARSET_EXTRA_COUNT ((int)(sizeof(charset_extra) / sizeof(charset_extra[0])))

_Static_assert(CHARSET_EXTRA_COUNT <= CHARSET_EXTRA_MAX,
              "charset_extra[] overflows the font's 256 glyph slots -- trim it");

/* slot -> codepoint. Returns 0 for a slot past the end of the charset
 * (mkfont.c leaves those blank). */
static inline uint32_t charset_codepoint(int slot)
{
    if (slot >= CHARSET_ASCII_BASE && slot < CHARSET_LATIN1_BASE)
        return (uint32_t)(CHARSET_ASCII_LO + (slot - CHARSET_ASCII_BASE));
    if (slot >= CHARSET_LATIN1_BASE && slot < CHARSET_EXTRA_BASE)
        return (uint32_t)(CHARSET_LATIN1_LO + (slot - CHARSET_LATIN1_BASE));
    if (slot >= CHARSET_EXTRA_BASE && slot < CHARSET_EXTRA_BASE + CHARSET_EXTRA_COUNT)
        return charset_extra[slot - CHARSET_EXTRA_BASE];
    return 0;
}

/* codepoint -> slot. Returns -1 if this charset has no glyph for it
 * (caller substitutes '?' or similar via a second charset_slot() call). */
static inline int charset_slot(uint32_t cp)
{
    if (cp >= CHARSET_ASCII_LO && cp <= CHARSET_ASCII_HI)
        return CHARSET_ASCII_BASE + (int)(cp - CHARSET_ASCII_LO);
    if (cp >= CHARSET_LATIN1_LO && cp <= CHARSET_LATIN1_HI)
        return CHARSET_LATIN1_BASE + (int)(cp - CHARSET_LATIN1_LO);
    for (int i = 0; i < CHARSET_EXTRA_COUNT; i++)
        if (charset_extra[i] == cp) return CHARSET_EXTRA_BASE + i;
    return -1;
}
