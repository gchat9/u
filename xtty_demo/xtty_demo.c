/* Shows what xtty is for: a "terminal program" in a window, with no pty and no
 * terminal emulator around it.  Prints a few lines and logs, as hex lines, the
 * input it gets (keys, mouse reports, pastes, query replies) to the file named
 * by -l; "size RxC" when the window is resized.
 *
 *   xtty_demo [-m] [-l logfile]      -m: ask for mouse reports (?1000, ?1006)
 */
#include <stdio.h>
#include <string.h>

#include "../_lib/xtty.h"

static FILE *logf;

static void on_input(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    if (!logf) return;
    for (size_t i = 0; i < n; i++) fprintf(logf, "%02x", b[i]);
    fputc('\n', logf); fflush(logf);
}

static void draw(void)
{
    int rows, cols; char s[160];
    xtty_size(&rows, &cols);
    int n = snprintf(s, sizeof s, "\033[H\033[2Jxtty demo %dx%d\r\nfoo bar-baz  qux\r\nsecond line here\r\n", cols, rows);
    xtty_write(s, (size_t)n);
}

int main(int argc, char **argv)
{
    int mouse = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) mouse = 1;
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) logf = fopen(argv[++i], "w");
    }
    if (xtty_open("xtty demo")) { fputs("xtty_demo: cannot open the window\n", stderr); return 1; }
    xtty_write("\033[?2004h", 8);                       /* bracketed paste */
    if (mouse) xtty_write("\033[?1000h\033[?1006h", 16);
    draw();
    xtty_write("\033[6n", 4);                           /* where is the cursor? (cursor position report) */
    while (!xtty_closed()) {
        xtty_poll(1000, on_input, NULL);
        if (xtty_resized()) {
            int r, c; xtty_size(&r, &c);
            if (logf) { fprintf(logf, "size %dx%d\n", r, c); fflush(logf); }
            draw();
        }
    }
    xtty_close();
    return 0;
}
