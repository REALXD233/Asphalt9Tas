"""Bounded, read-only listener-chain evidence. No callback execution."""
import argparse
import collections
import json
import struct
from pathlib import Path
from read_collision_span import read

def capture(census,base,limit):
    pid=census['pid'];rows=[]
    group=next(x for x in census['array_candidates'] if x['offset']==0x160)
    for i,item in enumerate(group['heads'][:limit]):
        wrapper=int(item['pointer'],16)
        head=read(pid,wrapper,32)
        if head!=bytes.fromhex(item['head_hex'])[:32]:
            raise ValueError('wrapper no longer matches census')
        node=struct.unpack_from('<Q',head,8)[0]
        if node==wrapper+8:
            rows.append(dict(member=i,empty=True));continue
        listener=node-16
        data=read(pid,listener,112)
        owner=struct.unpack_from('<Q',data,0x48)[0]
        owner_head=read(pid,owner,80)
        if read(pid,listener,112)!=data or read(pid,owner,80)!=owner_head:
            raise ValueError('listener changed')
        vptr=struct.unpack_from('<Q',owner_head)[0]
        rows.append(dict(member=i,wrapper=hex(wrapper),listener=hex(listener),
            listener_head=data.hex(),owner=hex(owner),owner_head=owner_head.hex(),
            owner_vtable_rva=hex(vptr-base),
            owner_wrapper_backref_matches=struct.unpack_from('<Q',owner_head,32)[0]==wrapper,
            limitation='first listener only; role is not classified'))
    return dict(pid=pid,count=len(rows),
        owner_vtable_groups=dict(collections.Counter(x['owner_vtable_rva'] for x in rows if 'owner_vtable_rva' in x)),
        rows=rows)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('census',type=Path);p.add_argument('base',type=lambda s:int(s,16))
    p.add_argument('output',type=Path);p.add_argument('--limit',type=int,default=16)
    a=p.parse_args()
    if not 1<=a.limit<=136: p.error('limit must be 1..136')
    r=capture(json.loads(a.census.read_text(encoding='utf-8-sig')),a.base,a.limit)
    with a.output.open('x',encoding='utf-8') as f:json.dump(r,f,indent=2)
    print(json.dumps({k:v for k,v in r.items() if k!='rows'}))
