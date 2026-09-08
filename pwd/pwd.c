#include "../_sys/_main.h"

static size_t my_strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    char buf[4096];
    long result;
    
    // Ignore standard options: -L, --logical, -P, --physical
    // (just skip them without processing)
    
    result = getcwd(buf, sizeof(buf));
    if (result < 0) {
        exit(1);
    }
    
    write(1, buf, my_strlen(buf));
    write(1, "\n", 1);
    exit(0);
    __builtin_unreachable();
}
