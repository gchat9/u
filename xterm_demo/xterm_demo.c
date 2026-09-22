#include "../_sys/_main.h"
#include "xterm_lib.h"

static void fill_framebuffer(uint32_t shift)
{
    static const uint8_t chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint8_t *fb = xterm_framebuffer();
    int width = xterm_columns();
    int height = xterm_rows();
    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++)
            fb[row * width + col] = chars[(row * width + col + shift) % 36];
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xterm_init();
   
    for (int i=0;;i++) {
        fill_framebuffer(i);
        xterm_render();
        if (xterm_wait(1000))
            xterm_render();
    }
}
