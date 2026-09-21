#!/data/data/com.termux/files/usr/bin/bash

set -e

make clean

rm -f /data/data/com.termux/files/usr/tmp/*
rm -f src/lj_vm.S src/lj_vm.o
rm -f src/host/buildvm
rm -f src/*.o
rm -f src/*.a
rm -f src/luajit
rm -f src/luajit.exe


export MINGW_HOME="/data/data/com.termux/files/usr/opt/llvm-mingw-w64"
export TARGET="x86_64-w64-mingw32"
export TOOLCHAIN="${MINGW_HOME}/bin"
export SYSROOT="${MINGW_HOME}/${TARGET}"
export PATH="${TOOLCHAIN}:$PATH"
export CC="${TOOLCHAIN}/${TARGET}-clang"
export CXX="${TOOLCHAIN}/${TARGET}-clang++"
export CPP="${CC} -E"
export LD="${MINGW_HOME}/bin/ld.lld"
export AR="${TOOLCHAIN}/${TARGET}-ar"
export AS="${TOOLCHAIN}/${TARGET}-as"
export NM="${TOOLCHAIN}/${TARGET}-nm"
export RANLIB="${TOOLCHAIN}/${TARGET}-ranlib"
export STRIP="${TOOLCHAIN}/${TARGET}-strip"
export STRINGS="${TOOLCHAIN}/${TARGET}-strings"
export OBJCOPY="${TOOLCHAIN}/${TARGET}-objcopy"
export OBJDUMP="${TOOLCHAIN}/${TARGET}-objdump"
export CFLAGS="-O1 -fstack-protector-strong \
--target=${TARGET} \
--sysroot=${SYSROOT}"


make -e \
TARGET_CC="/data/data/com.termux/files/usr/bin/x86_64-w64-mingw32-clang" \
TARGET_LDFLAGS="--sysroot=/data/data/com.termux/files/usr/sysroot -L/data/data/com.termux/files/usr/sysroot/lib" \
TARGET_SYS=Windows \
BUILDMODE=static
