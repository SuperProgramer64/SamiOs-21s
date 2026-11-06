# Historical boot investigation and handoff

English consolidation of the earlier 4–5 November notes. The top-level README and current docs supersede earlier status and installation recipes.

The target is the Galaxy A21s SM-A217F/M, Exynos 850, eight Cortex-A55 cores, GIC-400 and Mali-G52, running XNU 8796.121.2 with an owner-supplied iPhone X 20H392 userspace. A custom Android ROM changes the advertised model; confirm Exynos hardware and partition layout instead. Always supply an explicit local ADB serial.

## Early boot findings

S-Boot loads the raw Image at 0x80080000, EL1, with MMU and data cache off. The Exynos MPIDR uses Aff1 for the core index. The virtual timer is PPI 27; both virtual and physical timer delivery were checked before blaming an early stop at `Initializing serial KDP`.

The early watchdog setup was incomplete: PMU 0x11860000, CLUSTER0_NONCPU_OUT +0x1220 bit 7 enables the counter; CLUSTER0_NONCPU_INT_EN +0x1244 bit 2 permits reset. PMU RST_STAT +0x0404 bit 24 records a watchdog reset. Watchdog feeding duration was extended through successive test profiles; the current installed profile allows 180 minutes.

A forced hardware reset could corrupt retained RAM. An earlier rescue fault dereferenced a corrupted log pointer; the loader added address/length checks before recovering logs. Orderly exits and the bounded eMMC log provide better diagnostics.

## Boot chain

S-Boot → exyloader → payload kernel at 0x880000000 → stage0/init → Apple launchd → backboardd/SpringBoard. The payload ramdisk is mounted as md1 when the system image comes from eMMC. The root presents `rd=md0 emmc_root=1`; presenting an ordinary real-iPhone root triggered unavailable SEP/keybag behavior in earlier tests.

The one-boot marker is `HAKIOS:BOOT-XNU`. Missing marker means Android. Android shutdown can overwrite pmsg, so the marker must be written immediately before the intended reboot. Use the current USB-disconnect gate, not an old ungated command.

## Platform distinctions

The A32 reference is MediaTek; its loader, keypad, PMIC, CPU clocks and display kick are not Exynos drivers. Its Mali family and Panfrost transport were useful references. The A21s display is video mode. `fb_beam=0` avoids accessing Qualcomm display registers on this platform.

The original short runs stopped during the progress bar by design when their watchdog feeding interval expired. Later work addressed migration state, persistent writable data, native display identity, CoreAnimation/wallpapers, TD4150 input and eight-core startup. SpringBoard subsequently reached the physical desktop.

## Repository lessons

Do not mistake a copied older patch or prebuilt init for the active source. Export changes to Apple's files as well as new driver sources. The publication patch has been regenerated from the actual compiled XNU tree, and source/tests are included. Original boot images, phone firmware, logs and system images remain private.

Avoid excessive build parallelism on the development host. Do not attach USB during XNU transition on the affected Mac: its AppleUSBACM path previously panicked. Preserve existing journaled data on every payload update. Do not apply A32 power writes or reinitialize a data filesystem to solve a userspace failure.
