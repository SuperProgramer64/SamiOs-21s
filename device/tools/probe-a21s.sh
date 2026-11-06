#!/system/bin/sh
# Read-only hardware probe for the HakiOS A21s port. Writes nothing to any partition.
sec() { echo; echo "=== $1"; }
dtp() { # print a DT property as hex/str
	f="$1"; [ -e "$f" ] || return
	printf '%s: ' "${f#/sys/firmware/devicetree/base}"
	if tr -d '\0' < "$f" | grep -q '^[[:print:]]*$' 2>/dev/null && [ "$(wc -c < "$f")" -lt 200 ]; then tr '\0' ' ' < "$f"; echo
	else od -A n -t x1 "$f" | tr -s ' \n' ' ' | cut -c1-400; echo; fi
}

sec partitions
ls -l /dev/block/by-name/ | awk '{print $(NF-2), $NF}'
sec sizes
for p in boot recovery dtb dtbo vbmeta userdata super efs sec_efs param up_param keystorage; do
	b=$(readlink -f /dev/block/by-name/$p 2>/dev/null) && [ -e "$b" ] && echo "$p $b $(blockdev --getsize64 $b)"
done
sec storage
ls /dev/block | grep -E '^(mmcblk[0-9]+|sd[a-z])$'
for d in /sys/block/mmcblk0 /sys/block/sda; do [ -d $d ] && { echo $d; cat $d/size; cat $d/device/name $d/device/type $d/device/model 2>/dev/null; readlink -f $d; }; done
sec boot-header
dd if=/dev/block/by-name/boot bs=4096 count=1 2>/dev/null | od -A d -t x4 | head -3
echo "cmdline in header:"; dd if=/dev/block/by-name/boot bs=1 skip=64 count=512 2>/dev/null | tr -d '\0'; echo
echo "header_version=$(dd if=/dev/block/by-name/boot bs=1 skip=40 count=4 2>/dev/null | od -A n -t u4)"
echo "page_size=$(dd if=/dev/block/by-name/boot bs=1 skip=36 count=4 2>/dev/null | od -A n -t u4)"
echo "kernel_size=$(dd if=/dev/block/by-name/boot bs=1 skip=8 count=4 2>/dev/null | od -A n -t u4) kernel_addr=$(dd if=/dev/block/by-name/boot bs=1 skip=12 count=4 2>/dev/null | od -A n -t x4)"
echo "kernel magic at page:"; dd if=/dev/block/by-name/boot bs=4096 skip=1 count=1 2>/dev/null | od -A d -t x1 | head -5
sec recovery-header
dd if=/dev/block/by-name/recovery bs=4096 count=1 2>/dev/null | strings | head -5
sec cmdline
cat /proc/cmdline; echo
sec bootconfig
cat /proc/bootconfig 2>/dev/null
sec iomem
cat /proc/iomem | grep -vE '^ {4}' | head -80
sec memory-dt
for n in /sys/firmware/devicetree/base/memory*; do dtp $n/reg; done
dtp /sys/firmware/devicetree/base/model; dtp /sys/firmware/devicetree/base/compatible
sec reserved-memory
ls /sys/firmware/devicetree/base/reserved-memory 2>/dev/null
sec interrupt-controller
for n in $(find /sys/firmware/devicetree/base -maxdepth 2 -name 'interrupt-controller*' -type d); do echo $n; dtp $n/compatible; dtp $n/reg; done
sec cpus
for n in /sys/firmware/devicetree/base/cpus/cpu@*; do echo "$n $(tr -d '\0' < $n/compatible) method=$(tr -d '\0' < $n/enable-method 2>/dev/null)"; done
dtp /sys/firmware/devicetree/base/psci/compatible
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_available_frequencies 2>/dev/null | sort -u
sec timer
for n in $(find /sys/firmware/devicetree/base -maxdepth 2 -name 'timer*' -type d); do echo $n; dtp $n/compatible; dtp $n/clock-frequency; done
sec gpu-dt
for n in $(grep -rl -E 'mali|bifrost|arm,mali' /sys/firmware/devicetree/base --include=compatible 2>/dev/null); do
	d=$(dirname $n); echo "## $d"; for f in $d/*; do [ -f "$f" ] && dtp $f; done
done
sec gpu-sysfs
for d in /sys/devices/platform/*mali* /sys/class/misc/mali0/device; do [ -d $d ] && { echo "## $d"; ls $d; for f in gpuinfo core_mask dvfs_table clock vol power_policy gpu_id gpu_model; do [ -f $d/$f ] && echo "$f: $(cat $d/$f 2>/dev/null | head -5)"; done; }; done
ls -l /dev/mali* 2>/dev/null
sec gpu-dmesg
dmesg | grep -iE 'mali|gpu|g3d|bifrost' | head -60
sec display-dt
for n in $(grep -rl -E 'decon|dsim|panel|dsi' /sys/firmware/devicetree/base --include=compatible 2>/dev/null | head -20); do d=$(dirname $n); echo "## $d"; dtp $d/compatible; dtp $d/reg; done
sec display-dmesg
dmesg | grep -iE 'decon|dsim|panel|lcd|mipi' | head -40
cat /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/modes 2>/dev/null
wm size 2>/dev/null; wm density 2>/dev/null
sec touch
cat /proc/bus/input/devices | grep -E 'Name|Sysfs|Handlers'
for n in $(grep -rl -iE 'touch|tsp|ts_|himax|ilitek|novatek|focaltech|goodix|sec,' /sys/firmware/devicetree/base --include=compatible 2>/dev/null | head -10); do d=$(dirname $n); echo "## $d"; dtp $d/compatible; dtp $d/reg; done
dmesg | grep -iE 'tsp|touch|himax|ilitek|nvt|ft5|goodix' | head -30
sec pmic
for n in $(grep -rl -iE 's2mpu|s2mp|pmic|speedy|acpm' /sys/firmware/devicetree/base --include=compatible 2>/dev/null | head -15); do d=$(dirname $n); echo "## $d"; dtp $d/compatible; dtp $d/reg; done
dmesg | grep -iE 's2mpu|pmic|acpm|speedy' | head -20
ls /sys/class/regulator/ | head -60; for r in /sys/class/regulator/regulator.*; do echo "$(cat $r/name 2>/dev/null) $(cat $r/microvolts 2>/dev/null) $(cat $r/state 2>/dev/null)"; done | grep -iE 'g3d|gpu|buck|ldo' | head -40
sec mmc
for n in $(grep -rl -iE 'dw-mshc|dwmmc|sdhci|mmc' /sys/firmware/devicetree/base --include=compatible 2>/dev/null | head -6); do d=$(dirname $n); echo "## $d"; dtp $d/compatible; dtp $d/reg; done
sec uart
for n in $(grep -rl -iE 'uart|serial|usi' /sys/firmware/devicetree/base --include=compatible 2>/dev/null | head -10); do d=$(dirname $n); echo "## $d"; dtp $d/compatible; dtp $d/reg; done
sec keys
getevent -pl 2>/dev/null | grep -E 'add device|name|KEY_(POWER|VOLUME)' | head -30
sec pstore
ls -l /sys/fs/pstore/ 2>/dev/null; for n in /sys/firmware/devicetree/base/reserved-memory/*ramoops* /sys/firmware/devicetree/base/reserved-memory/*pstore*; do [ -d $n ] && { echo $n; dtp $n/reg; }; done
sec recovery-twrp
dd if=/dev/block/by-name/recovery bs=1M count=32 2>/dev/null | strings | grep -iE 'twrp|orangefox|teamwin' | head -3
sec done
