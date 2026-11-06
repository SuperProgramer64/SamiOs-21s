# Wi-Fi and Bluetooth: next bring-up priority

**Status: investigation and platform mapping, not working connectivity.** Work on the radio driver was paused at the user's request to prepare this source publication. The installed v11 touch candidate does not activate a new radio driver.

## Confirmed platform

Android exposes the A21s radios through Samsung **SCSC/SLSI**, with platform identifier `nacho_s612_a21`. WLAN and Bluetooth use the shared WLBT subsystem, not the Qualcomm WCN6750 driver currently present in the generic common tree.

Hardware facts derived from the phone's configuration:

| Resource | Address / value |
|---|---|
| AP↔WLBT mailbox | `0x119c0000`, register span `0x180` |
| PMU | `0x11860000`, span `0x10000` |
| I3C APM PMIC | `0x11a50000`, span `0x1800` |
| DBUS BAAW | `0x14c20000`, span `0x300` |
| PBUS BAAW | `0x14c30000`, span `0x300` |
| WLBT remap | `0x14c50000`, span `0x500` |
| Boot configuration | `0x14c60000`, span `0x1200` |
| GIC SPI mailbox / alive / configuration request / watchdog | `54 / 38 / 230 / 231` (GIC interrupt IDs are SPI+32) |
| Requested shared DRAM | 12 MiB; dynamically reserved, **no fixed base was established** |

Android uses the `scsc_platform_mif`, `scsc_mx`, `scsc_wlan` and `scsc_bt` modules. Its vendor directory contains the `mx140`/`mx140_t` firmware families and platform-specific HCF files. None of these vendor files are distributed here.

## Startup work identified

Both radios depend on a common sequence: validate the actual firmware, allocate/reserve suitable shared RAM, initialize management transport, release/reset the subsystem with the correct PMU handshake, handle CFG_REQ, perform the secure-monitor configuration, program memory translation, load the boot-controller patch, acknowledge configuration, and receive the firmware's startup indication. WLAN HIP/data-plane services and Bluetooth shared-memory/HCI transport come after this base.

A correct implementation must establish cache coherency, memory bounds, timeouts, IRQ ownership and cleanup. Register addresses alone are not a working driver. Do not point a mapping at an arbitrary unreserved physical address or apply MediaTek/Qualcomm power writes.

Read-only reference: [Samsung Exynos850 platform source, pinned revision](https://github.com/samsungexynos850/android_kernel_samsung_exynos850/tree/b8c71b7aecece9c67f0e840a6b074573e8041d20/drivers/misc/samsung/scsc). Its register descriptions and ordering were investigated; its GPL implementation is not copied into this repository's APSL/MIT driver sources.

## Next evidence needed

Capture the active firmware variant, HCF files and board-specific boot patch from the owner's Android installation, preserving hashes outside the repository. Compare the running hardware state with the documented startup order. Implement the common WLBT transport behind its own boot argument and validate firmware startup before advertising WLAN/Bluetooth availability to iOS.
