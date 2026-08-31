#!/bin/sh
# Same size, almost every byte different -- that pattern says the same members
# were stored in a different ORDER, not that they were compressed differently.
# Each member starts its own segment, so reordering rearranges the file without
# changing its length.
set -e
cd /root/gleipnir

echo "member order, Linux-written:"
./gleipnir l linux-same.gl | sed -n '2,4p'
echo
echo "member order, Windows-written:"
./gleipnir l win-written.gl | sed -n '2,4p'
echo
echo "per-member compressed sizes should match across platforms if the model"
echo "behaves identically; only their arrangement in the file differs."
