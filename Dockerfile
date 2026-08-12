
### BUILD ROOT STAGE ###
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

# Bring in config, kernel config, and rootfs overlay
COPY board/sfs/sfs_riscv64_defconfig configs/
COPY board/sfs/linux.config board/sfs/linux.config
COPY board/sfs/overlay board/sfs/overlay

# see board/sfs/sfs_riscv64_defconfig for initramfs details
RUN make sfs_riscv64_defconfig


# compile linux
RUN FORCE_UNSAFE_CONFIGURE=1 make -j$(nproc)

# To locate compiler used to build linux:
# RUN find /buildroot/output/host/bin -type l -o -type f | grep -E 'riscv64.*-(gcc|g\+\+)$'

RUN mkdir -p /work/sfs
RUN mkdir -p /root/rootfs

COPY tests/test.c /tmp/test.c
COPY tests/data/executable_test.c /tmp/executable_test.c
COPY sfs/ /work/sfs/
RUN mkdir -p board/sfs/overlay/tests && \
    riscv64-linux-gnu-gcc -gdwarf-5 -static \
    -o board/sfs/overlay/tests/run_tests /tmp/test.c && \
    chmod +x board/sfs/overlay/tests/run_tests && \
    \
    riscv64-linux-gnu-gcc -static -o board/sfs/overlay/tests/executable_test \
    /tmp/executable_test.c && \
    chmod +x board/sfs/overlay/tests/run_tests board/sfs/overlay/tests/executable_test


# module is built with the same compiler used to build the kernel
RUN cd /work/sfs && \
    make build \
        ARCH=riscv \
        CROSS_COMPILE=/buildroot/output/host/bin/riscv64-buildroot-linux-musl- \
        KDIR=/buildroot/output/build/linux-6.18


COPY sfsutils/ /work/sfsutils/
RUN cd /work/sfsutils && \
    make sfs_mkfs \
        ARCH=riscv \
        CROSS_COMPILE=/buildroot/output/host/bin/riscv64-buildroot-linux-musl- \
        KDIR=/buildroot/output/build/linux-6.18




### FINAL STAGE ###
# Just QEMU + cpio
# so the image used for `docker run` is lighter than the build root stage
FROM ubuntu:24.04

RUN apt update && apt install -y \
    qemu-system-misc \
    qemu-user-static \
    qemu-utils \
    cpio \
    && rm -rf /var/lib/apt/lists/*


RUN groupadd appgroup && \
    useradd -m -g appgroup appuser

RUN mkdir -p /mnt/sfs

WORKDIR /work

COPY --from=buildroot-stage /work/sfs /work/sfs
COPY --from=buildroot-stage /work/sfsutils /work/sfsutils

COPY --from=buildroot-stage /buildroot/board/sfs/overlay/tests /work/tests
COPY --from=buildroot-stage /buildroot/output/images/Image /work/Image
COPY --from=buildroot-stage /buildroot/output/images/rootfs.cpio.gz /work/rootfs.cpio.gz

# Kernel headers/Module.symvers needed to build the out-of-tree SFS module
# against the exact kernel Buildroot just built.
COPY --from=buildroot-stage /buildroot/output/build/linux-6.18 /work/linux-src

COPY --from=buildroot-stage /buildroot/board/sfs/overlay/init /init


# The module has to physically live inside the initramfs for /init's
# `insmod /sfs.ko` to find it at boot - repack rootfs.  cpio.gz with the
# freshly built .ko dropped at the archive root.
RUN apt update && apt install -y cpio gzip && rm -rf /var/lib/apt/lists/* && \
    mkdir -p /tmp/initramfs && \
    cd /tmp/initramfs && \
    zcat /work/rootfs.cpio.gz | cpio -idm && \
    cp /work/sfs/sfs.ko . && \
    cp /work/sfsutils/sfs_mkfs . && \
    mkdir -p tests/data && \
    cp /work/tests/run_tests tests/run_tests && \
    cp /work/tests/executable_test tests/executable_test && \
    cp /init ./init && \
    chmod +x ./init && \
    find . | cpio -o -H newc | gzip -9 > /work/rootfs.cpio.gz && \
    cd /work && rm -rf /tmp/initramfs

RUN zcat /work/rootfs.cpio.gz | cpio -it | grep '^init$'

COPY run_qemu.sh /work/run_qemu.sh
RUN chmod +x /work/run_qemu.sh

CMD ["/work/run_qemu.sh"]