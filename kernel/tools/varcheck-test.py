#!/usr/bin/env python3
"""Run the real persistence marker across fresh processes; reject corrupt data.
This tests the checker, not durability of any phone's storage hardware.
"""
import pathlib, subprocess, tempfile
root=pathlib.Path(__file__).resolve().parents[1]
header=root/'userland/src/varcheck.h'
assert header.exists(), 'persistent var read/write check is missing'
code=r'''
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "varcheck.h"
int main(void) { uint64_t before=0; int rc=hakios_var_check("marker","marker.next",".",&before); printf("%d %llu\n",rc,(unsigned long long)before); return rc?1:0; }
'''
with tempfile.TemporaryDirectory(prefix='hakios-varcheck-') as t:
    p=pathlib.Path(t);(p/'test.c').write_text(code)
    subprocess.run(['clang','-std=c11','-fsanitize=address,undefined','-I',str(header.parent),str(p/'test.c'),'-o',str(p/'test')],check=True)
    def run(): return subprocess.run([str(p/'test')],cwd=p,capture_output=True,text=True)
    a=run(); assert (a.returncode,a.stdout)==(0,'0 0\n'),a
    a=run(); assert (a.returncode,a.stdout)==(0,'0 1\n'),a
    (p/'marker.next').write_bytes(b'interrupted write')
    a=run(); assert (a.returncode,a.stdout)==(0,'0 2\n'),a
    assert not (p/'marker.next').exists()
    original=(p/'marker').read_bytes()
    for damaged in (b'',original[:16],original+b'x',bytes(32)):
        (p/'marker').write_bytes(damaged)
        a=run(); assert a.returncode==1, a
        assert (p/'marker').read_bytes()==damaged
print('var check: separate-process persistence, interrupted temp, corrupt/truncated markers PASS')
