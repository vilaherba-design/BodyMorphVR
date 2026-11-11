import struct, mmap
BASE=0x140000000
class Img:
    def __init__(s,path="Fallout4VR_unpacked.exe"):
        s.d=open(path,"rb").read()
        pe=struct.unpack_from("<I",s.d,0x3c)[0]
        n=struct.unpack_from("<H",s.d,pe+6)[0]; opt=struct.unpack_from("<H",s.d,pe+20)[0]
        so=pe+24+opt; s.secs=[]
        for i in range(n):
            nm,vs,va,rs,rp=struct.unpack_from("<8sIIII",s.d,so+40*i)
            s.secs.append((nm.rstrip(b"\0").decode(),va,vs,rp,rs))
    def r2o(s,rva):
        for nm,va,vs,rp,rs in s.secs:
            if va<=rva<va+max(vs,rs): return rp+rva-va
        return None
    def o2r(s,off):
        for nm,va,vs,rp,rs in s.secs:
            if rp<=off<rp+rs: return va+off-rp
        return None
    def rd(s,rva,n):
        o=s.r2o(rva); return s.d[o:o+n]
    def q(s,rva): return struct.unpack("<Q",s.rd(rva,8))[0]
    def u32(s,rva): return struct.unpack("<I",s.rd(rva,4))[0]
    def sec(s,name):
        for x in s.secs:
            if x[0]==name: return x
def findall(d,pat,start=0,end=None):
    i=start
    while True:
        i=d.find(pat,i,end)
        if i<0: return
        yield i; i+=1
