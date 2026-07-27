
MODULE_NAME = sfs
SFS_IMG := $(shell pwd)/sfs.img
KDIR ?= $(HOME)/linux


obj-m += $(MODULE_NAME).o

export CROSS_COMPILE=riscv64-linux-gnu-
export ARCH=riscv

.PHONY: build clean run


image:
	dd if=/dev/zero of=$(SFS_IMG) bs=4096 count=256

build:
	$(MAKE) $(MODULE_NAME).ko

$(MODULE_NAME).ko: $(MODULE_NAME).c
	$(MAKE) -C $(KDIR) modules M=$(shell pwd)
	cp $(MODULE_NAME).ko ~/rootfs
	cp $(MODULE_NAME).mod ~/rootfs
	# cp sfsutils/sfsutils ~/rootfs
	# cp sfs.img ~/rootfs

	cd ~/rootfs && find . | cpio -co > ../rootfs.cpio && cd ..

clean:
	$(MAKE) -C ~/linux clean M=$(shell pwd)


run: $(MODULE_NAME).ko
	$(MAKE) clean
	$(MAKE) build
	cd ~/linux && qemu-system-riscv64 \
		-drive file=$(SFS_IMG),format=raw,id=sfs,if=none \
		-device virtio-blk-device,drive=sfs \
		-machine virt \
		-nographic -no-reboot \
		-kernel arch/riscv/boot/Image \
		-initrd ~/rootfs.cpio -append 'panic=-1' # init=~/rootfs/sh

