#!/usr/bin/env bash
# Keyboard-backlight-across-suspend test for the applespi patch. Root.
# Puts the machine into s2idle for 30 s with the lid OPEN (s2idle wakes on
# the RTC alarm; deep would not, and deep cuts the keyboard power anyway),
# with the backlight set bright. Watch the keys:
#   stock kernel  -> backlight stays lit through the sleep
#   patched kernel -> goes dark at suspend, same level again on wake
# Restores mem_sleep=deep and the previous brightness afterwards.
L=/sys/class/leds/spi::kbd_backlight/brightness
log=/home/${SUDO_USER:-user}/dev/kernel-test/kbd-backlight-test.log
exec > >(tee -a "$log") 2>&1
echo "### $(date '+%F %T') kernel $(uname -r)"
grep -c LED_CORE_SUSPENDRESUME /lib/modules/$(uname -r)/build/drivers/input/keyboard/applespi.c 2>/dev/null | sed 's/^/flag in build tree source: /' || true
prev_sleep=$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/power/mem_sleep); prev_bl=$(cat $L)
echo "mem_sleep was $prev_sleep, brightness was $prev_bl"
echo s2idle > /sys/power/mem_sleep
echo 200 > $L; sleep 1; echo "brightness set to $(cat $L); sleeping 30 s in $(cat /sys/power/mem_sleep) -- WATCH THE KEYBOARD"
mark=$(journalctl -k -o cat --no-pager | wc -l)
rtcwake -m mem -s 30 >/dev/null 2>&1; rc=$?
sleep 2
echo "woke (rtcwake rc=$rc); brightness now $(cat $L)"
journalctl -k -o short-iso --no-pager | tail -n +$((mark+1)) | grep -E 'suspend entry|suspend exit|applespi|kbd_backlight|s2idle' | cut -c12-120
echo "$prev_sleep" > /sys/power/mem_sleep; echo "$prev_bl" > $L
echo "restored: mem_sleep $(cat /sys/power/mem_sleep), brightness $(cat $L)"
