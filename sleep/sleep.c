#include "../_sys/_main.h"
#include "atosecnsec.h"

__attribute__((noreturn)) static void main(int argc, char **argv)
{
    struct timespec ts;
    if (argc == 2){
      atosecnsec(argv[1], &ts);
      (void)nanosleep(&ts, NULL);
    }
    exit(0);
    __builtin_unreachable();
}

