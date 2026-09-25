#/bin/bash

kopt="${-}"

set +u
set -e

if [ -z "${MINGW_HOME}" ]; then
	MINGW_HOME="/data/data/com.termux/files/home/mingw-gcc-cross"
fi

set -u

CROSS_COMPILE_TRIPLET='x86_64-w64-mingw32'
CROSS_COMPILE_SYSTEM='windows'
CROSS_COMPILE_ARCHITECTURE='x86_64'
CROSS_COMPILE_SYSROOT="${MINGW_HOME}/${CROSS_COMPILE_TRIPLET}-ucrt"



CC="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-clang"
CXX="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-clang++"
RC="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-windres"
WINDRES="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-windres"
DLLTOOL="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-dlltool"
AR="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-ar"
AS="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-as"
LD="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-ld"
NM="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-nm"
RANLIB="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-ranlib"
STRIP="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-strip"
OBJCOPY="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-objcopy"
OBJDUMP="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-objdump"
READELF="${MINGW_HOME}/bin/${CROSS_COMPILE_TRIPLET}-readelf"
YASM="${MINGW_HOME}/bin/yasm"

export \
	CROSS_COMPILE_TRIPLET \
	CROSS_COMPILE_SYSTEM \
	CROSS_COMPILE_ARCHITECTURE \
	CROSS_COMPILE_SYSROOT \
	CMAKE_TOOLCHAIN_FILE \
	CC \
	CXX \
	RC \
	WINDRES \
	DLLTOOL \
	AR \
	AS \
	LD \
	NM \
	RANLIB \
	STRIP \
	OBJCOPY \
	OBJDUMP \
	READELF \
	YASM


ROOT=$(pwd)
BUILD="$ROOT/build"

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




