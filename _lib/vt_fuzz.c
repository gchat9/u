/*
 * vt_fuzz.c -- drives vt_feed() with the traffic a hostile server can send, for
 * vt_test.py (built with ASan/UBSan).  Usage:
 *   vt_fuzz repro            the screen shrink that left the saved cursor out of range
 *   vt_fuzz SEED ITERATIONS  random escape-heavy traffic, split at random points,
 *                            with resizes; exit status 0 if nothing went wrong
 */
#include "vt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t b[32]; int n; } Frag;
static const Frag frags[] = {
  {{0x1b,'['},2}, {{0x1b,']'},2}, {{0x1b,'P'},2}, {{0x1b,'('},2}, {{0x1b,'#'},2},
  {{0x1b,'7'},2}, {{0x1b,'8'},2}, {{0x1b,'c'},2}, {{0x1b,'\\'},2}, {{';'},1}, {{'?'},1},
  {{0x07},1}, {{'2',';'},2}, {{0xf0,0x9f,0x98,0x80},4}, {{0xe4,0xbd,0xa0},3}, {{0xc3,0xa9},2},
  {{0xff},1}, {{0x80},1},
  {{0x1b,'[','1','0','0','0','0','0','0','0','0','0','0','H'},15},
  {{0x1b,'[','9','9','9','9','9','9','9','9','9','9','9','9','m'},15},
  {{0x1b,'[','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','0','X'},24},
  {{0x1b,'[','?','1','0','4','9','h'},8},
  {{0x1b,'[','3','8',';','5',';','9','9','9','9','m'},12},
  {{0x1b,'[','4','8',';','2',';','2','5','5',';','0',';','0','m'},16},
  {{0x1b,']','0',';','x','y','z',0x07},7},
  {{0x1b,'[','@'},3}, {{0x1b,'[','S'},3}, {{0x1b,'[','T'},3}, {{0x1b,'[','L'},3}, {{0x1b,'[','M'},3},
  {{0x1b,'[','r'},3}, {{0x1b,'[','0',';','0','r'},5}, {{0x1b,'[','2','J'},4}, {{0x1b,'[','3','J'},4},
};
static const char alpha[] = "0123456789;:<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_`abcdefghijklmnopqrstuvwxyz{|}~";

static int repro(void) {
    /* Save the cursor on the bottom row of a blank screen, move it to the top,
     * shrink the screen (which evicts rows into the scrollback), restore. */
    VTParser *p = calloc(1, sizeof *p);
    vt_init(p, 50, 80);
    const char *a = "\x1b[50;1H\x1b" "7" "\x1b[1;1H";
    vt_feed(p, (const uint8_t *)a, strlen(a));
    vt_resize(p, 16, 80);
    const char *b = "\x1b" "8" "X";
    vt_feed(p, (const uint8_t *)b, strlen(b));
    int ok = p->scr.cur_row < p->scr.rows && p->scr.cur_col < p->scr.cols;
    vt_free(p); free(p);
    printf("%s: cursor after restore is inside the 16-row screen: %s\n", "repro", ok ? "yes" : "NO");
    return ok ? 0 : 1;
}

static int fuzz(unsigned seed, long iters) {
    srand(seed);
    VTParser *p = calloc(1, sizeof *p);
    int rows = 24, cols = 80;
    vt_init(p, rows, cols);
    for (long it = 0; it < iters; it++) {
        uint8_t buf[1024]; size_t n = 0;
        int k = 1 + rand() % 60;
        for (int j = 0; j < k && n < sizeof buf - 32; j++) {
            int r = rand() % 10;
            if (r < 5) { const Frag *f = &frags[rand() % (sizeof frags / sizeof frags[0])];
                         memcpy(buf + n, f->b, f->n); n += f->n; }
            else if (r < 9) buf[n++] = (uint8_t)alpha[rand() % (sizeof alpha - 1)];
            else buf[n++] = (uint8_t)rand();
        }
        size_t off = 0;                     /* split at random points: sequences straddle reads */
        while (off < n) { size_t take = 1 + rand() % (n - off); vt_feed(p, buf + off, take); off += take; }
        if (rand() % 300 == 0) { rows = 1 + rand() % 60; cols = 1 + rand() % 200; vt_resize(p, rows, cols); }
    }
    vt_free(p); free(p);
    printf("fuzz seed=%u iterations=%ld: no fault\n", seed, iters);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "repro")) return repro();
    if (argc == 3) return fuzz((unsigned)atoi(argv[1]), atol(argv[2]));
    fprintf(stderr, "usage: vt_fuzz repro | vt_fuzz SEED ITERATIONS\n");
    return 2;
}
