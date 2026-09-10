"""Huawei 439fd7f9 minimap-node capture. Reads only; never invokes getters."""
import argparse
import json
import struct
from pathlib import Path
from read_collision_span import read


def capture(pid, base, owner):
    spans={}
    def stable(a,n):
        k=(a,n)
        if k not in spans:
            x=read(pid,a,n)
            if x!=read(pid,a,n):raise ValueError('span changed')
            spans[k]=x
        return spans[k]
    def q(a):return struct.unpack('<Q',stable(a,8))[0]
    def signed(a):return struct.unpack('<q',stable(a,8))[0]
    if q(owner)!=base+0x845d6c0:raise ValueError('not the proven minimap type')
    service=q(owner+0xf0);v=q(service)
    if q(v+0x60)!=base+0x3a80d70:raise ValueError('unproven service getter')
    adjust=signed(v-0x70)
    world=q(service+adjust+0x90)
    if q(q(world)+0x60)!=base+0x59a193c:raise ValueError('unproven node getter')
    sub=world+0x148;collection=sub+signed(q(sub)-0x78)
    cv=q(collection)
    if q(cv+0x10)!=base+0x59a2e94 or q(cv+0x18)!=base+0x59a2eac:
        raise ValueError('unproven collection iterators')
    vector=q(collection+8);begin,end=struct.unpack('<2Q',stable(vector,16))
    if end<begin or (end-begin)%0x78 or end-begin>0x78000:raise ValueError('invalid node span')
    raw=stable(begin,end-begin) if end>begin else b''
    nodes=[]
    for off in range(0,len(raw),0x78):
        data=raw[off:off+0x78]
        if struct.unpack_from('<Q',data)[0]!=base+0x9e3d160:raise ValueError('unproven node type')
        nodes.append(dict(index=off//0x78,address=hex(begin+off),
            position_candidate=struct.unpack_from('<3f',data,8),
            rotation_candidate=struct.unpack_from('<4f',data,0x14),
            key=struct.unpack_from('<I',data,0x44)[0],
            size_scalars=struct.unpack_from('<2f',data,0x24),raw=data.hex()))
    # 59a1860 sorts three 0x78-stride vectors separately; names unresolved.
    node_groups=[]
    for offset in (0xe0,0x108,0x130):
        first,last,capacity=struct.unpack('<3Q',stable(world+offset,24))
        if last<first or capacity<last or (last-first)%0x78 or last-first>0x78000:
            raise ValueError('invalid related node vector')
        group_raw=stable(first,last-first) if last>first else b''
        group=[]
        for off in range(0,len(group_raw),0x78):
            data=group_raw[off:off+0x78]
            if struct.unpack_from('<Q',data)[0]!=base+0x9e3d160:
                raise ValueError('unproven related node type')
            group.append(dict(index=off//0x78,address=hex(first+off),
                position=struct.unpack_from('<3f',data,8),rotation=struct.unpack_from('<4f',data,0x14),
                raw_indices_3c_40_44=struct.unpack_from('<3I',data,0x3c),raw=data.hex()))
        node_groups.append(dict(vector_offset=hex(offset),begin=hex(first),end=hex(last),
                                count=len(group),nodes=group))
    # 3baed44..3baed8c uses world.virtual48 as the fallback finish-icon node.
    if q(q(world)+0x48)!=base+0x59a18f8:raise ValueError('unproven finish getter')
    finish=stable(world+0x1a0,0x78)
    if struct.unpack_from('<Q',finish)[0]!=base+0x9e3d160:raise ValueError('unproven finish node type')
    finish_position=struct.unpack_from('<3f',finish,8)
    finish_rotation=struct.unpack_from('<4f',finish,0x14)
    finish_key=struct.unpack_from('<I',finish,0x44)[0]
    matching=[n['index'] for n in nodes if n['position_candidate']==finish_position
              and n['rotation_candidate']==finish_rotation and n['key']==finish_key]
    icons={}
    for name,off in [('finish',0xa70),('checkpoint',0xa88)]:
        begin_icons,end_icons=struct.unpack('<2Q',stable(owner+off,16))
        if end_icons<begin_icons or (end_icons-begin_icons)%8 or end_icons-begin_icons>8192:
            raise ValueError('invalid icon vector')
        entries=[]
        for loc in range(begin_icons,end_icons,8):
            icon=q(loc)
            entries.append(dict(address=hex(icon),position=struct.unpack('<3f',stable(icon+0xe4,12)),
                rotation=struct.unpack('<4f',stable(icon+0xf0,16))))
        icons[name]=entries
    return dict(pid=pid,base=hex(base),owner=hex(owner),service=hex(service),
        service_adjustment=adjust,world=hex(world),collection=hex(collection),
        vector=hex(vector),begin=hex(begin),end=hex(end),count=len(nodes),nodes=nodes,
        fallback_finish=dict(address=hex(world+0x1a0),position=finish_position,
            rotation=finish_rotation,key=finish_key,matching_node_indices=matching,raw=finish.hex()),
        current_icons=icons,node_groups=node_groups,
        raw_spans=[dict(address=hex(a),length=n,hex=x.hex()) for (a,n),x in spans.items()],
        game_writes=0,method_calls=0,
        limitation='Current minimap source nodes, not a proven complete checkpoint/trigger list. Position and quaternion interpretation awaits projection/geometry validation. Double reads are not an atomic snapshot.')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('pid',type=int)
    p.add_argument('base',type=lambda x:int(x,16));p.add_argument('owner',type=lambda x:int(x,16))
    p.add_argument('output',type=Path);a=p.parse_args();r=capture(a.pid,a.base,a.owner)
    with a.output.open('x',encoding='utf-8') as f:json.dump(r,f,indent=2,allow_nan=False)
    print(json.dumps({k:v for k,v in r.items() if k!='raw_spans'},indent=2))
