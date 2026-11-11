import struct,sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
SRC="/mnt/user-data/uploads/Fallout 4 VR/Fallout4VR.exe"
d=bytearray(open(SRC,"rb").read())
h=open("hdr_dec.bin","rb").read()
key=h[0x58:0x78]; iv=h[0x78:0x88]; stolen=h[0x88:0x98]
codeVA=struct.unpack_from("<Q",h,0x48)[0]; codeRaw=struct.unpack_from("<Q",h,0x50)[0]
oep=struct.unpack_from("<Q",h,0x20)[0]
print("codeVA",hex(codeVA),"codeRaw",hex(codeRaw),"oep",hex(oep))
ecb=Cipher(algorithms.AES(key),modes.ECB()).decryptor()
iv2=ecb.update(iv)+ecb.finalize()
TEXT_RAW=0x400
blob=bytes(stolen)+bytes(d[TEXT_RAW:TEXT_RAW+codeRaw])
n=len(blob)//16*16
dec=Cipher(algorithms.AES(key),modes.CBC(iv2)).decryptor()
out=dec.update(blob[:n])+dec.finalize()+blob[n:]
print("len",len(blob),"mod16",len(blob)%16)
def at(buf,rva): 
    o=rva-0x1000
    return buf[o:o+8].hex()
for name,buf in (("A:out[0:]",out),("B:out[16:]",out[16:])):
    print(name,"AttachSkinned 0x1CBC00:",at(buf,0x1CBC00),"Reset3D 0xDDAD60:",at(buf,0xDDAD60),"slot 0xE0A140:",at(buf,0xE0A140),"oep",at(buf,oep))
open("out_blob.bin","wb").write(out)

# ---- write a flat file-layout copy with the decrypted .text (scratch only, never redistribute)
d[TEXT_RAW:TEXT_RAW+codeRaw]=out[:codeRaw]
open("Fallout4VR_unpacked.exe","wb").write(d)
print("wrote Fallout4VR_unpacked.exe", len(d))
