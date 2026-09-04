#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build/c68k"
C68K_DIR="${SCRIPT_DIR}/../Core/c68k"

mkdir -p "$BUILD_DIR"

echo "Building c68k static library..."

# Compile each source file individually
for src in "$C68K_DIR"/*.c; do
    name=$(basename "$src" .c)
    clang -c -arch arm64 -O2 \
        -I"$C68K_DIR" \
        -DTARGET_OS_MAC=1 \
        -DTARGET_CPU_ARM64=1 \
        "$src" \
        -o "$BUILD_DIR/${name}.o"
done

# Create static library
libtool -static -o "$BUILD_DIR/libc68k.a" "$BUILD_DIR"/*.o

echo "Done: $BUILD_DIR/libc68k.a"
