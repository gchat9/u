#include "../_sys/_.h"

__attribute__((naked)) __attribute__((noreturn)) void _start(void) {
    exit(0);
    __builtin_unreachable();
}

