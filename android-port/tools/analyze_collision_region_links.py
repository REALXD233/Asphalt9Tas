"""Offline region-wrapper linkage audit; never infers reset semantics from flags."""
import argparse
import collections
import json
from pathlib import Path
import struct

def analyze(census):
    result=[]
    for group in census['array_candidates']:
        if group['offset']!=0x160:
            continue
        for index,item in enumerate(group['heads']):
            wrapper=int(item['pointer'],16)
            head=bytes.fromhex(item['head_hex'])
            link=next(x for x in item['linked_heads'] if x['field']==0x50)
            native=bytes.fromhex(link['head_hex'])
            if len(head)<176 or len(native)<272:
                raise ValueError('truncated census object')
            flags=struct.unpack_from('<I',native,0xe8)[0]
            backref=struct.unpack_from('<Q',native,0x108)[0]
            result.append(dict(member=index,wrapper=hex(wrapper),native='0x'+link['pointer'],
                flags=flags,no_contact_response=bool(flags&4),
                user_pointer=hex(backref),backref_matches_wrapper_38=backref==wrapper+0x38,
                wrapper_field_30_unclassified=hex(struct.unpack_from('<Q',head,0x30)[0]),
                shape_resource=hex(struct.unpack_from('<Q',head,0x98)[0]),
                semantic_class='unknown'))
    return dict(pid=census['pid'],count=len(result),
        backref_matches=sum(x['backref_matches_wrapper_38'] for x in result),
        no_contact_count=sum(x['no_contact_response'] for x in result),
        field_30_groups=dict(collections.Counter(x['wrapper_field_30_unclassified'] for x in result)),
        static_evidence='Huawei RVA4cf7610 constructor: native+0xe8 |=4; native+0x108=wrapper+0x38; wrapper+0x98 shared shape resource',
        limitation='generic region linkage only; neither reset nor checkpoint nor finish classification proven',
        objects=result)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('census',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    result=analyze(json.loads(args.census.read_text(encoding='utf-8-sig')))
    with args.output.open('x',encoding='utf-8') as stream:
        json.dump(result,stream,indent=2)
    print(json.dumps({k:v for k,v in result.items() if k not in ('objects','field_30_groups')}))
