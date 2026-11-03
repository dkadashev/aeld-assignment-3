#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

SRCDIR="$(realpath $(dirname $0))"
OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION="1.33.1"
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
MAKE_ARGS="-j8 CROSS_COMPILE=$CROSS_COMPILE ARCH=$ARCH"

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}
    make $MAKE_ARGS defconfig
    make $MAKE_ARGS all
fi

echo "Adding the Image in outdir"
cp "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

mkdir "$OUTDIR/rootfs"
cd "$OUTDIR/rootfs"
mkdir bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir usr/bin usr/lib usr/sbin
mkdir var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
    # Git clone times out for me, so we're just downloading the source here
    # git clone git://busybox.net/busybox.git
    # cd busybox
    # git checkout ${BUSYBOX_VERSION}
    BUSYBOX_TARBALL="busybox-${BUSYBOX_VERSION}.tar.bz2"
    rm -f ${BUSYBOX_TARBALL}
    wget "https://busybox.net/downloads/${BUSYBOX_TARBALL}"
    tar xf "${BUSYBOX_TARBALL}"
    mv "busybox-${BUSYBOX_VERSION}" busybox
    cd busybox

    make distclean
    make defconfig
    # with TC build fails on kernels 6.8+:
    # https://lists.busybox.net/pipermail/busybox-cvs/2024-January/041752.html
    sed -i 's/^CONFIG_TC=y$/CONFIG_TC=n/' .config
else
    cd busybox
fi

make $MAKE_ARGS
make $MAKE_ARGS CONFIG_PREFIX="${OUTDIR}/rootfs" install

echo "Library dependencies"
SEARCH_DIRS="$(${CROSS_COMPILE}gcc --print-search-dirs |
    awk 'BEGIN{rv=1}; /^libraries: /{dirs=$0; sub(/^libraries: =?/, "", dirs); split(dirs, dirs_a, /:/); for (d in dirs_a) { print dirs_a[d] }; rv=0} END{exit rv}'
)"
INTERPRETER="$(basename $(${CROSS_COMPILE}readelf -a busybox | grep "program interpreter" | sed -re 's/^.*program interpreter: ([a-zA-Z0-9/.-]+)\]$/\1/'))"
LIBS="$(${CROSS_COMPILE}readelf -a busybox | grep "Shared library" | sed -re 's/^.*Shared library: \[([a-zA-Z0-9/.-]+)\]$/\1/')"

copy_in() {
    L="$1"
    TARGET="$2"
    FOUND=0
    for D in ${SEARCH_DIRS}; do
        FULL_PATH="$D/$L"
        if [ -r "${FULL_PATH}" ]; then
            echo "Copying in ${FULL_PATH}"
            cp "${FULL_PATH}" "${TARGET}"
            FOUND=1
            break
        fi
    done
    if [ "$FOUND" -ne 1 ]; then
        echo "Can't find $L" >&2
        exit 1
    fi
}

copy_in "$INTERPRETER" "${OUTDIR}/rootfs/lib/"
for l in ${LIBS}; do
    copy_in $l "${OUTDIR}/rootfs/lib64"
    copy_in $l "${OUTDIR}/rootfs/lib"
done

sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/console" c 5 1

cd "${SRCDIR}"
make clean
make CC=gcc $MAKE_ARGS

cp -a writer finder.sh finder-test.sh ../conf autorun-qemu.sh "${OUTDIR}/rootfs/home/"
sed -i -r -e "s/..\/conf\/assignment/conf\/assignment/" "${OUTDIR}/rootfs/home/finder-test.sh"

cd "${OUTDIR}/rootfs"
find . | cpio -H newc -ov --owner root:root | gzip >"${OUTDIR}/initramfs.cpio.gz"
