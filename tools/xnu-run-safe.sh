#!/system/bin/sh
# One run. Missing/ambiguous USB state never authorizes a restart.
# The UDC state is required in addition to the charging/VBUS indication.
usb_state() {
    online=$(cat /sys/class/power_supply/usb/online 2>/dev/null) || { echo unknown; return; }
    udc=$(cat /sys/class/udc/13600000.dwc3/state 2>/dev/null) || { echo unknown; return; }
    case "$online" in 0|1) ;; *) echo unknown; return ;; esac
    case "$udc" in
        'not attached')
            if [ "$online" = 0 ]; then echo disconnected; else echo connected; fi ;;
        attached|powered|reconnecting|unauthenticated|default|addressed|configured|suspended)
            echo connected ;;
        *) echo unknown ;;
    esac
}
if [ "${1:-}" = --check ]; then
    state=$(usb_state)
    echo "$state"
    case "$state" in disconnected) exit 0 ;; connected) exit 1 ;; *) exit 2 ;; esac
fi
# A launch is armed over ADB only while a real USB connection is observable.
if [ "$(usb_state)" != connected ]; then
    echo 'Refused: no confirmed initial USB connection.'
    exit 1
fi
echo 'Armed: waiting 30 seconds, then 10 consecutive seconds with USB absent.'
sleep 30
absent=0
previous=
for elapsed in $(seq 1 600); do
    state=$(usb_state)
    if [ "$state" != "$previous" ]; then echo "USB: $state"; previous=$state; fi
    if [ "$state" = disconnected ]; then absent=$((absent + 1)); else absent=0; fi
    if [ "$absent" -ge 10 ]; then
        sleep 1
        if [ "$(usb_state)" = disconnected ]; then
            echo 'USB absent and rechecked; requesting one XNU boot.'
            echo HAKIOS:BOOT-XNU > /dev/pmsg0 || exit 1
            reboot
            exit 0
        fi
        absent=0
    fi
    sleep 1
done
echo 'Expired without a confirmed USB disconnect; no restart requested.'
exit 1
