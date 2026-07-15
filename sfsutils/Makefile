
CROSS_COMPILE=riscv64-linux-gnu-
CC=gcc
CFLAGS=-gdwarf-5 -std=c23 -Wdeclaration-after-statement -Wall -Wextra -static-pie -fPIC -march=rv64id -mabi=lp64d --sysroot=$(HOME)/sysroot
# CFLAGS=-gdwarf-5 -std=c23 -Wdeclaration-after-statement -Wall -Wextra -static-pie -fPIC -march=rv64i -mabi=lp64 --sysroot=$(HOME)/sysroot

.PHONY: all clean


all: sfsutils

sfsutils: sfsutils.c sfs.h
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -o $@ $<
clean:
	rm sfsutils

