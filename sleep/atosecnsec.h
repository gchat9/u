static void atosecnsec(const char *str, struct timespec *seconds_nanoseconds){
    if (!seconds_nanoseconds) return;
    unsigned long s = 0, ns = 0;

    if (str){
        const char *p = str;
    
        // Parse integer part
        while (*p >= '0' && *p <= '9')
            s = s * 10 + (*p++ - '0');
    
        // Parse fractional part
        if (*p == '.') {
            p++;
            for(int digit=0; digit<9; digit++){
                ns *= 10;
                if (*p >= '0' && *p <= '9') {
                    ns += (*p++ - '0');
                }
            }
        }
    }
    
    seconds_nanoseconds->tv_sec = s;
    seconds_nanoseconds->tv_nsec = ns;
}
