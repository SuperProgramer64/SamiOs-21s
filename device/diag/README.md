# a21sdiag: first diagnostic for the Galaxy A21s

Not HakiOS yet: a small program that runs **before** Android's kernel, inside `boot`, to find
out on the real phone what the next steps depend on:

1. that Samsung's S-Boot runs a chained kernel at all, **where** it loads it, and at which EL;
2. the CPU's 16K-granule and PA-size bits (XNU), the timer's frequency, the GIC-400;
3. whether the GPU's power domain is still on when S-Boot hands over, and if so the Mali's
   `GPU_ID` and core masks (read only: nothing is powered or clocked);
4. the display path: DECON/DSIM/DPP registers into the log, coloured bars on S-Boot's
   framebuffer (`0xfa000000`; which bar shows which colour gives the byte order);
5. that the ramoops pmsg zone carries text to Android.

After 10 seconds it copies Android's kernel down to where S-Boot put it and jumps to it, so
the phone comes back to Android. **It writes no partition.**

## Build

Needs clang (Xcode's works) and an ELF `ld.lld` (`brew install lld`, or zig's `zig ld.lld`).

```sh
ANDROID_BOOT=/path/to/your/stock/boot.img ./build.sh
```

`build.sh` refuses any absolute relocation: the program must run wherever S-Boot loads it.
`GZIP=1` packs the kernel gzip-compressed. **S-Boot does not boot that** (tested: a boot loop,
no bars); do not flash a `GZIP=1` image except to repeat the test.

## Try it (TWRP)

Always pass the serial (`adb -s`): a second phone may be connected.

1. Keep your stock `boot.img` (and `recovery.img`, `dtb.img`) **off the phone**, on the Mac.
2. Push the image:
   ```sh
   adb -s "$HAKIOS_SERIAL" push a21sdiag.img /sdcard/a21sdiag.img
   ```
3. Reboot into **TWRP** (Volume Up + Power with the cable in).
4. TWRP → **Install** → **Install Image** → `a21sdiag.img` → partition **Boot**.
5. Reboot to system. The bars and the text should appear over the Samsung logo. After 10 s it
   says `chaining to Android` and **the screen then stays exactly like that** while Android
   boots underneath: the "logo" phase is S-Boot's framebuffer, which a21sdiag painted over, and
   it stays on screen until the boot animation starts. On a ROM that recompiles ART at boot
   (`odsign`/`odrefresh`) that is **2–3 minutes**. Wait; do not force a restart (the first run
   was reset at 63 s while Android was fine, see `/proc/last_kmsg` below).
6. Read the log:
   ```sh
   adb -s "$HAKIOS_SERIAL" shell "su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" | strings > a21sdiag-pmsg.txt
   ```
7. Put the stock `boot` back the same way (TWRP → Install Image → your `boot.img` → Boot), or
   leave a21sdiag in: it always returns to Android.

## If something goes wrong

- Black screen or boot loop: hold **Volume Down + Power** ~10 s to force a restart, then go
  straight into TWRP (**Volume Up + Power**, cable in) and flash the stock `boot.img` to **Boot**.
- Download mode (Odin) also works as a last resort: the stock firmware's `AP` holds `boot`.
- Never flash `efs`, `sec_efs`, `param`, `up_param`, `keystorage`, `vbmeta*` or anything else.

## What the result means

| seen | means |
| --- | --- |
| bars on screen, Android comes back | S-Boot runs the chain, the framebuffer is scanned out (video mode, or command mode with TE still running) |
| no bars, but `A21SDIAG` in `pmsg-ramoops-0` | the chain runs; the panel needs a trigger (command mode), as on the A32 |
| no bars and no log, phone boots Android normally | S-Boot did not run our code (it may check the kernel), or the log zone moved |
| phone stuck on the logo, nothing drawn | the program faulted before the screen; force a restart, flash stock `boot` |
| stuck on `chaining to Android` for > 4 min | Android did not come up: force a restart (Volume Down + Power), boot TWRP, and read `/proc/last_kmsg` there (Samsung keeps the previous kernel's log; a forced restart shows as `sec_hard_reset_hook`) |
| `start=` | where S-Boot loads the kernel: the address `mtkloader`'s Exynos successor relocates from |
| `EL=1` | S-Boot/the hypervisor keeps EL2; XNU runs at EL1 anyway |
| `PD_G3D status` bit 0 set, `GPU_ID=7212....` | the GPU is powered and clocked at hand-over: the Mali bring-up can start at step 4 |
