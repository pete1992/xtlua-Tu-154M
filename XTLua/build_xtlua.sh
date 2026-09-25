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
#export PATH="/data/data/com.termux/files/home/wincross/bin:$PATH"

echo "=== xtlua Windows x64 Build ==="

ROOT=$(pwd)
BUILD="$ROOT/build"

export PLATFORM="x86_64-w64-mingw32-"
export CC="x86_64-w64-mingw32-clang"
export CXX="x86_64-w64-mingw32-clang++"
export AR="${WINCROSS}/llvm-ar"
export AS="${WINCROSS}/llvm-as"
export RANLIB="${WINCROSS}/llvm-ranlib"
export RC="${WINCROSS}/llvm-rc"
# working include dir, don't change
export INCLUDEDIR="${CROSS}include"
export NM="${WINCROSS}/llvm-nm"
export STRIP="${WINCROSS}/llvm-strip"
export OBJDUMP="${WINCROSS}/llvm-objdump"
export READELF="${WINCROSS}/llvm-readelf"
# if we want to set flags, we better control which Linker is used
export LD="${WINCROSS}bin/lld-link"

# setting flags here
# added -fstack-protector-all because fstack-protector remains not verified 
# Hardened release flags for XTLua / Windows x86_64
export CCFLAGS="-O3 \
-ffunction-sections \
-fdata-sections \
-fstack-protector-all \
-D_FORTIFY_SOURCE=3 \
-ftrivial-auto-var-init=zero \
-mguard=cf \
-fcf-protection=full \
-fdiagnostics-fixit-info"

export CXXFLAGS="$CCFLAGS"

export LDFLAGS="-s -static-libgcc \
-static-libstdc++ \
-Wl,--gc-sections"


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

cp -f build/win.xpl $HOME



# llvm-readelf --file-header --program-headers --notes --version-info --section-groups --coff-exports --coff-imports --coff-load-config $HOME/win.xpl








