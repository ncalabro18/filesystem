
CROSS_COMPILE=riscv64-linux-gnu-
CC=gcc
SYS_ROOT=$(HOME)/sysroot
CFLAGS=-gdwarf-5 -std=c2x -Wall -Wextra -static -fPIC -march=rv64id -mabi=lp64d

.PHONY: all clean build
.DEFAULT: sfs_utils

all: sfs_utils
build: sfs_utils
clean:
	rm sfs_utils


sfs_utils: sfs_utils.c
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o $@ $<

sfs_mkfs: sfs_mkfs_main.c sfs_mkfs.c
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o $@ $^

