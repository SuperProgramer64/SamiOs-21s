#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the ignored optional header from firmware supplied by its owner."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('firmware', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
data = a.firmware.read_bytes()
if not 1 <= len(data) <= 1024 * 1024:
    p.error('firmware must contain 1..1048576 bytes')
lines = ['/* Generated locally from user-supplied firmware. Never commit this file. */',
         '#ifndef _HAKIOS_FOCALTECH_FW_H', '#define _HAKIOS_FOCALTECH_FW_H',
         f'#define FOCALTECH_FW_LENGTH {len(data)}u',
         'static const unsigned char focaltech_fw[FOCALTECH_FW_LENGTH] = {']
for n in range(0, len(data), 16):
    lines.append('  ' + ', '.join(f'0x{v:02x}' for v in data[n:n+16]) + ',')
lines += ['};', '#endif', '']
with a.output.open('x') as out:
    out.write('\n'.join(lines))
print('Generated local firmware header; do not publish it.')
