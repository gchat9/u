/*
 * emoji_charset.h — the emoji font's contents, and nothing more.
 *
 * Twenty commonly-used emoji, rendered at build time (see mkemoji.c, which
 * appends them to font<STEP>.bfnt) as small fixed-palette colour glyphs. This
 * array is the build input: mkemoji.c writes each emoji's codepoint into the
 * emoji file, in this order, next to its record, and xterm looks a codepoint
 * up in that table (xterm_emoji_index), so the programs do not include this
 * file and the set can change by rebuilding the fonts.
 *
 * Whether a codepoint is double-width is not this file's: that is width.h's
 * emoji_wide(), which is true for the whole emoji blocks, bitmap or not, so
 * cell width agrees with what wcwidth()-based programs assume, even for emoji
 * we can only draw as a placeholder box.
 */
#pragma once

#include <stdint.h>

static const uint32_t emoji_charset[] = {
    0x1f600, /* 😀 grinning face                */
    0x1f602, /* 😂 face with tears of joy       */
    0x1f60d, /* 😍 heart eyes                   */
    0x1f622, /* 😢 crying face                  */
    0x1f605, /* 😅 grinning sweat               */
    0x1f60a, /* 😊 smiling face                 */
    0x1f914, /* 🤔 thinking face                */
    0x1f62d, /* 😭 loudly crying                */
    0x1f44d, /* 👍 thumbs up                    */
    0x1f44e, /* 👎 thumbs down                  */
    0x1f64f, /* 🙏 folded hands                 */
    0x1f440, /* 👀 eyes                         */
    0x1f525, /* 🔥 fire                         */
    0x1f389, /* 🎉 party popper                 */
    0x1f480, /* 💀 skull                        */
    0x26a1,  /* ⚡ HIGH VOLTAGE SIGN            */
    0x2705,  /* ✅ check mark button            */
    0x274c,  /* ❌ cross mark                   */
    0x2b50,  /* ⭐ star                         */
    0x1f4af, /* 💯 hundred points               */
    0x1f631, /* 😱 FACE SCREAMING IN FEAR       */
    0x1f680, /* 🚀 rocket                       */
};
#define EMOJI_COUNT ((int)(sizeof(emoji_charset) / sizeof(emoji_charset[0])))
