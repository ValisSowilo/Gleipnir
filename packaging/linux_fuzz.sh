#!/bin/sh
# The project's own gates, run against the Linux binary.
#
# A build that compiles and round-trips one file is not a verified build.  The
# Windows binary is only trusted because gfuzz.py and tfuzz.py pass against it;
# the Linux one should clear the same bar or it does not get the same claim.
set -e
cd /root/nyx

# SRC is the repository working tree as WSL sees it, e.g. /mnt/c/src/nyx.
: "${SRC:?set SRC to the repo working tree as WSL sees it, e.g. SRC=/mnt/c/src/nyx}"

cp "$SRC/tfuzz.py" "$SRC/gfuzz.py" .
mkdir -p /tmp/fz
export TEMP=/tmp/fz

echo "=== tfuzz: round trips across thread counts ==="
python3 tfuzz.py --v2 nyx

echo
echo "=== gfuzz: corruption ==="
python3 gfuzz.py 40 --exe nyx
