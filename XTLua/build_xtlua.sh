#!/data/data/com.termux/files/usr/bin/bash


set -e
# we have 2 working windows crosscompiler at this point
# LLVM - CLANG

export CROSS="/data/data/com.termux/files/home/mingw-gcc-cross/bin"
# export this mess xD
# yes we export both of them 
export PATH="$CROSS:$PATH" 

echo "=== xtlua Windows x64 Build ==="

ROOT=$(pwd)
BUILD="$ROOT/build"
export PLATFORM="x86_64-w64-mingw32-ucrt"
export INCLUDEDIR="/data/data/com.termux/files/home/mingw-gcc-cross/include"
export CC="${CROSS}/clang"
export CXX="${CROSS}/clang++"
export CPP="${CROSS}/clang-cpp"
export AS="${CROSS}/llvm-as"
export AR="${CROSS}/llvm-ar"
export RANLIB="${CROSS}/llvm-ranlib"
export RC="${CROSS}/llvm-windres"
export NM="${CROSS}/llvm-nm"
export STRIP="${CROSS}/llvm-strip"
export OBJDUMP="${CROSS}/llvm-objcopy"
export READELF="${CROSS}/llvm-readelf"
export LD="${CROSS}bin/llvm-link"


export CFLAGS="-O3 -flto -ffunction-sections -fdata-sections -fstack-protector-all -D_FORTIFY_SOURCE=3 -ftrivial-auto-var-init=zero -fwinx64-eh-unwind=v2-best-effort -mguard=cf -fcf-protection=full -fdiagnostics-fixit-info"

export CXXFLAGS="$CFLAGS"

export LDFLAGS="-flto -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--guard-cf -Wl,--guard-longjmp"


echo " ";
echo "=== Configure CMake ==="

rm -rf "$BUILD"

cmake -S "$ROOT" \
-B "$BUILD" \
-G "Unix Makefiles" \
-DCMAKE_SYSTEM_NAME=Windows \
-DCMAKE_C_COMPILER="${CC}" \
-DCMAKE_CXX_COMPILER="${CXX}" \
-DCMAKE_RC_COMPILER="${RC}" \
-DCMAKE_LINKER_TYPE=LLD \
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_C_FLAGS="${CFLAGS}" \
-DCMAKE_CXX_FLAGS="${CXXFLAGS}" \
-DCMAKE_EXE_LINKER_FLAGS="${CFLAGS} ${LDFLAGS}" \
-DCMAKE_SHARED_LINKER_FLAGS="${CFLAGS} ${LDFLAGS}"

echo "Watch the Configuration"
echo " ";
sleep 5;

echo "=== Build ==="

cmake --build "$BUILD" -j8

echo "=== Finished ==="
