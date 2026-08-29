#!/bin/sh
# Cross-platform verification for Nyx, run inside WSL.
#
# The README claims an archive written on one platform reads on the other.
# That is exactly the sort of claim that breaks quietly on a struct-padding or
# endianness assumption and is only discovered by whoever needed the restore,
# so it is tested rather than asserted.
set -e
cd /root/nyx

echo "=== Linux nyx reading a Windows-written archive ==="
# SRC is the repository working tree as WSL sees it, e.g. /mnt/c/src/nyx.
: "${SRC:?set SRC to the repo working tree as WSL sees it, e.g. SRC=/mnt/c/src/nyx}"

cp "$SRC/win-written.nyx" .
./nyx t win-written.nyx
rm -rf wr && mkdir wr
./nyx x -q win-written.nyx wr
find wr -type f | sort
sha256sum wr/code.c

echo
echo "=== do both platforms produce the SAME archive from the same input? ==="
rm -rf same && mkdir same
cp nyx.c same/code.c
cp Makefile same/build.mk
./nyx c -5 -q linux-same.nyx same
sha256sum linux-same.nyx win-written.nyx
