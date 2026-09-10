"""Session-scoped read-only Android mesh export from a captured world census.

Verified Huawei layout only. Unsupported shapes remain explicitly omitted.
"""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import struct
from read_collision_span import read
from track_collision_export import export

def transform_for_bcol(raw):
    # Native RVA 5e35bb8 dots each ROW (+0,+16,+32) with a point.
    # BCOL exporter expects columns; origin remains floats 12..14.
    value = list(struct.unpack('<16f', raw))
    if not all(math.isfinite(x) for x in value):
        raise ValueError('nonfinite transform')
    out = value.copy()
    for r in range(3):
        for c in range(3):
            out[c*4+r] = value[r*4+c]
    return out

class Capture:
    def __init__(self, pid, material_base=None):
        self.pid = pid
        self.material_base = material_base
        self.cache = {}
        self.shapes = []
        self.shape_indices = {}
        self.payload = bytearray()

    def span(self, address, length):
        key = (address, length)
        if key not in self.cache:
            if not (0 < length <= 16*1024*1024):
                raise ValueError('unexpected span size')
            parts = []
            for start in range(0, length, 1048576):
                size = min(1048576, length-start)
                first = read(self.pid, address+start, size)
                if read(self.pid, address+start, size) != first:
                    raise ValueError('unstable capture span')
                parts.append(first)
            self.cache[key] = b''.join(parts)
        return self.cache[key]

    def shape(self, address, depth=0):
        if address in self.shape_indices:
            return self.shape_indices[address]
        if depth > 8:
            raise ValueError('shape recursion depth')
        head = self.span(address, 128)
        kind = struct.unpack_from('<I', head, 36)[0]
        if kind == 0:
            half = list(struct.unpack_from('<3f', head, 0x40))
            margin = struct.unpack_from('<f', head, 0x50)[0]
            if not all(math.isfinite(x) for x in [*half,margin]) or margin<0 or min(x+margin for x in half)<0:
                raise ValueError('invalid box dimensions')
            # Already scaled implicit dimensions. Do not apply local scale twice.
            value = dict(Type=0, HalfExtents=[x+margin for x in half],
                         ImplicitHalfExtents=half, CollisionMargin=margin,
                         Surface='outer box envelope including margin; rounded corners not modeled')
        elif kind == 8:
            radius = struct.unpack_from('<f',head,0x40)[0]*struct.unpack_from('<f',head,0x30)[0]
            if not math.isfinite(radius) or radius < 0:
                raise ValueError('invalid sphere radius')
            value = dict(Type=8, HalfExtents=[radius]*3, Surface='sphere tessellation')
        elif kind == 10:
            half=list(struct.unpack_from('<3f',head,0x40))
            axis=struct.unpack_from('<I',head,0x58)[0]
            margin=struct.unpack_from('<f',head,0x50)[0]
            if axis not in (0,1,2) or not all(math.isfinite(x) and x>=0 for x in [*half,margin]):
                raise ValueError('invalid capsule dimensions')
            value=dict(Type=10,HalfExtents=half,UpAxys=axis,CollisionMargin=margin,
                       Surface='capsule tessellation; extra collision margin not expanded')
        elif kind == 31:
            count = struct.unpack_from('<I',head,0x34)[0]
            pointer = struct.unpack_from('<Q',head,0x40)[0]
            if count > 4096:
                raise ValueError('compound count outside capture bound')
            children = self.span(pointer,count*88) if count else b''
            items=[]
            for i in range(count):
                child=children[i*88:(i+1)*88]
                child_address=struct.unpack_from('<Q',child,0x40)[0]
                items.append(dict(ChildIndex=self.shape(child_address,depth+1),
                                  LocalTrans=transform_for_bcol(child[:64])))
            value=dict(Type=31,Children=items)
        elif kind == 22:
            scale = list(struct.unpack_from('<3f', head, 0x34))
            if not all(math.isfinite(x) for x in scale):
                raise ValueError('invalid scale')
            child = struct.unpack_from('<Q', head, 0x48)[0]
            value = dict(Type=22, LocalScale=scale, ChildIndex=self.shape(child, depth+1))
        elif kind == 26:
            interface = struct.unpack_from('<Q', head, 0x58)[0]
            iface = self.span(interface, 64)
            scale = list(struct.unpack_from('<3f', iface, 8))
            count = struct.unpack_from('<I', iface, 0x1c)[0]
            if not 0 < count <= 256:
                raise ValueError('subpart count')
            descriptors = self.span(struct.unpack_from('<Q', iface, 0x28)[0], 48*count)
            materials = None
            material_meta = []
            if self.material_base is not None:
                # Huawei 439fd7f9: virtual getter 5e4bbcc -> 5e4bda8.
                # Descriptor array at +0x78, 48 bytes each; NOT upstream x64 +0x80.
                if struct.unpack_from('<Q',iface)[0] != self.material_base + 0x9f25ae8:
                    raise ValueError('material interface ABI not proven')
                mh = self.span(interface+0x68, 24)
                if struct.unpack_from('<I',mh,4)[0] != count:
                    raise ValueError('material subpart mismatch')
                materials = self.span(struct.unpack_from('<Q',mh,16)[0],count*48)
            offset = len(self.payload)
            for i in range(count):
                desc = descriptors[i*48:(i+1)*48]
                faces = struct.unpack_from('<I', desc)[0]
                index_addr = struct.unpack_from('<Q', desc, 8)[0]
                istride, nv = struct.unpack_from('<II', desc, 16)
                vaddr = struct.unpack_from('<Q', desc, 24)[0]
                vstride, itype, vtype = struct.unpack_from('<III', desc, 32)
                if itype not in (2,3) or vtype != 0 or vstride < 12 or vstride > 256:
                    raise ValueError('unsupported vertex/index format')
                index_format = '<3H' if itype == 3 else '<3I'
                if istride < struct.calcsize(index_format) or istride > 256:
                    raise ValueError('invalid index stride')
                vb = self.span(vaddr, nv*vstride)
                ib = self.span(index_addr, faces*istride)
                tri_materials = None
                if materials is not None:
                    md=materials[i*48:(i+1)*48]
                    nm=struct.unpack_from('<I',md)[0]
                    ms=struct.unpack_from('<I',md,16)[0]
                    nf=struct.unpack_from('<I',md,24)[0]
                    ts=struct.unpack_from('<I',md,40)[0]
                    if nf!=faces or ts!=1 or not 0<nm<=256 or not 4<=ms<=256:
                        raise ValueError('material descriptor outside proven byte-index layout')
                    tri_materials=self.span(struct.unpack_from('<Q',md,32)[0],nf)
                    if max(tri_materials,default=0)>=nm:
                        raise ValueError('material index out of range')
                    table=self.span(struct.unpack_from('<Q',md,8)[0],nm*ms)
                    material_meta.append(dict(Subpart=i,Count=nm,Stride=ms,RawTable=table.hex(),
                        TriangleCounts=dict(collections.Counter(tri_materials))))
                for f in range(faces):
                    indices = struct.unpack_from(index_format, ib, f*istride)
                    if max(indices) >= nv:
                        raise ValueError('index out of range')
                    xyz = [x*scale[k] for n in indices for k,x in enumerate(struct.unpack_from('<3f', vb, n*vstride))]
                    if not all(math.isfinite(x) for x in xyz):
                        raise ValueError('nonfinite triangle')
                    self.payload.extend(struct.pack('<9fi', *xyz, tri_materials[f] if tri_materials is not None else -1))
            value = dict(Type=26, ByteOffset=offset, ByteLength=len(self.payload)-offset)
            if material_meta:
                value['NativeMaterialTables']=material_meta
        else:
            raise NotImplementedError(f'unsupported shape {kind}')
        index = len(self.shapes)
        self.shapes.append(value)
        self.shape_indices[address] = index
        return index

