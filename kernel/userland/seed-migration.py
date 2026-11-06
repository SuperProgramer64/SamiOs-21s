#!/usr/bin/env python3
"""Seed a new port image for its actual system build, never an attached data volume.
Existing persistent volumes require a separate, explicit migration operation.
"""
import pathlib, plistlib, re, sys

def seed(system_version, destination):
    version = plistlib.loads(pathlib.Path(system_version).read_bytes())
    build = version['ProductBuildVersion']
    if not isinstance(build, str) or not re.fullmatch(r'[0-9]+[A-Z][0-9]+[a-z]?', build):
        raise ValueError('invalid ProductBuildVersion')
    data = {'LastSystemVersion': build,
            'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': build}}
    pathlib.Path(destination).write_bytes(plistlib.dumps(data))
    return build

if __name__ == '__main__':
    print('migration seed: ' + seed(*sys.argv[1:]))
