#include "../_sys/_main.h"
#include "../_lib/xterm.h"

static uint32_t rs = 12345;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

/* frame 0: everything random. frame k>0: a few rows rewritten, cursor moves. */
static void fill(int frame)
{
    uint8_t *fb = xterm_framebuffer(), *fg = xterm_fg_buffer();
    uint8_t *bg = xterm_bg_buffer(), *at = xterm_attr_buffer();
    int W = xterm_columns(), H = xterm_rows();
    for (int r = 0; r < H; r++) {
        if (frame > 0 && (rnd() % 5)) continue;           /* only some rows change */
        for (int c = 0; c < W; c++) {
            int i = r * W + c;
            uint32_t x = rnd();
            fb[i] = (uint8_t)x;
            fg[i] = (x & 0x300) ? 15 : (uint8_t)(x >> 12);
            bg[i] = (x & 0xC00) ? 0 : (uint8_t)(x >> 20);
            at[i] = 0;
            uint32_t k = rnd() % 16;
            if (k == 0) at[i] = XTERM_ATTR_UNDERLINE;
            else if (k == 1) at[i] = XTERM_ATTR_STRIKE;
            else if (k == 2) at[i] = XTERM_ATTR_UNDERLINE | XTERM_ATTR_STRIKE;
            else if (k == 3) at[i] = XTERM_ATTR_MISSING;
            else if (k == 4 && c + 1 < W) {                /* emoji pair (index may be out of range) */
                uint8_t e = (uint8_t)(rnd() % 24);
                fb[i] = fb[i+1] = e; at[i] = XTERM_ATTR_EMOJI_L; at[i+1] = XTERM_ATTR_EMOJI_R;
                fg[i+1] = fg[i]; bg[i+1] = bg[i]; c++;
            } else if (k == 5 && c + 1 < W) {              /* fullwidth pair */
                fb[i+1] = fb[i]; at[i] = XTERM_ATTR_FW_L; at[i+1] = XTERM_ATTR_FW_R | XTERM_ATTR_UNDERLINE;
                fg[i+1] = fg[i]; bg[i+1] = bg[i]; c++;
            }
        }
    }
    xterm_set_cursor(frame % 4 == 3 ? -1 : (frame * 7) % H, (frame * 11) % W);
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    int n = argc > 1 ? argv[1][0] - '0' : 0;
    xterm_init();
    for (int f = 0; f <= n; f++) { fill(f); xterm_render(); }
    xterm_wait(1500);
    exit(0);
}
