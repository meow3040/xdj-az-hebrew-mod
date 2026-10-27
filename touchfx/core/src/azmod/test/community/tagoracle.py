# SPDX-License-Identifier: GPL-3.0-only
# Independent check of community.c's firmware tag: FNV-1a 64 over the ELF file's own PT_LOAD PF_X
# segments. Run on the host after run.sh:  python3 tagoracle.py .harness.bin  (must equal "harness tag").
import struct,sys
b=open(sys.argv[1],"rb").read(); phoff=struct.unpack_from("<Q",b,0x20)[0]; n=struct.unpack_from("<H",b,0x38)[0]
h=1469598103934665603
for i in range(n):
    t,f,o,v,pa,fs,ms,al=struct.unpack_from("<IIQQQQQQ",b,phoff+56*i)
    if t==1 and f&1:
        for c in b[o:o+fs]: h=((h^c)*1099511628211)&0xffffffffffffffff
print("ep147-%016x"%h)
