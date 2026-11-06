/* MIT. Optional native 720x1600 status bar for the 20H392 bring-up.
 * UIKit's split-screen provider has no 360-point-wide phone profile. Retain
 * every recognized provider and use its flexible phone layout only for the
 * otherwise unsupported case. No display dimensions or touch coordinates are
 * changed. Enable explicitly with hakios_native_ui=1 and the native geometry.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *);
static uintptr_t slide;
static Class legacy, fallback;
static Class (*original_provider)(id, SEL, id, id);
static unsigned said;
static unsigned keyboard_said;
static unsigned wallpaper_said;
static CGSize (*original_keyboard_size)(id, SEL, long long);
static id (*original_device_logical_screen_class)(id, SEL);
static BOOL (*original_should_load_wallpaper_bundle)(id, SEL, id);
static void say(const char *format, ...) {
 char b[512];va_list ap;va_start(ap,format);int n=vsnprintf(b,sizeof(b),format,ap);va_end(ap);
 if(n>0) {
  int fd=open("/dev/console",O_WRONLY|O_NOCTTY);
  if(fd>=0) {write(fd,b,(size_t)n<sizeof(b)?(size_t)n:sizeof(b)-1);close(fd);}
 }
}
static Class native_provider(id self, SEL selector, id screen, id info) {
 Class result=original_provider(self,selector,screen,info);
 if(legacy && (!result || result==fallback)) {
  if(!said) {said=1;say("hakios: native display: unsupported status layout -> flexible phone layout\n");}
  return legacy;
 }
 return result;
}
/* On the native 360-point screen, 20H392 constructs UIKeyboardAutomatic
 * at its unsupported-screen fallback width of 320, while its implementation
 * and keyplane are 360 wide. Correct the default size before UIKit creates
 * and centers that container. UIKit retains ownership of layout and hit tests.
 * Preserve the stock height, non-fallback widths and landscape behavior. */
