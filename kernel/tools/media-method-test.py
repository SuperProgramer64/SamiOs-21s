#!/usr/bin/env python3
"""Compile and exercise the production build/selector/address guard."""
import pathlib, subprocess, tempfile
root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
    td=pathlib.Path(td)
    src=td/'check.c'
    src.write_text('''#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "inject_methods.h"
int main(void) {
 const char *c="BWCoreImageFilterRenderer", *s="prewarmPortraitFilterVersion:semanticStyleFilters:";
 assert(hakios_method_matches('+',c,s,0x1a2115c0cUL,0x1a2115c0cUL));
 assert(hakios_method_matches('+',c,s,0x1a2115c0cUL,0x1a2165b8cUL));
 assert(!hakios_method_matches('+',c,s,0x1a2115c0cUL,0x1a2165b90UL));
 assert(!hakios_method_matches('-',c,s,0x1a2115c0cUL,0x1a2165b8cUL));
 assert(!hakios_method_matches('+',"Other",s,0x1a2115c0cUL,0x1a2165b8cUL));
 assert(!hakios_method_matches('+',c,"other:",0x1a2115c0cUL,0x1a2165b8cUL));
}
''')
    binary=td/'check'
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
        '-I',str(root/'userland/src'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    media=(root/'userland/src/inject_media.c').read_text()
    assert 'hakios_method_matches(' in media
    print('media method: original/current implementation accepted; wrong kind, selector, class, address rejected: PASS')
