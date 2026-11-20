import sys
sys.path.insert(0,'tools')
from ponelf import Ko
from capstone import *
from capstone.arm64 import *
ko=Ko(sys.argv[1]); sec=sys.argv[2]; start=int(sys.argv[3],0); size=int(sys.argv[4],0)
md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN); md.detail=True
code=ko.bytes_of(sec)
for ins in md.disasm(code[start:start+size],start):
    print('0x%x: %-9s %s'%(ins.address,ins.mnemonic,ins.op_str))
