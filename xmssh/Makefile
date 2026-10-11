# xmssh: the SSH client of ../mssh in a window of its own, with a built-in xterm
# emulator (the xtty layer of ../_lib) instead of the user's terminal.  Same
# sources as mssh; only tty.c differs (this directory's). Needs /etc/font.bfnt
# at run time (make -C ../_font install), as xtmux does.
TARGET     := xmssh
SRCS       := mssh.c tty.c
EXTRA_OBJS := xtty.o vt.o xterm_screen.o xterm.o
include ../_mk/mssh.mk
CFLAGS     += -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -DX11_BACKEND

vpath %.c ../_lib

xterm.o: ../_lib/xterm.c ../_lib/xterm.h
	$(CC) $(CFLAGS) -DXTERM_LIB_ONLY -c $< -o $@
