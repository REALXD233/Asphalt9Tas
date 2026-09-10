"""Join live shape-attribute evidence to an existing geometry snapshot, offline."""
import argparse
import json
from pathlib import Path
import struct

def annotate(track_blob, evidence):
    if track_blob[:4] != b'BCOL':
        raise ValueError('not BCOL')
    size=struct.unpack_from('<I',track_blob,4)[0]
    manifest=json.loads(track_blob[8:8+size])
    lookup={(int(x['group'],16),x['member']):x for x in evidence['objects']}
    for obj in manifest['Objects']:
        source=obj['Source']
        row=lookup[(source['group'],source['member'])]
        if source['native']!=row['native'] or source['shape']!=row['shape']['address']:
            raise ValueError('geometry and attributes are from different objects')
        obj['NativeAttributes']=row['shape']
    manifest['Meta']['AttributeEvidence']=dict(pid=evidence['pid'],
        summary=evidence['summary'], limitation=evidence['limitation'])
    encoded=json.dumps(manifest).encode()
    return b'BCOL'+struct.pack('<I',len(encoded))+encoded+track_blob[8+size:]

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('track',type=Path)
    p.add_argument('attributes',type=Path)
    p.add_argument('output',type=Path)
    a=p.parse_args()
    blob=annotate(a.track.read_bytes(),json.loads(a.attributes.read_text()))
    with a.output.open('xb') as f:
        f.write(blob)
    print(str(a.output))
