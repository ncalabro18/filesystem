
FROM ubuntu:24.04 AS buildroot-stage

RUN apt update && apt install -y \
    build-essential \
    git \
    wget \
    cpio \
    unzip \
    rsync \
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    libncurses-dev \
    python3 \
    file \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /buildroot

# Pin a specific Buildroot release so the build is reproducible across
# machines and time, not whatever HEAD happens to be on the day you build.
ARG BUILDROOT_VERSION=2024.02.6
RUN git clone --branch ${BUILDROOT_VERSION} --depth 1 \
    https://github.com/buildroot/buildroot.git .

# Bring in our config, kernel config, and rootfs overlay
COPY board/sfs/sfs_riscv64_defconfig configs/
COPY board/sfs/linux.config board/sfs/linux.config
COPY board/sfs/overlay board/sfs/overlay

RUN chmod +x board/sfs/overlay/init

# Cross-compile the test binary and drop it into the overlay *before*
# Buildroot's make runs - BR2_ROOTFS_OVERLAY copies the overlay tree
# into the rootfs as part of the build, so the binary just needs to
# physically exist on disk at this path first. Static, no PIE: /init
# execs this directly with no dynamic loader available in the guest.
COPY tests/test.c /tmp/test.c
RUN mkdir -p board/sfs/overlay/tests && \
    riscv64-linux-gnu-gcc -gdwarf-5 -static -o board/sfs/overlay/tests/run_tests /tmp/test.c && \
    chmod +x board/sfs/overlay/tests/run_tests

RUN make sfs_riscv64_defconfig
RUN FORCE_UNSAFE_CONFIGURE=1 make -j$(nproc)


# Output of interest:
#   output/images/Image            (riscv64 kernel)
#   output/images/rootfs.cpio.gz   (initramfs)


# Final stage: just QEMU + the built artifacts needed for the module,
# so the image used for `docker run` is lighter than the Buildroot
# build stage (which uses in a full toolchain build).
FROM ubuntu:24.04

RUN apt update && apt install -y \
    qemu-system-misc \
    qemu-user-static \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    libc6-dev-riscv64-cross \
    build-essential \
    cpio \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /mnt/sfs

WORKDIR /work

COPY --from=buildroot-stage /buildroot/output/images/Image /work/Image
COPY --from=buildroot-stage /buildroot/output/images/rootfs.cpio.gz /work/rootfs.cpio.gz

# Kernel headers/Module.symvers needed to build the out-of-tree SFS module
# against the exact kernel Buildroot just built.
COPY --from=buildroot-stage /buildroot/output/build/linux-6.18 /work/linux-src

RUN mkdir -p ~/rootfs

COPY sfs/ /work/sfs/

RUN cd /work/sfs && \
make build ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- KDIR=/work/linux-src

COPY sfsutils/ /work/sfsutils/
RUN cd /work/sfsutils && \
    make build ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- KDIR=/work/linux-src

# The module has to physically live inside the initramfs for /init's
# `insmod /sfs.ko` to find it at boot - repack rootfs.cpio.gz with the
# freshly built .ko dropped at the archive root.
RUN apt update && apt install -y cpio gzip && rm -rf /var/lib/apt/lists/* && \
    mkdir -p /tmp/initramfs && \
    cd /tmp/initramfs && \
    zcat /work/rootfs.cpio.gz | cpio -idm && \
    cp /work/sfs/sfs.ko . && \
    find . | cpio -o -H newc | gzip -9 > /work/rootfs.cpio.gz && \
    cd /work && rm -rf /tmp/initramfs

COPY run_qemu.sh /work/run_qemu.sh
RUN chmod +x /work/run_qemu.sh

CMD ["/work/run_qemu.sh"]