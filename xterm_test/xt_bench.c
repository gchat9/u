#include "../_sys/_main.h"
#include "../_lib/xterm.h"

static uint32_t rs = 777;
static uint32_t rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

/* modes: a = demo-like (every cell changes, default colours)
 *        b = every cell changes AND has its own random colours (LUT worst case)
 *        c = text-like screen (70% blank, colour runs); each frame touches ONE row
 *        d = same text-like screen but every row changes each frame           */
__attribute__((noreturn)) static void main(int argc, char **argv)
{
    int mode = argc > 1 ? argv[1][0] : 'a';
    xterm_init();
    uint8_t *fb = xterm_framebuffer(), *fg = xterm_fg_buffer(), *bg = xterm_bg_buffer();
    int W = xterm_columns(), H = xterm_rows();
    for (int i = 0; i < W * H; i++) {           /* initial text-like screen */
        fb[i] = (rnd() % 10 < 7) ? 0 : (uint8_t)(1 + rnd() % 94);
        fg[i] = 15; bg[i] = 0;
    }
    for (int f = 0; f < 300; f++) {
        for (int r = 0; r < H; r++) {
            if (mode == 'c' && r != f % H) continue;
            for (int c = 0; c < W; c++) {
                int i = r * W + c;
                if (mode == 'a') fb[i] = (uint8_t)(1 + (i + f) % 60);
                else if (mode == 'b') { fb[i] = (uint8_t)(1 + (i + f) % 94); fg[i] = rnd(); bg[i] = rnd(); }
                else {
                    fb[i] = (rnd() % 10 < 7) ? 0 : (uint8_t)(1 + rnd() % 94);
                    fg[i] = (c / 6) % 3 ? 15 : 10 + (c / 6) % 5;
                }
            }
        }
        xterm_set_cursor(f % H, f % W);
        xterm_render();
    }
    exit(0);
}
