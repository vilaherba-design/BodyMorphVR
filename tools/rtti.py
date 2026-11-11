import sys,struct
from pe import *
im=Img()
text=im.sec(".text"); rd=im.sec(".rdata"); dat=im.sec(".data")
def vtable_for(cls):
    name=(".?AV%s@@"%cls).encode()+b"\0"
    res=[]
    for o in findall(im.d,name):
        td=im.o2r(o-0x10)
        if td is None: continue
        # COLs referencing td rva at +0xC, in .rdata
        tdb=struct.pack("<I",td)
        for c in findall(im.d,tdb,rd[3],rd[3]+rd[4]):
            col_o=c-0xC
            if col_o<rd[3]: continue
            sig,off,cd,tdr,chd=struct.unpack_from("<IIIII",im.d,col_o)
            if sig!=1 or tdr!=td: continue
            colva=BASE+im.o2r(col_o)
            for v in findall(im.d,struct.pack("<Q",colva),rd[3],rd[3]+rd[4]):
                res.append((off,im.o2r(v+8)))
        res.append(("TD",td))
    return res
if __name__=="__main__":
    for c in sys.argv[1:]:
        print(c,[(a if isinstance(a,str) else a, hex(b)) for a,b in vtable_for(c)])
