#include "../_sys/_main.h"

__attribute__((noreturn)) inline static void main(int argc, char **argv)
{
    char buf[4096];
    long result,l;
    
    // TODO: accept options -L, --logical, -P, --physical
    
    result = getcwd(buf, sizeof(buf));
    if (result < 0) {
        exit(1);
    }
    
    l=strlen(buf)-1;
    buf[l]='\n';
    write(1, buf, l+1);
    exit(0);
    __builtin_unreachable();
}
