"""Offline ELF64 ADRP+ADD literal-reference candidates; not function/type proof."""
import argparse
import mmap
import struct

def refs(path,offsets):
    with open(path,'rb') as stream, mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as blob:
        if blob[:6]!=b'\x7fELF\x02\x01':
            raise ValueError('expected little endian ELF64')
        phoff=struct.unpack_from('<Q',blob,32)[0]
        ents,num=struct.unpack_from('<HH',blob,54)
        segments=[struct.unpack_from('<II6Q',blob,phoff+i*ents) for i in range(num)]
        targets={}
        for off in offsets:
            for typ,flags,fo,va,pa,fs,ms,align in segments:
                if typ==1 and fo<=off<fo+fs: targets[va+off-fo]=off
        pages={x&~4095 for x in targets}
        for typ,flags,fo,va,pa,fs,ms,align in segments:
            if typ!=1 or not flags&1: continue
            for off in range(fo,fo+fs-32,4):
                ins=struct.unpack_from('<I',blob,off)[0]
                if ins&0x9f000000!=0x90000000: continue
                imm=((ins>>5)&0x7ffff)<<2|((ins>>29)&3)
                if imm&(1<<20): imm-=1<<21
                page=((va+off-fo)&~4095)+(imm<<12)
                if page not in pages: continue
                reg=ins&31
                for step in range(1,9):
                    add=struct.unpack_from('<I',blob,off+step*4)[0]
                    if add&0xffc00000==0x91000000 and (add>>5)&31==reg:
                        target=page+((add>>10)&4095)
                        if target in targets:
                            print(f'literal_file=0x{targets[target]:x} adrp=0x{va+off-fo:x} add=0x{va+off-fo+step*4:x} target_va=0x{target:x}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('elf');p.add_argument('offsets',nargs='+',type=lambda x:int(x,16))
    a=p.parse_args();refs(a.elf,a.offsets)
