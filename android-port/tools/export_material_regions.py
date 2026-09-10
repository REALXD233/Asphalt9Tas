"""Offline material-region view from Android tables; original .TRACK stays intact."""
import argparse,collections,json,struct
from pathlib import Path
from track_collision_export import Track,IDENTITY,export

NAMES=['ramp','magnet','highjump','wreck','respawn','nochassis']
def build(source):
    t=Track(source.read_bytes());payload=bytearray();shapes=[];objects=[];counts=collections.Counter()
    def visit(index,trans,source_id,path):
        s=t.shapes[index];kind=s['Type']
        if kind==31:
            for n,c in enumerate(s['Children']):visit(c['ChildIndex'],(*trans,c['LocalTrans']),source_id,(*path,n))
            return
        if kind==22:
            scale=IDENTITY.copy();scale[0],scale[5],scale[10]=s['LocalScale']
            visit(s['ChildIndex'],(*trans,scale),source_id,path);return
        tables=s.get('NativeMaterialTables',[])
        if len(tables)>1:raise ValueError('multi-subpart material scoping needs explicit ranges')
        table=bytes.fromhex(tables[0]['RawTable']) if tables else b''
        buckets=collections.defaultdict(list)
        for vertices,mat in t.triangles(index,transforms=trans):buckets[mat].append(vertices)
        for mat,triangles in buckets.items():
            attrs={};packed=None
            if tables and mat>=0:
                if tables[0]['Stride']!=16:raise ValueError('unproven packed material stride')
                packed=struct.unpack_from('<I',table,mat*16+8)[0]
                tags=[name for bit,name in enumerate(NAMES) if packed & (1<<(16+bit))]
                attrs=dict(status='decoded',packed=packed,attributes=tags,
                           category_raw=packed&255,secondary_raw=(packed>>8)&255)
                for tag in tags:counts[tag]+=len(triangles)
            offset=len(payload)
            for vs in triangles:payload.extend(struct.pack('<9fi',*(x for v in vs for x in v),mat))
            shapes.append(dict(Type=26,ByteOffset=offset,ByteLength=len(payload)-offset))
            objects.append(dict(ShapeIndex=len(shapes)-1,Trans=IDENTITY,ColFlags=t.objects[source_id]['ColFlags'],
                NativeAttributes=attrs,Source=dict(original_object=source_id,child_path=path,material_index=mat)))
    for i,o in enumerate(t.objects):visit(o['ShapeIndex'],(o['Trans'],),i,())
    manifest=dict(Meta=dict(Version=1,AttributeEvidence=dict(source=str(source),attribute_triangle_counts=dict(counts),
        limitation='material attribute names, not proven gameplay outcomes; primitives tessellated; checkpoint regions unresolved')),
        Shapes=shapes,Objects=objects)
    raw=json.dumps(manifest).encode()
    return b'BCOL'+struct.pack('<I',len(raw))+raw+struct.pack('<I',len(payload))+payload,dict(counts)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();blob,counts=build(a.source);a.output.mkdir(exist_ok=False,parents=True)
    track=a.output/'material-regions.TRACK';track.write_bytes(blob)
    print(dict(attribute_triangle_counts=counts,triangles=export(track,a.output/'viewer')))
