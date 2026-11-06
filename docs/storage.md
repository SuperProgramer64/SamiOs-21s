# Storage, persistence and logs

The code allows writes only within validated reservations. Never copy these example offsets to another layout without running `device/tools/xnusys.py` planning and verification against that phone.

The tested layout used:

| Region | Absolute 512-byte LBA | Length |
|---|---:|---:|
| xnusys reservation | 12,216,320 | 8,075,264 sectors |
| Journaled HFS+ data | 17,068,032 | 3,092,480 sectors |
| Payload reservation | 20,160,512 | 64 MiB reserved; payload limit 60 MiB |
| Log reservation | 20,283,392 | 8,192 sectors (4 MiB) |

The system APFS measured 2,482,311,168 bytes. The HFS+ data volume measured 1,583,349,760 bytes. These measurements are descriptive, not installation authorization for another device.

The logical-partition table is re-read before staging. `xnusys.py` rejects overlap with allocated Android extents, unsuitable capacity and incompatible geometry. Initial data creation is a one-time operation; existing HFS primary or backup headers prevent accidental reinitialization. Kernel payload updates must preserve that volume and the existing system image.

`emmc_multi=1` enables CMD18 reads with checked fallback. Writable data uses `emmc_write=1`, the validated `emmc_var_lba`/`emmc_var_sectors`, and `var_hfs=emmc1`. eMMC log arguments identify their separate window. The driver includes flush handling and tests for rejected out-of-window requests.

The latest recovered v10 log had 773 valid records, no invalid records, and retained the complete approximately ten-minute run within the reservation. No kernel panic was found. Raw logs and phone identifiers stay private.

## Restart behavior

`tools/xnu-run-safe.sh` is a one-run gate: it must initially observe USB connected, waits 30 seconds, and then requires 10 consecutive seconds with both VBUS/charging and the USB controller reporting disconnection. Unknown state never permits a restart. It writes the existing loader's one-boot marker only after that gate succeeds.

The installed test loader had a 180-minute watchdog. Ordinary iOS restart behavior should not be assumed to request XNU automatically; the explicit one-boot marker controls that route. Holding both volume keys for about three seconds is the log-preserving exit implemented for this device. Forced hardware reset can lose the end of a log.
