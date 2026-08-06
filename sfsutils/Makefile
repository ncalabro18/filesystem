
CROSS_COMPILE=riscv64-linux-gnu-
CC=gcc
SYS_ROOT=$(HOME)/sysroot
CFLAGS=-gdwarf-5 -std=c2x -Wall -Wextra -static -fPIC -march=rv64id -mabi=lp64d # --sysroot=$(SYS_ROOT)

.PHONY: all clean build
.DEFAULT: sfsutils

all: sfsutils
build: sfsutils
clean:
	rm sfsutils


sfsutils: sfsutils.c
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o $@ $<


