#!/usr/bin/env python3
"""Exercise the actual cache invalidator on private binary plists, without a device."""
import pathlib, plistlib, struct, subprocess, tempfile

root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
    td = pathlib.Path(td)
    binary = td/'display-cache'
    subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-framework', 'CoreFoundation', str(root/'userland/src/display_cache.c'),
                    '-o', str(binary)], check=True)
    def run(width, height, scale, cached, expected, changed, dpi=None):
        cache = td/'cache.plist'
        backup = td/'cache.plist.hakios-display-old'
        cache.unlink(missing_ok=True); backup.unlink(missing_ok=True)
        original = plistlib.dumps({'CacheVersion':'test', 'CacheExtra':
            {'oBbtJ8x+s1q0OkaiocPuog':cached, 'unrelated':'keep'}}, fmt=plistlib.FMT_BINARY)
        cache.write_bytes(original)
        args=[str(binary), '--cache', str(cache), str(width), str(height), str(scale)]
        if dpi is not None: args.append(str(dpi))
        result = subprocess.run(args, capture_output=True, text=True)
        assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
        assert backup.exists() == changed
        assert cache.exists() != changed
        assert (backup if changed else cache).read_bytes() == original
    screen = lambda w,h,s: struct.pack('<IIIffi', w,h,458,s,0,-1)
    run(720,1600,2,screen(1125,2436,2),0,True)
    run(720,1600,2,screen(720,1600,2),0,False)
    run(720,1600,2,screen(720,1600,3),0,True)
    run(720,1600,2,b'bad',1,False)
    run(0,1600,2,screen(1125,2436,2),2,False)
    run(828,1792,2,screen(828,1792,2),0,True,dpi=326)
    record=struct.pack('<IIIffi',828,1792,326,2,0,16)
    run(828,1792,2,record,0,False,dpi=326)
    print('display-cache: stale size, unchanged size, stale scale, malformed data, invalid geometry: PASS')
