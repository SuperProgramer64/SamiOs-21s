#!/system/bin/sh
# Second read-only pass: device-tree nodes by compatible, panel, touch, recovery.
sec() { echo; echo "=== $1"; }
hex() { od -A n -t x1 "$1" | tr -s ' \n' ' ' | cut -c1-300; }
show() { d="$1"; echo "## ${d#/sys/firmware/devicetree/base}"; for f in "$d"/*; do [ -f "$f" ] || continue; n=$(basename "$f"); case "$n" in name|phandle|linux,phandle) continue;; esac
	if [ "$(wc -c < "$f")" -lt 160 ] && ! od -A n -t x1 "$f" | grep -qE ' (0[1-9a-f]|1[0-9a-f]|[89a-f][0-9a-f])' ; then echo "  $n = \"$(tr '\0' ' ' < "$f")\""; else echo "  $n = $(hex "$f")"; fi; done; }
find /sys/firmware/devicetree/base -name compatible > /data/local/tmp/compat.lst
match() { while read f; do if tr '\0' '\n' < "$f" | grep -qiE "$1"; then dirname "$f"; fi; done < /data/local/tmp/compat.lst; }

sec gpu
for d in $(match 'mali|bifrost'); do show $d; done
sec gic
for d in $(match 'arm,gic|gic-400|gic-v3|cortex-a15-gic'); do show $d; done
sec arch-timer
for d in $(match 'armv8-timer|armv7-timer'); do show $d; done
sec pmu-g3d-powerdomain
for d in $(match 'pd|power-domain|exynos-pd'); do case $d in *g3d*|*G3D*|*gpu*) show $d;; esac; done
for d in $(match 'exynos-pd'); do echo "$d $(tr -d '\0' < $d/pd-name 2>/dev/null)"; done | head -30
sec cmu
for d in $(match 'cmu|clock-controller'); do echo "$d $(tr '\0' ' ' < $d/compatible)"; done | head -20
sec decon-dsim
for d in $(match 'decon|dsim|dpp'); do show $d; done 2>/dev/null | head -120
sec panel
cat /sys/class/lcd/panel/lcd_type /sys/class/lcd/panel/window_type 2>/dev/null
ls /sys/class/lcd/ 2>/dev/null
dmesg | grep -iE 'panel.*(name|id)|lcd.*id|mcd-panel|panel_drv' | head -15
sec touch
for d in $(match 'tsp|touch|ilitek|himax|novatek|nvt|ft8|focaltech|sec,'); do case $d in *spi*|*i2c*|*tsp*|*touch*) show $d;; esac; done 2>/dev/null | head -60
ls /sys/class/sec/tsp 2>/dev/null | head; cat /sys/class/sec/tsp/chip_name /sys/class/sec/tsp/fw_ver_ic 2>/dev/null
cat /sys/bus/spi/devices/spi1.0/modalias 2>/dev/null; readlink /sys/bus/spi/devices/spi1.0/driver
sec gpio-keys
for d in $(match 'gpio-keys'); do show $d; for k in $d/*/; do [ -d $k ] && show ${k%/}; done; done
sec ramoops
for d in $(match 'ramoops'); do show $d; done
ls /sys/firmware/devicetree/base/reserved-memory/ | grep -iE 'oops|pstore|log|debug'
for n in debug_snapshot bootloader_fb el3mon_reserve; do [ -d /sys/firmware/devicetree/base/reserved-memory/$n ] && show /sys/firmware/devicetree/base/reserved-memory/$n; done
sec uart
for d in $(match 'uart|usi'); do echo "$d $(tr '\0' ' ' < $d/compatible) reg=$(hex $d/reg 2>/dev/null)"; done | head -10
sec mmc
for d in $(match 'dw-mshc|dwmmc|exynos.*mmc'); do echo "$d $(tr '\0' ' ' < $d/compatible) reg=$(hex $d/reg 2>/dev/null)"; done
sec recovery
dd if=/dev/block/by-name/recovery bs=2048 count=1 2>/dev/null | od -A d -t x4 | head -3
mkdir -p /data/local/tmp/rec && cd /data/local/tmp/rec
rs=$(dd if=/dev/block/by-name/recovery bs=1 skip=16 count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
ks=$(dd if=/dev/block/by-name/recovery bs=1 skip=8 count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
echo "recovery kernel=$ks ramdisk=$rs"
off=$(( (2048 + ks + 2047) / 2048 ))
dd if=/dev/block/by-name/recovery bs=2048 skip=$off count=$(( (rs + 2047) / 2048 )) 2>/dev/null > rd
head -c 6 rd | od -A n -t x1
(gzip -dc rd 2>/dev/null || lz4 -dc rd 2>/dev/null || xz -dc rd 2>/dev/null) | strings | grep -iE 'twrp|teamwin|orangefox|ro.twrp|ro.build.version' | sort -u | head -8
cd /; rm -rf /data/local/tmp/rec
sec boot-kernel
ks=$(dd if=/dev/block/by-name/boot bs=1 skip=8 count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
dd if=/dev/block/by-name/boot bs=2048 skip=1 count=1 2>/dev/null | od -A d -t x1 | head -5
dd if=/dev/block/by-name/boot bs=1 skip=$((2048+0x38)) count=4 2>/dev/null; echo " <- magic at 0x38"
echo "kernel bytes=$ks"; zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_ARM64_VA_BITS=|CONFIG_PSTORE|CONFIG_ARM_GIC' | head
rm -f /data/local/tmp/compat.lst
sec done
