"""Verify the packaged runtime set, not just the staging directory."""
import hashlib
import json
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as apk:
    manifest=json.loads(apk.read('assets/runtime/manifest.json'))
    checked=[]
    def visit(value):
        if isinstance(value,dict):
            if 'asset' in value and 'sha256' in value:
                data=apk.read('assets/'+value['asset'])
                assert hashlib.sha256(data).hexdigest()==value['sha256'].lower(),value['asset']
                checked.append(value['asset'])
            for item in value.values(): visit(item)
        elif isinstance(value,list):
            for item in value:visit(item)
    visit(manifest)
    assert checked
    for controller in ['a9tas_g4_input_action_controller_v1','a9tas_native_arm64_g4_input_action_controller_v1']:
        assert b'A9TAS_RECORD_SLOWMO_DIVISOR' in apk.read('assets/runtime/'+controller)
    dex=b''.join(apk.read(name) for name in apk.namelist() if name.endswith('.dex'))
    assert b'record_slowmo_divisor' in dex
    assert b'recordSlowmoSpinner' in dex
    print('PASS packaged hashes:',len(checked),'both controller slowmo options, Java preference/UI')
