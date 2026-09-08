CFLAGS:= -static -nostdlib -flto -Os -fno-stack-clash-protection -fno-stack-protector -fno-exceptions -ffreestanding -ffunction-sections -fdata-sections -Wl,--gc-sections
.PHONY: clean all

all: pwd

pwd:  pwd.c
	$(CC) $(CFLAGS) -static -nostdlib -fno-pic -fno-pie -no-pie \
	  -Wl,-z,max-page-size=0x100 \
	  -Wl,--nmagic          \
	  -Wl,--build-id=none \
	  -Wl,-z,noprop \
	  -Wl,-T,../_sys/cleanup-re.ld \
	  -o pwd pwd.c

clean:
	rm -f pwd 2>/dev/null

