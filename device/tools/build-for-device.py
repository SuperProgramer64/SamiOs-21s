#!/usr/bin/env python3
"""Build a device-specific A21s loader after reading its actual super layout."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--serial', required=True)
p.add_argument('--android-boot', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
if not a.android_boot.is_file():
    p.error('supply this phone\'s backed-up Android boot image')
spec = importlib.util.spec_from_file_location('xnusys', root / 'tools/xnusys.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
r = module.region(a.serial)
module.show(r)
base = int(r['super_start'] + r['first'])
count = int(r['sectors'] - module.PAYLOAD)
if base <= 0 or count <= 0 or count % module.ALIGN:
    raise SystemExit('invalid or unaligned region; refusing to build')
print('Building a loader for this exact layout. Re-run after any repartition.', flush=True)
env = os.environ.copy()
env['ANDROID_BOOT'] = str(a.android_boot.resolve())
env['STEP'] = '4'
env['EXTRA_CFLAGS'] = (env.get('EXTRA_CFLAGS', '') +
    f' -DXNU_EMMC_ROOT=1 -DXNUSYS_LBA_DEC={base} -DXNUSYS_IMAGE_SECTORS={count}')
subprocess.run(['bash', str(root / 'loader/build.sh')], env=env, check=True)
