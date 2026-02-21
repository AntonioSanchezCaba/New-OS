#!/usr/bin/env bash
##############################################################################
# build_cross.sh - Build an x86_64-elf GCC cross-compiler
#
# This script downloads and builds binutils and GCC targeting x86_64-elf.
# The result is installed in $PREFIX (default: $HOME/cross).
#
# Usage: bash scripts/build_cross.sh
#        PREFIX=/opt/cross bash scripts/build_cross.sh
#
# Requirements (Ubuntu/Debian):
#   sudo apt install build-essential bison flex libgmp-dev libmpc-dev \
#                    libmpfr-dev texinfo libisl-dev
#
# Requirements (macOS with Homebrew):
#   brew install gmp mpfr libmpc
##############################################################################

set -e

TARGET=x86_64-elf
PREFIX="${PREFIX:-$HOME/cross}"
export PATH="$PREFIX/bin:$PATH"

BINUTILS_VERSION=2.41
GCC_VERSION=13.2.0

BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.gz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.gz"

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "=== NovOS Cross-Compiler Build ==="
echo "Target:   $TARGET"
echo "Prefix:   $PREFIX"
echo "Jobs:     $JOBS"
echo

mkdir -p "$HOME/cross-src"
cd "$HOME/cross-src"

# Download and build binutils
if [ ! -f "binutils-${BINUTILS_VERSION}.tar.gz" ]; then
    echo "Downloading binutils $BINUTILS_VERSION..."
    curl -LO "$BINUTILS_URL"
fi

if [ ! -d "binutils-${BINUTILS_VERSION}" ]; then
    tar xzf "binutils-${BINUTILS_VERSION}.tar.gz"
fi

echo "Building binutils..."
mkdir -p build-binutils
cd build-binutils
../binutils-${BINUTILS_VERSION}/configure \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --with-sysroot \
    --disable-nls \
    --disable-werror
make -j"$JOBS"
make install
cd ..

# Download and build GCC (C only)
if [ ! -f "gcc-${GCC_VERSION}.tar.gz" ]; then
    echo "Downloading GCC $GCC_VERSION..."
    curl -LO "$GCC_URL"
fi

if [ ! -d "gcc-${GCC_VERSION}" ]; then
    tar xzf "gcc-${GCC_VERSION}.tar.gz"
fi

echo "Building GCC (this takes a while)..."
mkdir -p build-gcc
cd build-gcc
../gcc-${GCC_VERSION}/configure \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --disable-nls \
    --enable-languages=c,c++ \
    --without-headers
make -j"$JOBS" all-gcc
make -j"$JOBS" all-target-libgcc
make install-gcc
make install-target-libgcc
cd ..

echo
echo "=== Cross-compiler installed to $PREFIX ==="
echo "Add to your PATH:"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
echo
echo "Test with:"
echo "  x86_64-elf-gcc --version"
