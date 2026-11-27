// termios.h - terminal control: ioctl numbers, flags, and the kernel's termios

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCSCTTY   0x540e
#define TIOCGPTN    0x80045430
#define TIOCGPGRP   0x540f
#define TIOCSPTLCK  0x40045431
#define IGNBRK      0x00000001
#define BRKINT      0x00000002
#define PARMRK      0x00000008
#define ISTRIP      0x00000020
#define INLCR       0x00000040
#define IGNCR       0x00000080
#define OPOST       0x00000001
#define ECHONL      0x00000040
#define CSIZE       0x00000030
#define CS8         0x00000030
#define PARENB      0x00000100
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
