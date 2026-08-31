"""Parse the block table straight out of a gen2x container -- ground truth,
no instrumentation in the compressor to get wrong."""
import os, struct, collections, sys
d=open(sys.argv[1],'rb').read(); o=0
if d[o]!=1: print("stored container"); raise SystemExit
o+=1
n=struct.unpack_from('<Q',d,o)[0]; o+=8
o+=3; nsym=d[o]; o+=1; o+=nsym
wn=struct.unpack_from('<Q',d,o)[0]; o+=8
nd=struct.unpack_from('<I',d,o)[0]; o+=4; o+=nd*19
nchunk=struct.unpack_from('<I',d,o)[0]; o+=4
print(f"n={n:,} expanded={wn:,} deflate={nd} chunks={nchunk}")
KIND={0:'model',1:'x86',2:'store',3:'alpha'}
tot=collections.Counter(); totb=collections.Counter()
for c in range(nchunk):
    cl,al,sl=struct.unpack_from('<QQQ',d,o); o+=24
    nb=struct.unpack_from('<I',d,o)[0]; o+=4
    for b in range(nb):
        t=d[o]; o+=1; ln=struct.unpack_from('<I',d,o)[0]; o+=4
        tot[t&3]+=1; totb[t&3]+=ln
for k in sorted(tot):
    print(f"  {KIND[k]:<6} {tot[k]:>5} blocks {totb[k]:>12,} bytes  ({100*totb[k]/wn:.1f}%)")