static CGSize native_keyboard_size(id self, SEL selector, long long orientation) {
 CGSize result=original_keyboard_size(self,selector,orientation);
 if((orientation==1 || orientation==2) && result.width==320 && result.height>0 && result.height<=400) {
  result.width=360;
  if(!keyboard_said) {keyboard_said=1;say("hakios: native keyboard: portrait container 320 -> 360 points\n");}
 }
 return result;
}
static void install_keyboard(void) {
 if(original_keyboard_size)return;
 Class cls=objc_getClass("UIKeyboardImpl");if(!cls)return;
 Method method=class_getClassMethod(cls,sel_registerName("defaultSizeForInterfaceOrientation:"));
 IMP original=method?method_getImplementation(method):NULL;
 const char *types=method?method_getTypeEncoding(method):NULL;
 if(!original || (uintptr_t)original-slide!=0x18911ca08UL || !types ||
     strcmp(types,"{CGSize=dd}24@0:8q16"))return;
 original_keyboard_size=(CGSize (*)(id,SEL,long long))original;
 method_setImplementation(method,(IMP)native_keyboard_size);
 say("hakios: native keyboard: guarded 20H392 default size installed in %s\n",getprogname());
}
static id native_device_logical_screen_class(id self, SEL selector) {
 (void)self;(void)selector;
 if(!wallpaper_said) {
  wallpaper_said=1;
  say("hakios: native wallpaper: logical screen class 360w-800h@2x -> 375w-812h@3x~iphone\n");
 }
 return (id)CFSTR("375w-812h@3x~iphone");
}
static BOOL native_should_load_wallpaper_bundle(id self, SEL selector, id url) {
 if(!url)return false;
 id path=((id (*)(id,SEL))objc_msgSend)(url,sel_registerName("path"));
 const char *cpath=path?((const char * (*)(id,SEL))objc_msgSend)(path,sel_registerName("UTF8String")):NULL;
 if(cpath) {
  if(strstr(cpath,"393w-852h")!=NULL || strstr(cpath,"430w-932h")!=NULL) {
   return false;
  }
  if(strstr(cpath,"375w-812h")!=NULL) {
   return true;
  }
 }
 if(original_should_load_wallpaper_bundle) {
  return original_should_load_wallpaper_bundle(self,selector,url);
 }
 return true;
}
static void install_wallpaper(void) {
 if(!original_device_logical_screen_class) {
  Class cls=objc_getClass("WKDefaultWallpaperManager");
  if(cls) {
   SEL sel=sel_registerName("deviceLogicalScreenClass");
   Method m=class_getInstanceMethod(cls,sel);
   IMP orig=m?method_getImplementation(m):NULL;
   const char *types=m?method_getTypeEncoding(m):NULL;
   if(orig && (uintptr_t)orig-slide==0x1b23d8e64UL && types && !strcmp(types,"@16@0:8")) {
    original_device_logical_screen_class=(id (*)(id,SEL))orig;
    method_setImplementation(m,(IMP)native_device_logical_screen_class);
    say("hakios: native wallpaper: guarded 20H392 screen class installed in %s\n",getprogname());
   }
  }
 }
 if(!original_should_load_wallpaper_bundle) {
  Class cls=objc_getClass("WKWallpaperBundle");
  if(cls) {
   SEL sel=sel_registerName("shouldLoadWallpaperBundleAtURL:");
   Method m=class_getClassMethod(cls,sel);
   IMP orig=m?method_getImplementation(m):NULL;
   const char *types=m?method_getTypeEncoding(m):NULL;
   if(orig && (uintptr_t)orig-slide==0x1b23beca0UL && types && !strcmp(types,"B24@0:8@16")) {
    original_should_load_wallpaper_bundle=(BOOL (*)(id,SEL,id))orig;
    method_setImplementation(m,(IMP)native_should_load_wallpaper_bundle);
    say("hakios: native wallpaper: guarded 20H392 bundle filter installed in %s\n",getprogname());
   }
  }
 }
}
static void image_added(const struct mach_header *header, intptr_t delta) {
 (void)header;(void)delta;
 install_keyboard();
 install_wallpaper();
 if(original_provider)return;
 Class cls=objc_getClass("_UIStatusBarVisualProvider_iOS");
 if(!cls)return;
 legacy=objc_getClass("_UIStatusBarVisualProvider_LegacyPhone");
 fallback=objc_getClass("_UIStatusBarVisualProvider_Fallback");
 SEL selector=sel_registerName("visualProviderSubclassForScreen:visualProviderInfo:");
 Method method=class_getClassMethod(cls,selector);
 IMP original=method?method_getImplementation(method):NULL;
 const char *types=method?method_getTypeEncoding(method):NULL;
 if(!legacy || !fallback || !original || (uintptr_t)original-slide!=0x189abe338UL ||
     !types || strcmp(types,"#32@0:8@16@24")) {
  say("hakios: native display: provider guard rejected address=%#lx types=%s\n",
      original?(uintptr_t)original-slide:0,types?types:"none");return;
 }
 original_provider=(Class (*)(id,SEL,id,id))original;
 method_setImplementation(method,(IMP)native_provider);
 say("hakios: native display: guarded 20H392 provider installed in %s\n",getprogname());
}
__attribute__((constructor))static void begin(void) {
 char args[4096];size_t n=sizeof(args)-1;
 if(sysctlbyname("kern.bootargs",args,&n,NULL,0))return;
 args[n]=0;bool enable=false,width=false,height=false,scale=false;
 for(char *t=strtok(args," \t");t;t=strtok(NULL," \t")) {
  if(!strcmp(t,"hakios_native_ui=1"))enable=true;
  if(!strcmp(t,"fbwidth=720"))width=true;
  if(!strcmp(t,"fbheight=1600"))height=true;
  if(!strcmp(t,"fbscale=2"))scale=true;
 }
 if(!enable || !width || !height || !scale)return;
 size_t length=0;slide=(uintptr_t)_dyld_get_shared_cache_range(&length)-0x180000000ULL;
 _dyld_register_func_for_add_image(image_added);
}
