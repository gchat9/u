#include "../_sys/_main.h"
#include "../_lib/xterm.h"
#include "../_font/charset.h"

static void fill_framebuffer(uint32_t shift)
{
    static const uint8_t chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "                                    ";
    static uint8_t slots[72];
    static int slots_ready = 0;
    if (!slots_ready) {
        for (int i = 0; i < 72; i++) slots[i] = (uint8_t)charset_slot(chars[i]);
        slots_ready = 1;
    }
    uint8_t *fb = xterm_framebuffer();
    int width = xterm_columns();
    int height = xterm_rows();
    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++)
            fb[row * width + col] = slots[(row * width + col + shift) % 72];
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xterm_init();
   
    for (int i=0;i<300;i++) {
        fill_framebuffer(i);
        xterm_render();
    }
    exit(0);
}
