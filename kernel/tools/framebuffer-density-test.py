#!/usr/bin/env python3
"""Extract the selector's production density expression and verify its profiles."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
body=s[s.index('case kFBMethodGetDotPitch:'):s.index('case kFBMethodGetLayerDefaultSurface:')]
expr=re.search(r'out\[0\] = (.*);',body)[1].replace('fOwner->fWidth','width')
program='''#include <stdint.h>
#include <assert.h>
#define kFBDotsPerInch 458
#define kFBDefaultWidth 1125
static uint32_t gFBDPI;
static uint32_t pitch(uint32_t width){return EXPR;}
int main(void){
 gFBDPI=0;assert(pitch(1125)==458);
 gFBDPI=326;assert(pitch(828)==326);
 gFBDPI=270;assert(pitch(720)==270);
 gFBDPI=1;assert(pitch(720)==293);
 gFBDPI=1001;assert(pitch(720)==293);
}
'''.replace('EXPR',expr)
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'check.c').write_text(program)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
assert '"fbdpi"' in s
print('Framebuffer density: default, XR 326, native 270, invalid bounds fallback: PASS')
