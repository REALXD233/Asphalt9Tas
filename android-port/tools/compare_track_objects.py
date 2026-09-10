"""Offline object-level comparison; no fitted transforms or gameplay labels."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
from track_collision_export import Track
from compare_collision_tracks import key, mesh_shape


def fingerprint(triangles):
    return hashlib.sha256(repr(sorted(triangles.items())).encode()).hexdigest()


def objects(path):
    blob = path.read_bytes()
    track = Track(blob)
    result = []
    for index, obj in enumerate(track.objects):
        shape = obj['ShapeIndex']
        if not mesh_shape(track, shape):
            continue
        world = list(track.triangles(shape, transforms=(obj['Trans'],)))
        local = list(track.triangles(shape))
        counter = Counter((key(v, 3), m) for v, m in world)
        local_counter = Counter((key(v, 3), m) for v, m in local)
        result.append(dict(index=index, shape=shape, triangles=len(world),
            transform=obj['Trans'], world_hash=fingerprint(counter),
            local_hash=fingerprint(local_counter), counter=counter,
            materials=dict(Counter(m for _, m in world))))
    return hashlib.sha256(blob).hexdigest(), result


def match_objects(a, b):
    remaining_a = {x['index']: x for x in a}
    remaining_b = {x['index']: x for x in b}
    matches = []
    for field, label in [('world_hash', 'same_world_geometry_and_material'),
                         ('local_hash', 'same_local_geometry_world_differs')]:
        ga, gb = defaultdict(list), defaultdict(list)
        for item in remaining_a.values(): ga[item[field]].append(item)
        for item in remaining_b.values(): gb[item[field]].append(item)
        for h in sorted(ga.keys() & gb.keys()):
            # Only unique matches: identical copies have no proven identity.
            if len(ga[h]) != 1 or len(gb[h]) != 1: continue
            x, y = ga[h][0], gb[h][0]
            matches.append(dict(a=x['index'], b=y['index'], classification=label,
                triangles_a=x['triangles'], triangles_b=y['triangles']))
            del remaining_a[x['index']]; del remaining_b[y['index']]
    candidates = []
    for x in remaining_a.values():
        for y in remaining_b.values():
            common = sum((x['counter'] & y['counter']).values())
            if common:
                candidates.append(dict(a=x['index'], b=y['index'], common=common,
                    fraction_a=common/max(1,x['triangles']),
                    fraction_b=common/max(1,y['triangles']),
                    unmatched_a=x['triangles']-common, unmatched_b=y['triangles']-common))
    return matches, sorted(candidates, key=lambda x: -x['common'])


def compare(a, b):
    ha, oa = objects(a); hb, ob = objects(b)
    matches, candidates = match_objects(oa, ob)
    def clean(items): return [{k:v for k,v in x.items() if k!='counter'} for x in items]
    return dict(a=dict(path=str(a),sha256=ha,mesh_objects=clean(oa)),
        b=dict(path=str(b),sha256=hb,mesh_objects=clean(ob)),
        unique_matches=matches, remaining_overlap_candidates=candidates,
        limitations='Mesh objects only; coordinates rounded to 3 decimals; material IDs compared numerically. Local equality does not prove gameplay identity. Transform changes are not evidence of live motion. No coordinate alignment or checkpoint classification inferred.')


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('a',type=Path);p.add_argument('b',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();report=compare(args.a,args.b)
    with args.output.open('x',encoding='utf-8') as f: json.dump(report,f,indent=2)
    print(json.dumps(dict(matches=report['unique_matches'],
        overlaps=report['remaining_overlap_candidates'][:12]),indent=2))
