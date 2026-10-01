#!/usr/bin/env bash
# Hibernation bisect with brcmfmac LOADED (no hook: this goes straight to
# the kernel, systemd and its sleep hooks are not involved). Root.
#   hib-bisect.sh devices      freeze + thaw devices only, no image, no power-off
#   hib-bisect.sh test_resume  write the real image, read it back and restore
#                              it in this same boot, no power-off
# Both return by themselves; a power-off instead is the result we are after.
mode=${1:?usage: $0 devices|test_resume}
log=/home/${SUDO_USER:-user}/dev/kernel-test/hib-bisect.log
exec > >(tee -a "$log") 2>&1
echo "### $(date '+%F %T') kernel $(uname -r) mode=$mode boot $(cat /proc/sys/kernel/random/boot_id)"
lsmod | grep -q '^brcmfmac ' && echo "brcmfmac loaded, wlp2s0 $(nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -o 'wlp2s0:.*')" || { echo "brcmfmac NOT loaded; load it first"; exit 1; }
echo 1 > /sys/power/pm_debug_messages
case $mode in
  devices)     echo devices > /sys/power/pm_test; echo platform > /sys/power/disk ;;
  test_resume) echo none > /sys/power/pm_test; echo test_resume > /sys/power/disk ;;
  *) echo "bad mode"; exit 1 ;;
esac
echo "pm_test: $(cat /sys/power/pm_test)"; echo "disk: $(cat /sys/power/disk)"; echo "resume dev: $(cat /sys/power/resume)"
mark=$(journalctl -k -o cat --no-pager | wc -l)
sync; sync
echo "--- entering: echo disk > /sys/power/state ($(date '+%T'))"
echo disk > /sys/power/state; rc=$?
echo "--- back ($(date '+%T')) rc=$rc"
sleep 3
journalctl -k -o short-iso --no-pager | tail -n +$((mark+1)) | grep -iE 'PM:|hibernat|brcmf|02:00.0|wlp2s0|pcieport|snapshot|image' | cut -c12-150 | head -40
echo none > /sys/power/pm_test; echo platform > /sys/power/disk; echo 0 > /sys/power/pm_debug_messages
echo "restored: pm_test $(cat /sys/power/pm_test | grep -o '\[.*\]'), disk $(cat /sys/power/disk | grep -o '\[.*\]')"
echo "wifi now: $(nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -o 'wlp2s0:.*'); link: $(cat /sys/bus/pci/devices/0000:02:00.0/current_link_speed 2>&1)"
