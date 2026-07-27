#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev

echo "### Loading SFS module ###"
insmod /sfs.ko

echo "### Mounting test image ###"
mkdir -p /mnt/sfs
mount -t sfs /dev/vdb /mnt/sfs

echo "### Demo: write/read/list ###"
echo "hello from SFS" > /mnt/sfs/test.txt
cat /mnt/sfs/test.txt
ls -la /mnt/sfs

echo "### Unmounting cleanly ###"
umount /mnt/sfs
rmmod sfs

echo "### SFS demo complete ###"
poweroff -f