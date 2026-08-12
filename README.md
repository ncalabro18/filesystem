
# Simple Filesystem - SFS

[![SFS Tests](https://github.com/ncalabro18/filesystem/actions/workflows/test.yml/badge.svg)](https://github.com/ncalabro18/filesystem/actions/workflows/test.yml)

A filesystem targeting RISC-V.

Originally implemented for *Introduction to Linux Development* - offered at UMass Lowell and designed by Red Hat.

Additional features include extents, symbolic links, hard links, file renaming, simplified execution, and greatly expanded test coverage (34 total tests).


## SFS RISC-V Test/Demo Image

Reproducible: `make run_tests` alone takes you from
source to a booted, module-loaded, mounted, and tested SFS instance under
`qemu-system-riscv64`, with no manual steps and no host-side toolchain
dependency beyond Docker and Make (the Makefile is there for convenience,
not a real build requirement).



## Build

```bash
docker build -t sfs-riscv-test .
```

This single command:

- Clones a pinned Buildroot release
- Builds a minimal musl toolchain and BusyBox initramfs from `sfs_riscv64_defconfig`
- Builds the Linux kernel from `linux.config`
- Cross-compiles `sfs.ko` against that exact kernel using the same compiler
- Repacks the initramfs with `sfs.ko` included
- Produces a runtime image containing only QEMU and the built artifacts


## Run Tests

```bash
docker run --rm sfs-riscv-test
```

No flags are needed. `run-qemu.sh` is the image's default `CMD`.
 - boots the kernel
 - `/init` loads the module
 - mounts the test image
 - runs the test suite (TAP output)
 - unmounts
 - remounts for a persistence test
 - unmounts
 - powers off
 
The entire run is deterministic.

Optionally, pipe the output to a log file:

```bash
docker run --rm sfs-riscv-test | tee sfs-demo-log.txt
```
