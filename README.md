
# Simple Filesystem - SFS

A filesystem targeting RISC-V.

Originally implemented for *Introduction to Linux Development* offered at UMass Lowell and designed by Red Hat.

Additional features include symbolic links, hard links, file renaming, simplified execution, an extended maximum file size (2 MB), and greatly expanded test coverage (35 total tests).


## SFS RISC-V Test/Demo Image

Reproducible: `docker build` + `docker run` alone take you from checked-in
source to a booted, module-loaded, mounted, and tested SFS instance under
`qemu-system-riscv64`, with no manual steps and no host-side toolchain
dependency beyond Docker itself.

To build and run:

```bash
make run
```


## Build

```bash
docker build -t sfs-riscv-test .
```

This single command:

- Clones a pinned Buildroot release
- Builds a minimal musl toolchain and BusyBox initramfs from `sfs_riscv64_defconfig`
- Builds the Linux kernel from the checked-in `linux.config`
- Cross-compiles `sfs.ko` against that exact kernel
- Repacks the initramfs with `sfs.ko` included
- Produces a runtime image containing only QEMU and the built artifacts


## Run Tests

```bash
docker run --rm sfs-riscv-test
```

No flags are needed. `run-qemu.sh` is the image's default `CMD`. It creates a
fresh test disk image, boots the kernel, and `/init` loads the module,
mounts the test image, runs the tests, unmounts the filesystem cleanly, and
powers off. The entire run is deterministic.

Optionally, pipe the output to a log file:

```bash
docker run --rm sfs-riscv-test | tee sfs-demo-log.txt
```
