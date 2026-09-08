CFLAGS:= -static -nostdlib -flto -Os -fno-stack-clash-protection -fno-stack-protector -fno-exceptions -ffreestanding -ffunction-sections -fdata-sections -Wl,--gc-sections
.PHONY: clean all

all: sleep

sleep:  sleep.c atosecnsec.h
	$(CC) $(CFLAGS) -static -nostdlib -fno-pic -fno-pie -no-pie \
	  -Wl,-z,max-page-size=0x100 \
	  -Wl,--nmagic          \
	  -Wl,--build-id=none \
	  -Wl,-z,noprop \
	  -Wl,-T,../_sys/cleanup-re.ld \
	  -o sleep sleep.c

clean:
	rm -f sleep 2>/dev/null
