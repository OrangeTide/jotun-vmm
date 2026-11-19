#!/bin/sh
# Demonstration: build a Linux kernel from source inside the Jotun guest.
#
# The linux-kernel-demo Make target copies this script into the kernel source
# tree and runs it as pid 1. It configures a minimal kernel (modules, debug
# info, BTF, and the Python-dependent DRM_MSM driver all off) and builds the
# arch image. The whole compile and link runs in-guest on a host toolchain
# served through the rootfs mount. See doc/kernel-build.md for the full runbook.
#
# JOBS overrides the parallelism (default 4), matched to the libOS process
# budget rather than the host core count.

export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/root
cd /ksrc || { echo "no /ksrc mount"; exit 1; }

case "$(uname -m)" in
    aarch64) KARCH=arm64;  KIMG=Image ;;
    x86_64)  KARCH=x86_64; KIMG=bzImage ;;
    *) echo "unsupported arch $(uname -m)"; exit 1 ;;
esac

echo "=== kernel-demo: make ARCH=$KARCH defconfig ==="
make ARCH="$KARCH" defconfig || exit 1

echo "=== kernel-demo: minimal config (no MODULES/debug/BTF/gdb/msm) ==="
sed -i \
    -e 's/^CONFIG_MODULES=y/# CONFIG_MODULES is not set/' \
    -e 's/^CONFIG_GDB_SCRIPTS=y/# CONFIG_GDB_SCRIPTS is not set/' \
    -e 's/^CONFIG_DRM_MSM=[ym]/# CONFIG_DRM_MSM is not set/' \
    .config
sed -i \
    -e '/^CONFIG_DEBUG_INFO_DWARF.*=y/d' \
    -e '/^CONFIG_DEBUG_INFO_BTF.*=y/d' \
    -e 's/^CONFIG_DEBUG_INFO=y/# CONFIG_DEBUG_INFO is not set/' \
    .config
echo 'CONFIG_DEBUG_INFO_NONE=y' >> .config
make ARCH="$KARCH" olddefconfig || exit 1

echo "=== kernel-demo: make ARCH=$KARCH -j${JOBS:-4} $KIMG ==="
make ARCH="$KARCH" -j"${JOBS:-4}" "$KIMG"
rc=$?
if [ $rc -ne 0 ]; then
    echo "=== kernel-demo: parallel stopped (rc=$rc); finishing serially ==="
    make ARCH="$KARCH" "$KIMG"
    rc=$?
fi

ls -l vmlinux "arch/$KARCH/boot/$KIMG" 2>/dev/null
echo "=== kernel-demo END rc=$rc ==="
exit $rc
