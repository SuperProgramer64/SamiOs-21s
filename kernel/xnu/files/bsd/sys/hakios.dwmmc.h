/*
 * The Exynos 850's eMMC as read-only /dev/emmc0; gated /dev/emmc1 is a disjoint HFS+ data window.
 */
#ifndef _SYS_HAKIOS_DWMMC_H_
#define _SYS_HAKIOS_DWMMC_H_

#include <stdbool.h>
#include <sys/types.h>

__BEGIN_DECLS
dev_t hakios_dwmmc_root(void);      /* IOFindBSDRoot, rd=emmc0: the device, or NODEV */
void hakios_dwmmc_devfs(void);      /* after devfs: emmc0/remmc0; gated emmc1/remmc1 */
bool hakios_dwmmc_dev(dev_t dev);   /* one of its devices (APFS: its requests are not free) */
__END_DECLS

#endif
