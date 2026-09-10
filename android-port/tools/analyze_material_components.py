"""Offline edge-connected attribute surfaces; not gameplay trigger identities."""
import argparse,collections,json,math
from pathlib import Path
from track_collision_export import Track

def components(triangles,digits=4):
    parent=list(range(len(triangles)))
    def root(i):
        while parent[i]!=i:
            parent[i]=parent[parent[i]];i=parent[i]
        return i
    edges={}
    for i,vs in enumerate(triangles):
        vertices=[tuple(round(x,digits) for x in v) for v in vs]
        for a,b in ((0,1),(1,2),(2,0)):
            if vertices[a]==vertices[b]:continue
            edge=tuple(sorted((vertices[a],vertices[b])))
            if edge in edges:parent[root(i)]=root(edges[edge])
            else:edges[edge]=i
    groups=collections.defaultdict(list)
    for i in range(len(triangles)):groups[root(i)].append(i)
    out=[]
    for indices in groups.values():
        lo=[float('inf')]*3;hi=[-float('inf')]*3;area=0.
        for i in indices:
            a,b,c=triangles[i];u=[b[j]-a[j] for j in range(3)];v=[c[j]-a[j] for j in range(3)]
            area+=math.hypot(u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0])/2
            for p in (a,b,c):
                for j in range(3):lo[j]=min(lo[j],p[j]);hi[j]=max(hi[j],p[j])
        out.append(dict(triangle_count=len(indices),triangle_indices=indices,area=area,
                        bounds=[lo,hi],bounds_center=[(a+b)/2 for a,b in zip(lo,hi)]))
    return sorted(out,key=lambda x:(-x['triangle_count'],x['bounds_center']))

def analyze(path):
    t=Track(path.read_bytes());groups=collections.defaultdict(list)
    for o in t.objects:
        tags=o.get('NativeAttributes',{}).get('attributes',[])
        if tags:
            triangles=[v for v,_ in t.triangles(o['ShapeIndex'],transforms=(o['Trans'],))]
            for tag in tags:groups[tag].extend(triangles)
    result={name:components(ts) for name,ts in groups.items()}
    return dict(source=str(path),coordinate_system='raw game XYZ, Z up',
        rule='shared nondegenerate edge, endpoints rounded to 4 decimal places; not volumetric connectivity or gameplay IDs',
        components=result,summary={k:dict(components=len(v),triangles=sum(x['triangle_count'] for x in v)) for k,v in result.items()})

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('track',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    result=analyze(a.track)
    with a.output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps(result['summary']))
