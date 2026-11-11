import re,struct,sys,pickle,os
from pe import *
im=Img()
t=im.sec(".text"); to=t[3]; tsz=t[4]
def build():
    # all rip-relative lea targets: dict target_rva -> [insn_rva]
    pat=re.compile(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]',re.S)
    tgt={}
    buf=im.d[to:to+tsz]
    for m in pat.finditer(buf):
        i=m.start()
        if i+7>len(buf): continue
        disp=struct.unpack_from("<i",buf,i+3)[0]
        insn=t[1]+i
        tgt.setdefault(insn+7+disp,[]).append(insn)
    return tgt
if os.path.exists("leaxref.pkl"): tgt=pickle.load(open("leaxref.pkl","rb"))
else:
    tgt=build(); pickle.dump(tgt,open("leaxref.pkl","wb"))
if __name__=="__main__":
    for a in sys.argv[1:]:
        r=int(a,16); print(hex(r),[hex(x) for x in tgt.get(r,[])])
