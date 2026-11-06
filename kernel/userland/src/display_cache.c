/*
 * Discard only MobileGestalt's stale hardware cache after an explicit display
 * geometry change. Run before launchd, after init mounts and seeds /private/var.
 * Keep the original plist by rename; never change user preferences or the HFS.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static int
number(const char *s, unsigned *out)
{
	char *end;
	unsigned long value = strtoul(s, &end, 10);
	if (!*s || *end || value == 0 || value > 16384) return 0;
	*out = (unsigned)value;
	return 1;
}

static int
invalidate(const char *path, unsigned width, unsigned height, unsigned scale, unsigned dpi)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return errno == ENOENT ? 0 : 1;
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 1024*1024) {
		close(fd); return 1;
	}
	size_t size = (size_t)st.st_size, done = 0;
	UInt8 *bytes = malloc(size);
	if (!bytes) { close(fd); return 1; }
	while (done < size) {
		ssize_t n = read(fd, bytes + done, size - done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
		done += (size_t)n;
	}
	close(fd);
	CFDataRef data = done == size ? CFDataCreate(NULL, bytes, (CFIndex)size) : NULL;
	free(bytes);
	CFPropertyListRef plist = data ? CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL) : NULL;
	if (data) CFRelease(data);
	int result = 1;
	if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID()) goto end;
	CFTypeRef extra = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("CacheExtra"));
	if (!extra || CFGetTypeID(extra) != CFDictionaryGetTypeID()) goto end;
	/* Obfuscated screen-info key in 20H392: two u32 pixels, pitch, two floats, class. */
	CFTypeRef record = CFDictionaryGetValue((CFDictionaryRef)extra, CFSTR("oBbtJ8x+s1q0OkaiocPuog"));
	if (!record) { result = 0; goto end; } /* no persisted screen answer */
	if (CFGetTypeID(record) != CFDataGetTypeID() || CFDataGetLength(record) != 24) goto end;
	uint32_t old_width, old_height, old_dpi;
	float old_scale;
	const UInt8 *screen = CFDataGetBytePtr(record);
	memcpy(&old_width, screen, 4); memcpy(&old_height, screen + 4, 4);
	memcpy(&old_dpi, screen + 8, 4);
	memcpy(&old_scale, screen + 12, 4);
	if (!old_width || !old_height || !isfinite(old_scale) || old_scale <= 0) goto end;
	if (old_width == width && old_height == height && old_scale == (float)scale && (!dpi || old_dpi == dpi)) {
		printf("display-cache: %ux%u scale %u already matches; retained\n", width, height, scale);
		result = 0; goto end;
	}
	char backup[4096];
	if (snprintf(backup, sizeof(backup), "%s.hakios-display-old", path) >= (int)sizeof(backup)) goto end;
	if (rename(path, backup) != 0) goto end;
	char directory[4096];
	if (strlen(path) >= sizeof(directory)) goto end;
	strcpy(directory, path);
	char *slash = strrchr(directory, '/');
	if (slash) {
		*slash = 0;
		fd = open(*directory ? directory : "/", O_RDONLY | O_DIRECTORY);
		if (fd < 0) goto end;
		result = fsync(fd) != 0; close(fd);
	} else result = 0;
	printf("display-cache: cached %ux%u scale %g -> requested %ux%u scale %u; saved %s; directory sync %s\n",
	    old_width, old_height, (double)old_scale, width, height, scale, backup, result ? "FAILED" : "OK");
end:
	if (plist) CFRelease(plist);
	if (result) fprintf(stderr, "display-cache: unable to validate or preserve %s (errno %d)\n", path, errno);
	return result;
}

int
main(int argc, char **argv)
{
	unsigned width = 0, height = 0, scale = 0, dpi = 0;
	/* Explicit path/geometry is useful for diagnostics on disposable cache copies. */
	if ((argc == 6 || argc == 7) && strcmp(argv[1], "--cache") == 0) {
		if (!number(argv[3], &width) || !number(argv[4], &height) || !number(argv[5], &scale) || scale > 4) return 2;
		if (argc == 7 && (!number(argv[6], &dpi) || dpi < 72 || dpi > 1000)) return 2;
		return invalidate(argv[2], width, height, scale, dpi);
	}
	if (argc != 1) return 2;
	char args[4096]; size_t size = sizeof(args);
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) || size >= sizeof(args)) return 1;
	args[size] = 0;
	for (char *token = strtok(args, " \t"); token; token = strtok(NULL, " \t")) {
		if (!strncmp(token, "fbwidth=", 8)) (void)number(token + 8, &width);
		if (!strncmp(token, "fbheight=", 9)) (void)number(token + 9, &height);
		if (!strncmp(token, "fbscale=", 8)) (void)number(token + 8, &scale);
		if (!strncmp(token, "fbdpi=", 6)) (void)number(token + 6, &dpi);
	}
	if (!width || !height || !scale || scale > 4 || (dpi && (dpi < 72 || dpi > 1000))) {
		puts("display-cache: no explicit display geometry; cache retained"); return 0;
	}
	static const char *paths[] = {
		"/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist",
		"/private/var/mobile/Library/Caches/com.apple.MobileGestalt.plist",
		"/private/var/mobile/Library/Preferences/com.apple.MobileGestalt.plist",
	};
	int result = 0;
	for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) result |= invalidate(paths[i], width, height, scale, dpi);
	return result;
}
