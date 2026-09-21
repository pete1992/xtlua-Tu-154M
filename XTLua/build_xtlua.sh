#!/data/data/com.termux/files/usr/bin/bash


set -e
# we have 2 working windows crosscompiler at this point
# LLVM - CLANG
export WINCROSS="/data/data/com.termux/files/home/wincross/bin"
# GCC - mingw
export CROSS="/data/data/com.termux/files/home/mingw-gcc-cross/bin"
# export this mess xD
# yes we export both of them 
export PATH="$WINCROSS:$CROSS:$PATH" 
export PATH="/data/data/com.termux/files/home/wincross/bin:$PATH"

echo "=== xtlua Windows x64 Build ==="

ROOT=$(pwd)
BUILD="$ROOT/build"


export CC="x86_64-w64-mingw32-clang"
export CXX="x86_64-w64-mingw32-clang++"
export AR="x86_64-w64-mingw32-ar"
export RANLIB="x86_64-w64-mingw32-ranlib"
export RC="x86_64-w64-mingw32-windres"
export PLATFORM="x86_64-w64-mingw32-"
# working include dir, don't change
export INCLUDEDIR="${CROSS}include"
export NM="${WINCROSS}/x86_64-w64-mingw32-nm"
export STRIP="${CROSS}/${PLATFORM}strip"
export OBJDUMP="${WINCROSS}/x86_64-w64-mingw32-objcopy"
export READELF="${CROSS}/${PLATFORM}readelf"
# if we want to set flags, we better control which Linker is used
export LD="${WINCROSS}bin/ld.lld"

# setting flags here
export CCFLAGS="-O3 -ffunction-sections -fdata-sections -fstack-protector-strong -mguard=cf -D_FORTIFY_SOURCE=3 -ftrivial-auto-var-init=zero -fdiagnostics-fixit-info"
export CXXFLAGS="$CCFLAGS -fstack-protector-all -fcf-protection"
export LDFLAGS="-static-libgcc -static-libstdc++"


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
-DCMAKE_BUILD_TYPE=Release \
-DCMAKE_C_FLAGS="$CCFLAGS" \
-DCMAKE_CXX_FLAGS="$CXXFLAGS" \
-DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
-DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS"

echo "Watch the Configuration"
echo " ";
sleep 20;

echo "=== Build ==="

cmake --build "$BUILD" -j8

echo "=== Finished ==="
