// termios.h - terminal control: ioctl numbers, flags, and the kernel's termios

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCSCTTY   0x540e
#define TIOCGPTN    0x80045430
#define TIOCGPGRP   0x540f
#define TIOCSPTLCK  0x40045431
#define OPOST       0x00000001
#define IXON        0x00000400
#define ICRNL       0x00000100
#define ISIG        0x00000001
#define ICANON      0x00000002
#define ECHO        0x00000008
#define IEXTEN      0x00008000
#define VTIME       5
#define VMIN        6

/* Kernel termios ABI (generic layout: x86, arm, aarch64, riscv). */
struct ktermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};
struct winsz { uint16_t row, col, xpix, ypix; };
