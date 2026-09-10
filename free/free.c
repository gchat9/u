#include "../_sys/_main.h"

static int put(const char *s, size_t n)
{
    return write_all_fd(1, s, n);
}

static int put_u32(uint32_t v)
{
    char buf[12];
    char *p = buf + sizeof(buf);
    do { *--p = (char)('0' + v % 10); } while (v = v / 10);
    return put(p, (size_t)(buf + sizeof(buf) - p));
}

static int field(uint64_t value)
{
    if (put(" ", 1)) return 1;
    return put_u32((uint32_t)(value / 1024));
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    struct sysinfo si;
    uint64_t total, free_mem, swap_total, swap_free;

    (void)argc;
    (void)argv;
    if (sysinfo(&si) < 0) exit(1);

    total = (uint64_t)si.totalram * si.mem_unit;
    free_mem = (uint64_t)si.freeram * si.mem_unit;
    swap_total = (uint64_t)si.totalswap * si.mem_unit;
    swap_free = (uint64_t)si.freeswap * si.mem_unit;

    if (put("             total       used       free\nMem:",
            sizeof("             total       used       free\nMem:") - 1) ||
        field(total) || field(total - free_mem) || field(free_mem) ||
        put("\nSwap:", sizeof("\nSwap:") - 1) ||
        field(swap_total) || field(swap_total - swap_free) || field(swap_free) ||
        put("\n", 1))
        exit(1);

    exit(0);
    __builtin_unreachable();
}
