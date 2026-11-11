import sys
from capstone import *
from pe import *
im=Img()
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=False
def dis(rva,n=0x100,stop_ret=True):
    o=im.r2o(rva); code=im.d[o:o+n]
    for i in md.disasm(code,BASE+rva):
        print("%x  %-8s %s"%(i.address-BASE,i.mnemonic,i.op_str))
        if stop_ret and i.mnemonic=="ret": break
if __name__=="__main__":
    dis(int(sys.argv[1],16),int(sys.argv[2],16) if len(sys.argv)>2 else 0x100, False)
