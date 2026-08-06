
.PHONY = run build
.DEFAULT = run

MODULE_NAME = sfs



build:
	$(MAKE) test


test: test.c
	riscv64-linux-gnu-gcc -gdwarf-5 -static-pie -fPIC --sysroot=$(HOME)/sysroot -o $(HOME)/rootfs/test test.c
	cd ~/rootfs && find . | cpio -co > ../rootfs.cpio && cd ..

run:
	make test
	cd ~/linux && qemu-system-riscv64 -machine virt -nographic -no-reboot -kernel arch/riscv/boot/Image -initrd ~/rootfs.cpio -append 'panic=-1'

