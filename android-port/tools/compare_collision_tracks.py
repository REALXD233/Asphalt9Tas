"""Offline world-space geometry comparison, no assumed object ordering or alignment."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
from track_collision_export import Track

def mesh_shape(t, i):
    s=t.shapes[i]
    return s.get('Type')==26 or (s.get('Type')==22 and mesh_shape(t,s['ChildIndex']))

def extract(path):
    blob=path.read_bytes();t=Track(blob)
    triangles=[];materials=collections.Counter()
    for obj in t.objects:
        if mesh_shape(t,obj['ShapeIndex']):
            for vertices,mat in t.triangles(obj['ShapeIndex'],transforms=(obj['Trans'],)):
                triangles.append((vertices,mat));materials[mat]+=1
    return dict(path=str(path),sha256=hashlib.sha256(blob).hexdigest(),objects=len(t.objects),
        mesh_triangles=len(triangles),materials=dict(materials),
        bounds=[[min(v[a] for vs,_ in triangles for v in vs) for a in range(3)],
                [max(v[a] for vs,_ in triangles for v in vs) for a in range(3)]]),triangles

def key(vs, digits):
    return tuple(sorted(tuple(round(x,digits) for x in v) for v in vs))

def compare(a,b):
    ma,ta=extract(a);mb,tb=extract(b)
    results=[]
    for digits in (3,2,1):
        ca=collections.Counter(key(v,digits) for v,_ in ta)
        cb=collections.Counter(key(v,digits) for v,_ in tb)
        common=sum((ca&cb).values())
        results.append(dict(coordinate_round_digits=digits,matching_triangles=common,
            fraction_a=common/len(ta),fraction_b=common/len(tb)))
    return dict(a=ma,b=mb,geometry_matches=results,
        limitations='No fitted transform. Rounded vertex-set equality is a conservative match, not Hausdorff distance; bin boundaries can miss close vertices. Different tessellation is not necessarily a different surface. Primitive display meshes excluded.')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('a',type=Path);p.add_argument('b',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();result=compare(a.a,a.b)
    with a.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps(result))
