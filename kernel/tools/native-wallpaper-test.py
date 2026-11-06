#!/usr/bin/env python3
"""Exercise the actual native WallpaperKit callbacks; replace only Objective-C dispatch."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'userland/src/display_native.c').read_text()
a=s.index('static id native_device_logical_screen_class(')
b=s.index('static void install_wallpaper(',a)
body=s[a:b]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef void *id;
typedef const char *SEL;
typedef bool BOOL;
#define CFSTR(x) (x)
static unsigned wallpaper_said,logs,orig_calls;
static bool orig_result;
static void say(const char *fmt,...){(void)fmt;logs++;}
static SEL sel_registerName(const char *n){return n;}
static id test_dispatch(id object,SEL selector){
 assert(object);
 assert(!strcmp(selector,"path")||!strcmp(selector,"UTF8String"));
 return object;
}
/* objc_msgSend uses selector-specific return types; this stand-in preserves
 * its pointer ABI while the real callbacks remain unmodified. */
#define objc_msgSend test_dispatch
static BOOL orig(id self,SEL sel,id url){
 (void)self;(void)sel;assert(url);orig_calls++;return orig_result;
}
static BOOL (*original_should_load_wallpaper_bundle)(id,SEL,id)=orig;
'''
tests=r'''
int main(void){
 assert(!strcmp(native_device_logical_screen_class(0,0),"375w-812h@3x~iphone"));
 native_device_logical_screen_class(0,0);assert(logs==1);
 assert(!native_should_load_wallpaper_bundle(0,0,0));assert(orig_calls==0);
 assert(native_should_load_wallpaper_bundle(0,0,"/Library/Wallpaper/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper"));
 assert(!native_should_load_wallpaper_bundle(0,0,"/Library/Wallpaper/7400.WWDC_2022-393w-852h@3x~iphone.wallpaper"));
 assert(!native_should_load_wallpaper_bundle(0,0,"/Library/Wallpaper/7400.WWDC_2022-430w-932h@3x~iphone.wallpaper"));
 assert(orig_calls==0);
 orig_result=false;
 assert(!native_should_load_wallpaper_bundle(0,0,"/Library/Wallpaper/Solid_Black.wallpaper"));assert(orig_calls==1);
 orig_result=true;
 assert(native_should_load_wallpaper_bundle(0,0,"/Library/Wallpaper/Other.wallpaper"));assert(orig_calls==2);
 puts("actual WallpaperKit callbacks: stock variant, nil, duplicate rejection and original allow/deny preserved PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'check.c').write_text(prefix+body+tests)
 subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize=function',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
