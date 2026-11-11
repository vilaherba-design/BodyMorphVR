import struct,sys
d=open("/mnt/user-data/uploads/Fallout 4 VR/Fallout4VR.exe","rb").read()
off=0x3ebf310-0xF0
raw=d[off:off+0xF0]
def dec(raw,mode):
    out=bytearray(raw); key=struct.unpack_from("<I",raw,0)[0]
    for x in range(4,0xF0,4):
        v=struct.unpack_from("<I",raw,x)[0]
        struct.pack_into("<I",out,x,v^key)
        key = v if mode==0 else (v^key)
    return bytes(out)
for m in (0,1):
    h=dec(raw,m)
    print(m,"sig",hex(struct.unpack_from("<I",h,4)[0]))
    if struct.unpack_from("<I",h,4)[0]==0xC0DEC0DF:
        print(h.hex())
        if m==0: open("hdr_dec.bin","wb").write(h)
