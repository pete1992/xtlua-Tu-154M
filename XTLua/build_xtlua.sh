#!/data/data/com.termux/files/usr/bin/bash


set -e
# we have 2 working windows crosscompiler at this point
# LLVM - CLANG
# GCC - mingw
export CROSS="/data/data/com.termux/files/home/mingw-gcc-cross/bin"
# export this mess xD
# yes we export both of them 
export PATH="$CROSS:$PATH" 
#export PATH="/data/data/com.termux/files/home/CROSS/bin:$PATH"

echo "=== xtlua Windows x64 Build ==="

ROOT=$(pwd)
BUILD="$ROOT/build"

export PLATFORM="x86_64-w64-mingw32-"
export CC="x86_64-w64-mingw32-clang"
export CXX="x86_64-w64-mingw32-clang++"
export AR="${CROSS}/llvm-ar"
export AS="${CROSS}/llvm-as"
export RANLIB="${CROSS}/llvm-ranlib"
export RC="${CROSS}/llvm-rc"
# working include dir, don't change
export INCLUDEDIR="${CROSS}include"
export NM="${CROSS}/llvm-nm"
export STRIP="${CROSS}/llvm-strip"
export OBJDUMP="${CROSS}/llvm-objdump"
export READELF="${CROSS}/llvm-readelf"
# if we want to set flags, we better control which Linker is used
export LD="${CROSS}bin/lld-link"

export CCFLAGS="-O3 -flto -mstackrealign \
-ffunction-sections \
-fdata-sections \
-fwinx64-eh-unwind=v2-best-effort \
-fstack-protector-all -fstack-protector-strong -mlvi-hardening \
-D_FORTIFY_SOURCE=3 \
-mguard=cf \
-fcf-protection=full \
-fms-extensions \
-fdiagnostics-fixit-info"

export CXXFLAGS="$CCFLAGS"

export LDFLAGS="-s -fuse-ld=lld -flto -static-libgcc \
-static-libstdc++ -Wl,--gc-sections"


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
-DCMAKE_EXE_LINKER_FLAGS="$CCFLAGS $LDFLAGS" \
-DCMAKE_SHARED_LINKER_FLAGS="$CCFLAGS $LDFLAGS"

echo "Watch the Configuration"
echo " ";
sleep 20;

echo "=== Build ==="

cmake --build "$BUILD" -j8

echo "=== Finished ==="

cp -f build/win.xpl $HOME

# Setup the environment for cross-compilation

  
  

# llvm-readelf --file-header --program-headers --notes --version-info --section-groups --coff-exports --coff-imports --coff-load-config win.xpl








