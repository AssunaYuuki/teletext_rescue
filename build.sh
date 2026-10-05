#!/bin/sh
# Сборка Teletext Rescue под Linux и macOS: trcli — все декодеры, отчёты и HTML из командной строки.
# Окна программы — только под Windows (build.bat).
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
cmake -S cpp -B "$BUILD" $GEN -DCMAKE_BUILD_TYPE=Release $ARCH
cmake --build "$BUILD" -j "$JOBS"

echo
echo "Done: $BUILD/trcli"
"$BUILD/trcli" devices || true
echo
echo "Examples:"
echo "  $BUILD/trcli vbi recording.vbi          # examine and decode everything readable"
echo "  $BUILD/trcli vbi recording.vbi --cpu    # the same on the processor"
echo "  $BUILD/trcli build stream.t42 project   # pages from a .t42 stream"
echo "  $BUILD/trcli export project             # output.t42 + HTML pages"
