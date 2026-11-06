# A21s device-specific loader

The loader source is in `loader/`. The address shown by `xnusys.py plan` differs between phones. The earlier image used fixed values for one device; it cannot find a payload staged at another device's location.

This update reads the target layout and rebuilds the loader with its own LBA and system-window size. It is a build-time configuration: repeat the procedure if the partition layout changes. Persistent `/private/var` and the eMMC log require their own validated per-device windows; do not reuse values from a different phone.

Requirements: unlocked Galaxy A21s, root ADB in Android/recovery, a private backup of that phone's boot image, a locally supplied iOS identity header, clang, an AArch64 linker and the project build dependencies. The resulting image includes your own Android boot image and must not be redistributed.

```sh
export HAKIOS_SERIAL=your-device-serial
python3 device/tools/xnusys.py plan --serial "$HAKIOS_SERIAL"
python3 device/tools/build-for-device.py --serial "$HAKIOS_SERIAL" --android-boot /path/to/your-backup-boot.img
```

The output is `device/loader/exyloader.img`. Separately prepare the matching system and XNUP payload from your own iOS image and firmware. Use the guarded `xnusys.py` commands after reviewing their plan and preserving existing data. This package does not contain Apple images, device firmware or a universal prebuilt boot image. The build does not flash or reboot the phone.

A stall at `Initializing serial KDP` after a payload is found is a separate early-XNU issue; the LBA fix alone is not evidence that it is resolved.