def run(census_path, destination, material_base=None):
    census = json.loads(census_path.read_text(encoding='utf-8-sig'))
    if not census['header_stable'] or not census['same_process']:
        raise ValueError('unstable census')
    capture = Capture(census['pid'], material_base)
    world = int(census['world'],16)
    objects, omitted = [], []
    world_now = capture.span(world,1024)
    for group in census['array_candidates']:
        if group['offset'] not in (0x148,0x160):
            continue
        begin,end,limit = struct.unpack_from('<3Q',world_now,group['offset'])
        if begin != int(group['data'],16) or end-begin != group['size']*8:
            raise ValueError('world changed since census')
        pointers = capture.span(begin,end-begin)
        field = 0x90 if group['offset']==0x148 else 0x50
        for i, item in enumerate(group['heads']):
            wrapper = int(item['pointer'],16)
            if struct.unpack_from('<Q',pointers,i*8)[0] != wrapper:
                raise ValueError('object list changed')
            native = struct.unpack('<Q',capture.span(wrapper+field,8))[0]
            body = capture.span(native,256)
            shape_address = struct.unpack_from('<Q',body,0xd0)[0]
            head = capture.span(shape_address,128)
            kind = struct.unpack_from('<I',head,36)[0]
            meta = dict(group=group['offset'], member=i, native=hex(native), shape=hex(shape_address), type=kind)
            try:
                index = capture.shape(shape_address)
            except NotImplementedError as error:
                omitted.append(dict(meta,reason=str(error)))
                continue
            objects.append(dict(ShapeIndex=index, Trans=transform_for_bcol(body[16:80]),
                                ColFlags=struct.unpack_from('<I',body,0xe8)[0], Source=meta))
    if read(capture.pid,world,1024) != world_now:
        raise ValueError('world changed during export')
    destination.mkdir(parents=True,exist_ok=False)
    manifest = dict(Meta=dict(Version=1,Source='Android live mesh subset'),Shapes=capture.shapes,Objects=objects)
    raw = json.dumps(manifest).encode()
    track = destination/'android-meshes.TRACK'
    track.write_bytes(b'BCOL'+struct.pack('<I',len(raw))+raw+struct.pack('<I',len(capture.payload))+capture.payload)
    count = export(track,destination/'viewer')
    page = destination/'viewer/viewer.html'
    page.write_text(page.read_text(encoding='utf-8').replace('上游历史样本 · 非安卓实时地图',
        '安卓游戏实读 · 碰撞对象快照'),encoding='utf-8')
    provenance = dict(pid=capture.pid,exported_objects=len(objects),triangles=count,
        omitted_objects=omitted,omitted_counts=dict(collections.Counter(x['type'] for x in omitted)),
        spans=[dict(address=hex(a),bytes=n,sha256=hashlib.sha256(b).hexdigest()) for (a,n),b in capture.cache.items()],
        validation='each span immediately repeated, equal; world stable; not an atomic snapshot',
        limitations=('material indices and raw tables captured; material semantics unresolved' if material_base is not None else 'materials unknown')+'; sphere tessellation, box outer envelopes include margin, rounded corners not modeled; reset walls unclassified')
    (destination/'provenance.json').write_text(json.dumps(provenance,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in provenance.items() if k not in ('spans','omitted_objects')}))

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('census',type=Path)
    parser.add_argument('destination',type=Path)
    parser.add_argument('--material-base',type=lambda x:int(x,16))
    args=parser.parse_args()
    run(args.census,args.destination,args.material_base)
