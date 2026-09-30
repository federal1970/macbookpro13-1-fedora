#!/usr/bin/env bash
# Test plan for Jordan Brough's applesmc charge-limit series (v3 candidate)
# on MacBookPro13,1 (SBS battery ACPI0002:00, no BFCL key). Run as root on
# the 7.2.7-jordantest kernel built with the series. Writes a transcript to
# the file given as $1 (default ~/dev/kernel-test/v3-test.log).
# The charge limit is restored to $KEEP (default 80) at the end.
log=${1:-/home/${SUDO_USER:-user}/dev/kernel-test/v3-test.log}
KEEP=${KEEP:-80}
A=/sys/class/power_supply/BAT0/charge_control_end_threshold
exec > >(tee "$log") 2>&1
say() { printf '\n### %s\n' "$*"; }
rd() { if [ -e "$A" ]; then cat "$A" 2>&1 | tr -d '\n'; echo; else echo "(file absent)"; fi; }
wr() { echo "$1" > "$A" 2>/dev/null; echo "write $1 -> rc=$?"; }
mark=$(journalctl -k -o cat --no-pager | wc -l)
dm() { journalctl -k -o short-iso --no-pager | tail -n +$((mark+1)) | grep -E 'battery|applesmc|hook|BCLM|BFCL|MagSafe' | cut -c12-; mark=$(journalctl -k -o cat --no-pager | wc -l); }

say "kernel and module provenance"
uname -r; grep -oE 'mem_sleep_default=[a-z]+' /proc/cmdline
modinfo -n applesmc; dkms status 2>/dev/null | grep -E "$(uname -r)" || echo "no dkms modules for this kernel"
grep -E '^CONFIG_(ACPI_BATTERY_HOOKS|ACPI_BATTERY|ACPI_SBS|SENSORS_APPLESMC)=' /boot/config-$(uname -r)
cat /sys/class/power_supply/BAT0/device/hid; ls /sys/bus/platform/drivers/acpi-sbs/ | grep ACPI0002 || true
ls /lib/modules/$(uname -r)/kernel/drivers/acpi/ | grep -E 'battery|sbs' | tr '\n' ' '; echo

say "fresh load: modprobe applesmc"
modprobe -r applesmc 2>/dev/null; sleep 1; modprobe applesmc; sleep 1
echo "attribute after modprobe: $(rd)"; ls /sys/class/power_supply/BAT0/extensions/ 2>/dev/null; dm

say "full range with both boundaries: 20 21 50 99 100"
for v in 20 21 50 99 100; do wr $v; echo "  read-back: $(rd)"; done; dm

say "out of range: 10 and 101 must fail with EINVAL and leave the value unchanged"
wr 50; echo "  baseline: $(rd)"
for v in 10 101; do wr $v; echo "  read-back: $(rd)"; done; dm

say "same value twice in a row must be a no-op, not an error"
wr 75; wr 75; echo "  read-back: $(rd)"; dm

say "uevent and upower view"
grep THRESHOLD /sys/class/power_supply/BAT0/uevent
upower -i /org/freedesktop/UPower/devices/battery_BAT0 2>/dev/null | grep -iE 'threshold' || echo "(upower not showing thresholds; needs a restart to re-read)"

say "rmmod applesmc: attribute must disappear, hook unregistered"
modprobe -r applesmc; sleep 1; echo "attribute after rmmod: $(rd)"; dm
modprobe applesmc; sleep 1; echo "attribute after modprobe again: $(rd)"; dm

say "unbind/rebind the SBS battery device (platform driver acpi-sbs) with the hook registered"
D=/sys/bus/platform/drivers/acpi-sbs
dev=$(ls $D 2>/dev/null | grep ACPI0002 | head -1)
if [ -n "$dev" ]; then
  echo "$dev" > $D/unbind; sleep 2
  echo "after unbind: BAT0 dir $( [ -d /sys/class/power_supply/BAT0 ] && echo present || echo gone )"; dm
  echo "$dev" > $D/bind; sleep 3
  echo "after bind: attribute $(rd)"; ls /sys/class/power_supply/BAT0/extensions/ 2>/dev/null; dm
else
  echo "no ACPI0002 device under $D ?!"; ls $D 2>&1
fi

say "any BFCL / MagSafe warning at all this boot (must be none on this machine)"
journalctl -k -b --no-pager | grep -ciE 'BFCL|MagSafe' || true

say "restore the limit to $KEEP"
wr $KEEP; echo "final: $(rd)"
echo; echo "transcript: $log"
