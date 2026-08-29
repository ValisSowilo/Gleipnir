#!/bin/sh
# Same size, almost every byte different -- that pattern says the same members
# were stored in a different ORDER, not that they were compressed differently.
# Each member starts its own segment, so reordering rearranges the file without
# changing its length.
set -e
cd /root/nyx

echo "member order, Linux-written:"
./nyx l linux-same.nyx | sed -n '2,4p'
echo
echo "member order, Windows-written:"
./nyx l win-written.nyx | sed -n '2,4p'
echo
echo "per-member compressed sizes should match across platforms if the model"
echo "behaves identically; only their arrangement in the file differs."
