#!/bin/sh
# Why do a Linux-written and a Windows-written archive of the same bytes differ?
#
# Expected answer: only the metadata the format stores per member -- mtime and
# permission bits -- not the compressed payload.  If the payloads differ, the
# model behaves differently across platforms and that is a real problem.
set -e
cd /root/nyx

echo "sizes:"
stat -c '  %s  %n' linux-same.nyx win-written.nyx

echo
echo "first differing byte:"
cmp linux-same.nyx win-written.nyx || true

echo
echo "how many bytes differ in total:"
cmp -l linux-same.nyx win-written.nyx | wc -l

echo
echo "payload region (past the 48-byte header, excluding the index+trailer):"
# The index lives at the end and holds names, mtimes and modes.  The segment
# payload sits between the header and the index, so compare that region alone.
SZ=$(stat -c %s linux-same.nyx)
IDX=$((SZ - 4096))
dd if=linux-same.nyx bs=1 skip=48 count=$((IDX - 48)) 2>/dev/null | sha256sum
dd if=win-written.nyx bs=1 skip=48 count=$((IDX - 48)) 2>/dev/null | sha256sum
