"""Split tagged material batches into selectable connected surfaces, offline."""
import argparse,copy,json,struct
from pathlib import Path
from analyze_material_components import components
from track_collision_export import Track,IDENTITY,export

def split(blob):
    t=Track(blob);m=copy.deepcopy(t.manifest);payload=bytearray(t.payload);objects=[]
    for original,o in enumerate(t.objects):
        if not o.get('NativeAttributes',{}).get('attributes'):
            objects.append(o);continue
        ts=list(t.triangles(o['ShapeIndex'],transforms=(o['Trans'],)))
        for n,part in enumerate(components([v for v,_ in ts])):
            start=len(payload)
            for i in part['triangle_indices']:
                vs,mat=ts[i];payload.extend(struct.pack('<9fi',*(x for v in vs for x in v),mat))
            m['Shapes'].append(dict(Type=26,ByteOffset=start,ByteLength=len(payload)-start))
            obj=copy.deepcopy(o);obj.update(ShapeIndex=len(m['Shapes'])-1,Trans=IDENTITY)
            obj['ConnectedSurface']=dict(parent_display_object=original,component=n,
                bounds=part['bounds'],area=part['area'],not_gameplay_region_id=True)
            objects.append(obj)
    m['Objects']=objects;m['Meta']['ComponentRule']='shared edge rounded to 4 decimal places; disconnected per material batch; not gameplay trigger IDs'
    raw=json.dumps(m).encode()
    return b'BCOL'+struct.pack('<I',len(raw))+raw+struct.pack('<I',len(payload))+payload

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    blob=split(a.source.read_bytes());a.output.mkdir(parents=True,exist_ok=False)
    path=a.output/'connected-surfaces.TRACK';path.write_bytes(blob)
    print('triangles',export(path,a.output/'viewer'))
