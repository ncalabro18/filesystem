#!/bin/bash
set -eu

IMG=/work/sfs-test.img
IMG_SIZE_MB=${SFS_TEST_IMG_SIZE_MB:-64}

if [ ! -f "$IMG" ]; then
    echo "[run-qemu] creating fresh ${IMG_SIZE_MB}MB test image at $IMG"
    qemu-img create -f raw "$IMG" "${IMG_SIZE_MB}M"
    
    echo "[run-qemu] formatting SFS image..."
    qemu-riscv64-static /work/sfsutils/sfs_mkfs "$IMG" 16384
    echo "[run-qemu] SFS image formatted"
fi

qemu-system-riscv64 \
    -M virt \
    -m 512M \
    -smp 2 \
    -nographic \
    -no-reboot \
    -kernel /work/Image \
    -initrd /work/rootfs.cpio.gz \
    -append "console=ttyS0 panic=-1" \
    -drive file="$IMG",format=raw,id=hd0,if=none \
    -device virtio-blk-device,drive=hd0


# echo "[run-qemu] final filesystem tree:"
# qemu-riscv64-static /work/sfsutils/sfsutils list "$IMG"