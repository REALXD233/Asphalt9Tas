"""Extract multiset differences in a selected mesh pair, without altering inputs."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct
from compare_collision_tracks import key
from track_collision_export import Track, IDENTITY, export


def subtract(a, b):
    remaining = Counter((key(v, 3), m) for v, m in b)
    result = []
    for vertices, material in a:
        k = key(vertices, 3), material
        if remaining[k]: remaining[k] -= 1
        else: result.append((vertices, material))
    return result


def classify(a, b):
    da, db = subtract(a,b), subtract(b,a)
    ga, gb = Counter(key(v,3) for v,_ in da), Counter(key(v,3) for v,_ in db)
    return da, db, sum((ga & gb).values())


def emit(triangles, folder, label):
    payload=bytearray(); objects=[]; shapes=[]
    buckets=defaultdict(list)
    for v,m in triangles: buckets[m].append(v)
    for m, values in sorted(buckets.items()):
        start=len(payload)
        for vs in values: payload.extend(struct.pack('<9fi',*(c for v in vs for c in v),m))
        shapes.append(dict(Type=26,ByteOffset=start,ByteLength=len(payload)-start))
        objects.append(dict(ShapeIndex=len(shapes)-1,Trans=IDENTITY,ColFlags=0,
                            Source=dict(comparison_only=True,material=m)))
    manifest=dict(Meta=dict(Version=1),Shapes=shapes,Objects=objects)
    header=json.dumps(manifest).encode()
    track=folder/'difference.TRACK'
    track.write_bytes(b'BCOL'+struct.pack('<I',len(header))+header+struct.pack('<I',len(payload))+payload)
    export(track,folder/'viewer')
    page=folder/'viewer/viewer.html'
    text=page.read_text(encoding='utf-8')
    page.write_text(text.replace('上游历史样本 · 非安卓实时地图',label),encoding='utf-8')


def run(a,b,ai,bi,out):
    def load(path,index):
        raw=path.read_bytes(); t=Track(raw); obj=t.objects[index]
        return list(t.triangles(obj['ShapeIndex'],transforms=(obj['Trans'],))),hashlib.sha256(raw).hexdigest()
    ta,ha=load(a,ai); tb,hb=load(b,bi)
    da,db,material=classify(ta,tb)
    out.mkdir(parents=True,exist_ok=False)
    for name,tris,label in [('upstream-only',da,'对照差异：上游未匹配面 · 非完整地图'),
                            ('android-only',db,'对照差异：安卓未匹配面 · 非完整地图')]:
        folder=out/name; folder.mkdir();emit(tris,folder,label)
    def details(tris):
        return [dict(vertices=v,material=m,center=[sum(p[i] for p in v)/3 for i in range(3)]) for v,m in tris]
    report=dict(a=dict(path=str(a),sha256=ha,index=ai,total=len(ta)),
                b=dict(path=str(b),sha256=hb,index=bi,total=len(tb)),
                unmatched_a=len(da),unmatched_b=len(db),
                same_geometry_different_material_pairs=material,
                unmatched_geometry_a=len(da)-material,unmatched_geometry_b=len(db)-material,
                triangles_a=details(da),triangles_b=details(db),
                limitations='3-decimal vertex multiset comparison; no fitted alignment. Geometric differences can include rounding, triangulation or placement differences. No missing-wall or checkpoint claim.')
    (out/'comparison.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if not k.startswith('triangles_')},indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('a',type=Path);p.add_argument('b',type=Path)
    p.add_argument('ai',type=int);p.add_argument('bi',type=int);p.add_argument('output',type=Path)
    x=p.parse_args();run(x.a,x.b,x.ai,x.bi,x.output)
