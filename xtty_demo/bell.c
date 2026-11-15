/* Driver for test_bell.py: a window whose key 'b' makes a BEL, one second later. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../_lib/xtty.h"

static double pending;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void on_input(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++)
        if (b[i] == 'b') pending = now() + 1.0;
}

int main(void)
{
    if (xtty_open("xtty bell test")) return 1;
    xtty_write("ready\r\n", 7);
    for (;;) {
        xtty_poll(50, on_input, NULL);
        if (xtty_closed()) break;
        if (pending && now() >= pending) { pending = 0; xtty_write("\a", 1); }
    }
    return 0;
}
