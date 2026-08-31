#!/bin/sh
# Build a distributable gleipnir.exe.
#
# Three things matter here and none of them are obvious, so they are spelled
# out rather than buried in flags:
#
#   -march=x86-64-v2   NOT -march=native.  A native build targets whatever CPU
#                      compiled it and dies with an illegal-instruction fault
#                      on anything older.  v2 is SSE4.2/POPCNT, which is every
#                      x86-64 machine since roughly 2009.  The development
#                      builds in this tree use -march=native and must never be
#                      handed to anyone.
#
#   -ffp-contract=off  detect_period() measures mean absolute difference in
#                      double precision, and the stride it picks changes what
#                      the model does.  Letting the compiler fuse multiply-add
#                      makes that arithmetic depend on the target CPU, which
#                      would let two builds disagree about a file.  Verified:
#                      with this flag, native and portable builds produce
#                      byte-identical archives on x-ray, mr, sao and dickens
#                      -- the first three being the files where a period is
#                      actually detected.
#
#   -static            MinGW otherwise leaves libgcc_s_seh-1.dll and
#                      libwinpthread-1.dll as runtime dependencies, and a
#                      compressor that will not start is worse than a slow one.
#                      After this the only imports are KERNEL32 and the UCRT,
#                      which ships with Windows 10 and later.
#
#   sh build.sh [output-name]

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ZL="$HERE/tools/zlib-1.3.1"
OUT="${1:-gleipnir.exe}"
SRC="$HERE/gleipnir.c"

if [ ! -f "$ZL/libz.a" ]; then
    echo "build.sh: $ZL/libz.a not found." >&2
    echo "  zlib is needed for DEFLATE stream recovery.  Build it with:" >&2
    echo "    cd tools/zlib-1.3.1 && ./configure && make" >&2
    exit 1
fi

echo "compiling $(basename "$SRC") -> $OUT"
gcc -O3 -funroll-loops \
    -march=x86-64-v2 \
    -ffp-contract=off \
    -static \
    -I "$ZL" \
    -o "$HERE/$OUT" "$SRC" "$ZL/libz.a" -lm

echo "checking it starts and reports a version"
"$HERE/$OUT" --version

echo
echo "checking it has no non-system runtime dependencies"
if objdump -p "$HERE/$OUT" 2>/dev/null | grep -iE 'libgcc|winpthread|libstdc'; then
    echo "  WARNING: the above DLLs must ship alongside the exe" >&2
    exit 1
fi
echo "  ok: KERNEL32 + UCRT only"

echo
echo "round-trip check"
TMPD="${TMPDIR:-${TEMP:-/tmp}}/genbuild.$$"
mkdir -p "$TMPD/out"
# Compress this script and the source, so the check exercises both a small
# text file and a large one, then compare byte for byte.
cp "$0" "$TMPD/small.txt"
cp "$SRC" "$TMPD/large.c"
"$HERE/$OUT" c -3 -t1 -q "$TMPD/a.gen" "$TMPD/small.txt" "$TMPD/large.c"
"$HERE/$OUT" t "$TMPD/a.gen" >/dev/null
"$HERE/$OUT" d -t1 -q "$TMPD/a.gen" "$TMPD/out"
for f in small.txt large.c; do
    if cmp -s "$TMPD/$f" "$TMPD/out/$f"; then
        echo "  $f round-tripped"
    else
        echo "  $f MISMATCH" >&2
        exit 1
    fi
done
rm -rf "$TMPD"

echo
ls -l "$HERE/$OUT" | awk '{print "  " $5 " bytes  " $NF}'
echo "done."
