#!/bin/sh
# Cross-platform verification for Gleipnir, run inside WSL.
#
# The README claims an archive written on one platform reads on the other.
# That is exactly the sort of claim that breaks quietly on a struct-padding or
# endianness assumption and is only discovered by whoever needed the restore,
# so it is tested rather than asserted.
set -e
cd /root/gleipnir

echo "=== Linux gleipnir reading a Windows-written archive ==="
# SRC is the repository working tree as WSL sees it, e.g. /mnt/c/src/gleipnir.
: "${SRC:?set SRC to the repo working tree as WSL sees it, e.g. SRC=/mnt/c/src/gleipnir}"

cp "$SRC/win-written.gl" .
./gleipnir t win-written.gl
rm -rf wr && mkdir wr
./gleipnir x -q win-written.gl wr
find wr -type f | sort
sha256sum wr/code.c

echo
echo "=== do both platforms produce the SAME archive from the same input? ==="
rm -rf same && mkdir same
cp gleipnir.c same/code.c
cp Makefile same/build.mk
./gleipnir c -5 -q linux-same.gl same
sha256sum linux-same.gl win-written.gl
