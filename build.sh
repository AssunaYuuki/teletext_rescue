#!/bin/sh
# Сборка Teletext Rescue под Linux и macOS: программа с окнами (SDL3 + Dear ImGui) и trcli.
# Под Windows — build.bat.
#
#   ./build.sh            сборка в cpp/build-<система>/
#   ./build.sh clean      то же с чистого листа
#
# Нужно: компилятор C++20 (g++ 10+ или clang 12+ / Xcode 13+), CMake 3.20+.
# Для расчёта на видеокарте во время работы: OpenCL (Linux — ocl-icd и драйвер видеокарты;
# macOS — встроен в систему). Без него всё считается на процессоре.
set -e
cd "$(dirname "$0")"
OS=$(uname -s | tr 'A-Z' 'a-z')
BUILD="cpp/build-$OS"

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake not found. Install it first:"
    echo "  Debian/Ubuntu: sudo apt install cmake g++"
    echo "  Fedora:        sudo dnf install cmake gcc-c++"
    echo "  macOS:         xcode-select --install && brew install cmake"
    exit 1
fi
[ "$1" = "clean" ] && rm -rf "$BUILD"

GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
if [ "$OS" = "darwin" ]; then JOBS=$(sysctl -n hw.ncpu); else JOBS=$(nproc 2>/dev/null || echo 4); fi

# macOS: один файл и для Apple Silicon, и для Intel
ARCH=""
[ "$OS" = "darwin" ] && ARCH="-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
# Linux 32-bit (x86) on a 64-bit machine: TR_ARCH=i686 ./build.sh  (needs gcc-multilib and the :i386 libraries)
if [ "$OS" = "linux" ] && [ "$TR_ARCH" = "i686" ]; then
    BUILD="cpp/build-linux-i686"
    export PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig
    ARCH="-DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 -DCMAKE_SYSTEM_PROCESSOR=i686 -DSDL_WAYLAND=OFF"
    [ "$1" = "clean" ] && rm -rf "$BUILD"
fi
cmake -S cpp -B "$BUILD" $GEN -DCMAKE_BUILD_TYPE=Release $ARCH
if [ -n "$GEN" ]; then cmake --build "$BUILD" -j "$JOBS" -- -k 0; else cmake --build "$BUILD" -j "$JOBS" -- -k; fi

echo
if [ "$OS" = "darwin" ]; then echo "Done: $BUILD/TeletextRescue.app and $BUILD/trcli"; else echo "Done: $BUILD/TeletextRescue and $BUILD/trcli"; fi
"$BUILD/trcli" || true
echo
echo "Examples:"
echo "  $BUILD/trcli recording.vbi --export out   # decode, write everything into the folder out"
