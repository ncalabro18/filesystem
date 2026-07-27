#!/bin/bash
set -eu

IMG=/work/sfs-test.img
IMG_SIZE_MB=${SFS_TEST_IMG_SIZE_MB:-4096}

if [ ! -f "$IMG" ]; then
    echo "[run-qemu] creating fresh ${IMG_SIZE_MB}MB test image at $IMG"
    qemu-img create -f raw "$IMG" "${IMG_SIZE_MB}M"
    qemu-riscv64-static /work/sfsutils/sfsutils init "$IMG" "${IMG_SIZE_MB}" 64 256
fi

exec qemu-system-riscv64 \
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