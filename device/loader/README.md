# exyloader: the A21s's loader

The Exynos 850 successor of `hakios-a32`'s `mtkloader`. It sits in `boot` in front of
Android's kernel (`tools/chainboot.py`), S-Boot starts it as Linux (0x80080000, EL1, MMU and
D-cache off), and it is built in numbered steps, read only first, each saying its result in the
log. After the steps it gives the phone back to Android.

| step | what | state |
| --- | --- | --- |
| 1 | the eMMC from bare metal: DW MMC 0 (`0x12100000`) by PIO (`dwmmc.h`), the MBR, the GPT header and entries (`boot`, `super` by name), `boot`'s first block, the first block of the `xnusys` region, 1 MiB timed | **works** (2026-11-04): 5/5, 4972 KiB/s with single-block reads |
| 2 | the XNU payload (`xnuloader/mkpayload.py`: kernel + RAM disk) off the eMMC, from the last 64 MiB of the `xnusys` region (LBA 20160512, `tools/xnusys.py payload`) to `0x8c0000000` | written, not run |
| 3 | the kernel's segments at `0x880000000` (XNU's 2 GiB: no firmware reserves anything there), the RAM disk after it as md0, XNU's device tree (`xnu.h`: iPhone X identity from your IPSW, `/arm-io/gic` = `arm,gic-400`, the CPUs by Aff1, the panic log at `0xc1000000`, `uart0` a page of RAM whose text goes to `loader_log`), `boot_args` (the boot framebuffer `0xfa000000` 720×1600, `-v`) | written, not run |
| 4 | a note for the next boot (where `loader_log` is), the watchdogs off, the screen cleared, the caches invalidated, the jump at EL1 | written, not run |

**XNU only when Android asks for it**: with STEP ≥ 2 the loader boots Android as always, unless the
pmsg zone holds `HAKIOS:BOOT-XNU` (written by Android, one run per request). The next boot after an
XNU run (leave it with Volume Down + Power, which keeps the RAM) puts the start and the end of that
run's kernel log into `pmsg-ramoops-0`, then boots Android.

## The first XNU boot

```sh
STEP=4 ANDROID_BOOT=/path/to/your/stock/boot.img ./build.sh
# the payload: the kernel built with ../kernel/, and hello (one program that prints) as md0
python3 ~/hakios-xnu-kernel/xnuloader/mkpayload.py -o payload-hello.bin \
    kernel=~/hakios-xnu-kernel/hakios-kernel ramdisk=~/hakios-xnu-kernel/prebuilt/userland/hello
python3 ../tools/xnusys.py payload payload-hello.bin --serial "$HAKIOS_SERIAL" --i-understand
```

Flash `exyloader.img` to Boot (TWRP), boot Android, then ask for one run:

```sh
adb -s "$HAKIOS_SERIAL" shell "su -c 'echo HAKIOS:BOOT-XNU > /dev/pmsg0'" && adb -s "$HAKIOS_SERIAL" reboot
```

The loader reads the payload (~3 s), says what it loaded and jumps; the kernel's text console (`-v`)
then fills the screen. Leave with Volume Down + Power; the next boot is Android with the run's log in
`pmsg-ramoops-0`. Boot arguments: `debug=0x8 serial=3 rd=md0 cpus=1 mali=2 -v` (+ a payload `cmdline`).

## Build and run step 1

```sh
ANDROID_BOOT=/path/to/your/stock/boot.img ./build.sh        # STEP=1 by default
adb -s "$HAKIOS_SERIAL" push exyloader.img /sdcard/hakios/exyloader.img
```

Flash it to **Boot** from TWRP (Install → Install Image), reboot to system, and wait for
Android (2–3 minutes on a ROM that recompiles ART: the screen keeps the loader's text all that
time). Then:

```sh
adb -s "$HAKIOS_SERIAL" shell "su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" | strings
```

Step 1 should end in `step 1: 5/5 passed` and `eMMC: OK`, with the read speed on the line
before. The first run's log:

```
CTRL=00000010 BMOD=00000200 CTYPE=00010000      S-Boot reads by PIO too (no IDMAC); 8-bit bus
CLKDIV=00000001 CLKENA=00000001 STATUS=00008906
VERID=5342270a HCON=08e42541                    DW MMC 2.70a
FIFO 64 bits at +200, hold reg
CMD13 rca=1 R1=00000900 state=4 (transfer)      the card as S-Boot left it
LBA 0: MBR 55 aa  OK
LBA 1: GPT  OK  entries at 2, 128 x 128
GPT: boot at 282624, super at 737280 +19554304  OK
boot: ANDROID!  OK
xnusys region inside super  OK                  first block zeros: no system.apfs yet
1 MiB in 205 ms = 4972 KiB/s                    XNU's 16 MiB kernel: ~3.4 s this way
step 1: 5/5 passed
```

Back to normal: flash your stock `boot.img` to Boot the same way.
