#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run source checks and relevant driver tests; never contacts a phone."""
from pathlib import Path
import argparse, json, subprocess, sys

root = Path(__file__).resolve().parents[1]
common = root / 'kernel'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--extended', action='store_true',
                    help='Also run instruction-level PSCI tests; requires built XNU and Unicorn')
args = parser.parse_args()
names = '''touch-startup touch-read touch-transport touch-power touch-app-info
touch-report-bounds touch-firmware-status touch-recovery touch-queue
display-cache native-status native-keyboard native-wallpaper framebuffer-density
ui-method media-method ca-profile focaltech-optional-fw dwmmc dwmmc-clock
emmc-log-record emmc-log-writer'''.split()
if args.extended:
    names += 'psci-alias psci-gated psci-reset psci-trampoline'.split()
results = []
outdir = root / '.local/verification'
outdir.mkdir(parents=True, exist_ok=True)
for name in names:
    path = common / 'tools' / (name + '-test.py')
    if not path.exists():
        raise SystemExit('Missing test: ' + str(path.relative_to(root)))
    with (outdir / (name + '.txt')).open('w') as log:
        run = subprocess.run([sys.executable, str(path)], cwd=common, stdout=log,
                             stderr=subprocess.STDOUT)
    results.append({'test': name, 'returncode': run.returncode})
    print(f'{name}: {"PASS" if run.returncode == 0 else "FAIL"}', flush=True)
    if run.returncode:
        print((outdir / (name + '.txt')).read_text())
        raise SystemExit(run.returncode)
for name in ['drivers-check.py', 'boot-args-test.py']:
    run = subprocess.run([sys.executable, str(common / 'tools' / name)], cwd=common)
    results.append({'test': name, 'returncode': run.returncode})
    if run.returncode:
        raise SystemExit(run.returncode)
run = subprocess.run([sys.executable, '-m', 'unittest', 'discover', '-s', 'tests'],
                     cwd=root / 'device')
results.append({'test': 'device-unittest', 'returncode': run.returncode})
if run.returncode:
    raise SystemExit(run.returncode)
report = {'date': '2026-11-06', 'tests': results, 'result': 'PASS', 'extended': args.extended,
          'scope': 'Host verification only; no phone writes and no claim of working wireless/Metal'}
(outdir / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print('Source and driver verification PASS; detailed output is in .local/verification/')
