"""Read a concrete 0x80dffc8 type candidate, not a proven current race owner."""
import argparse,json,struct
from pathlib import Path
from read_collision_span import read

def decode(raw,address,keys):
    values=[word^(address+i*8)^keys[i] for i,word in enumerate(struct.unpack('<3Q',raw))]
    if len(set(values))!=1:raise ValueError('encoded replicas disagree')
    value=values[0]
    return value-(1<<64) if value&(1<<63) else value

def capture(pid,base,address):
    spans={}
    def stable(a,n):
        b=read(pid,a,n)
        if read(pid,a,n)!=b:raise ValueError('changed read')
        spans[hex(a)]=b.hex();return b
    keys=[struct.unpack('<Q',stable(base+x,8))[0] for x in (0xa57e330,0xa57e348,0xa57e360)]
    h=stable(address,120)
    if struct.unpack_from('<Q',h)[0]!=base+0x80dffc8:raise ValueError('wrong candidate type')
    root,count=struct.unpack_from('<QQ',h,0x48)
    if count>256:raise ValueError('table too large for this capture')
    seen=set();rows=[]
    def walk(p):
        if not p:return
        if p in seen or len(seen)>=count:raise ValueError('tree cycle/count mismatch')
        seen.add(p);b=stable(p,64);left,right=struct.unpack_from('<QQ',b)
        walk(left)
        rows.append(dict(node=hex(p),key=struct.unpack_from('<I',b,32)[0],value=decode(b[40:64],p+40,keys)))
        walk(right)
    walk(root)
    if len(rows)!=count or any(a['key']>=b['key'] for a,b in zip(rows,rows[1:])):raise ValueError('tree ordering/count mismatch')
    if stable(address,120)!=h:raise ValueError('candidate changed')
    return dict(pid=pid,address=hex(address),table_count=count,rows=rows,
        scalar_28=decode(h[40:64],address+40,keys),
        status='type and encoded values verified; current race ownership and units unresolved',reads=spans)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('pid',type=int);p.add_argument('base',type=lambda x:int(x,16));p.add_argument('address',type=lambda x:int(x,16));p.add_argument('output',type=Path);a=p.parse_args()
    result=capture(a.pid,a.base,a.address)
    with a.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k!='reads'}))
