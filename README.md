
# Simple Filesystem - sfs

A filesystem targetting RISC-V

Originally implemented for Introduction to Linux Development


## SFS RISC-V test/demo image

Reproducible: `docker build` + `docker run` alone take you from checked-in
source to a booted, module-loaded, mounted-and-tested SFS instance under
`qemu-system-riscv64`, with no manual steps and no host-side toolchain
dependency beyond Docker itself.

## Build

```bash
docker build -t sfs-riscv-test .
```

This single command:
 -  Clones a pinned Buildroot release
 -  Builds a minimal musl toolchain + busybox initramfs from `sfs_riscv64_defconfig`
 -  Builds the Linux kernel from the checked-in `linux.config`
 -  Cross-compiles `sfs.ko` against that exact kernel
 -  Repacks the initramfs with `sfs.ko` included
 -  Produces a runtime image with just QEMU + the built artifacts

## Run

```bash
docker run --rm sfs-riscv-test
```

No flags needed - `run-qemu.sh` is the image's default `CMD`. It creates a
fresh test disk image, boots the kernel, and `/init` loads the module,
mounts the test image, runs the demo (or your test suite, once that's
wired in), unmounts cleanly, and powers off. The whole run is a
deterministic, capturable serial log from boot to shutdown - pipe it to a
file for your writeup:

```bash
docker run --rm sfs-riscv-test | tee sfs-demo-log.txt
```

