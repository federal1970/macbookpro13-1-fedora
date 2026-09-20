#!/usr/bin/env bash
# Try S3 (deep) once: disarm the wakeup sources mbp-suspend-fix.sh never touched,
# take brcmfmac out of the picture, sleep 60 s, then report who woke us.
# Everything is restored on exit, including on Ctrl-C or an error.
set -u

REPORT=${REPORT:-/var/tmp/deep-test-report.txt}
SLEEP_SECS=${SLEEP_SECS:-60}
TURN_OFF=${TURN_OFF:-"RP05 XHC2 SPIT ARPT"}
MASK_GPE=${MASK_GPE:-}          # e.g. MASK_GPE="gpe07"
MEM_SLEEP=${MEM_SLEEP:-deep}    # deep | s2idle -- s2idle gives the control figure

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
grep -q "$MEM_SLEEP" /sys/power/mem_sleep || { echo "no '$MEM_SLEEP' in /sys/power/mem_sleep" >&2; exit 1; }

# /sys/power/pm_test turns any suspend into a scripted 5-second dry run that
# returns on its own. Left set, it makes every result here meaningless.
if [ -w /sys/power/pm_test ] && ! grep -q '\[none\]' /sys/power/pm_test; then
  echo "pm_test was $(sed 's/.*\[\(.*\)\].*/\1/' /sys/power/pm_test) -- forcing it to none"
  echo none > /sys/power/pm_test
fi
grep -q '\[none\]' /sys/power/pm_test 2>/dev/null || { echo "pm_test is not none, refusing" >&2; exit 1; }

exec > >(tee "$REPORT") 2>&1
chmod 644 "$REPORT" 2>/dev/null

ORIG_MEM_SLEEP=$(sed 's/.*\[\(.*\)\].*/\1/' /sys/power/mem_sleep)
RESTORE_WAKEUP=""
RESTORE_MASK=""
RELOAD_WIFI=0

snap_gpe() { grep -H . /sys/firmware/acpi/interrupts/* 2>/dev/null \
    | sed 's|/sys/firmware/acpi/interrupts/||;s/:[[:space:]]*/ /' | awk '{print $1, $2}'; }

wakeup_is_enabled() { grep -qE "^$1[[:space:]].*\*enabled" /proc/acpi/wakeup; }

cleanup() {
  echo
  echo "--- restoring ---"
  rtcwake -m disable >/dev/null 2>&1
  echo "$ORIG_MEM_SLEEP" > /sys/power/mem_sleep 2>/dev/null
  echo "mem_sleep -> $(cat /sys/power/mem_sleep)"
  for g in $RESTORE_MASK; do
    echo unmask > "/sys/firmware/acpi/interrupts/$g" 2>/dev/null \
      && echo "unmasked $g -> $(awk '{$1="";print}' /sys/firmware/acpi/interrupts/$g)"
  done
  for d in $RESTORE_WAKEUP; do
    wakeup_is_enabled "$d" || { echo "$d" > /proc/acpi/wakeup; echo "re-armed $d"; }
  done
  if [ "$RELOAD_WIFI" = 1 ]; then
    modprobe brcmfmac && echo "brcmfmac reloaded"
    systemctl start NetworkManager && echo "NetworkManager started"
  fi
  echo "report: $REPORT"
}
trap cleanup EXIT INT TERM

echo "=== deep (S3) test, $(date '+%F %T') ==="
echo
echo "pm_test : $(cat /sys/power/pm_test)"
echo "disk    : $(cat /sys/power/disk)"
echo
echo "--- wakeup sources before ---"
grep -v disabled /proc/acpi/wakeup

echo
echo "--- disarming ---"
for d in $TURN_OFF; do
  if wakeup_is_enabled "$d"; then
    echo "$d" > /proc/acpi/wakeup
    RESTORE_WAKEUP="$RESTORE_WAKEUP $d"
    echo "$d -> disabled"
  else
    echo "$d already disabled"
  fi
done

echo
echo "--- wakeup sources after disarming ---"
grep -v disabled /proc/acpi/wakeup || echo "(none left enabled)"

if [ -n "$MASK_GPE" ]; then
  echo
  echo "--- masking GPEs ---"
  for g in $MASK_GPE; do
    f=/sys/firmware/acpi/interrupts/$g
    if [ -w "$f" ]; then
      echo "$g before: $(cat $f)"
      echo mask > "$f" && RESTORE_MASK="$RESTORE_MASK $g"
      echo "$g after : $(cat $f)"
    else
      echo "WARNING: $f not writable"
    fi
  done
fi

echo
echo "--- taking brcmfmac out ---"
systemctl stop NetworkManager && RELOAD_WIFI=1
modprobe -r brcmfmac_wcc 2>/dev/null
if modprobe -r brcmfmac; then echo "brcmfmac unloaded"; else echo "WARNING: could not unload brcmfmac"; fi

B=/sys/class/power_supply/BAT0
BAT_STATUS=$(cat $B/status 2>/dev/null)
CHARGE_BEFORE=$(cat $B/charge_now 2>/dev/null || echo 0)
VOLT_BEFORE=$(cat $B/voltage_now 2>/dev/null || echo 0)
TEMP_BEFORE=$(cat $B/temp 2>/dev/null || echo "")
if [ "$BAT_STATUS" != "Discharging" ]; then
  echo
  echo "NOTE: battery status is '$BAT_STATUS' -- unplug the charger for a drain figure."
fi

snap_gpe > /tmp/.gpe-before
BEFORE_BOOT=$(awk '{print $1}' /proc/uptime)
BEFORE_WALL=$(date +%s)

echo
echo "--- going to sleep: $MEM_SLEEP, ${SLEEP_SECS}s RTC alarm as a safety net ---"
echo "$MEM_SLEEP" > /sys/power/mem_sleep
echo "mem_sleep = $(cat /sys/power/mem_sleep)"
rtcwake -m no -s "$SLEEP_SECS" >/dev/null 2>&1 || echo "WARNING: could not arm RTC alarm"
sync
echo mem > /sys/power/state

AFTER_BOOT=$(awk '{print $1}' /proc/uptime)
AFTER_WALL=$(date +%s)
snap_gpe > /tmp/.gpe-after

echo
echo "=== woke up ==="
printf 'asked for   : %s s\n' "$SLEEP_SECS"
printf 'wall clock  : %s s\n' "$((AFTER_WALL - BEFORE_WALL))"
printf 'uptime delta: %s s  (counts suspended time on this kernel)\n' \
  "$(awk -v a="$AFTER_BOOT" -v b="$BEFORE_BOOT" 'BEGIN{printf "%.1f", a-b}')"

echo
echo "--- verdict ---"
if journalctl -k --no-pager --since "@$BEFORE_WALL" 2>/dev/null | grep -q 'suspend debug: Waiting'; then
  echo "VOID -- pm_test was active, this was a scripted dry run, not a real suspend."
elif ! journalctl -k --no-pager --since "@$BEFORE_WALL" 2>/dev/null | grep -q 'sleep state S3'; then
  echo "WARNING -- no 'ACPI: PM: Preparing to enter system sleep state S3' in the log;"
  echo "           check that this really was deep and not s2idle."
fi
if [ "$((AFTER_WALL - BEFORE_WALL))" -ge "$((SLEEP_SECS - 5))" ]; then
  echo "SLEPT THE FULL TIME -- no spurious wake. The RTC alarm brought it back."
else
  echo "WOKE EARLY after $((AFTER_WALL - BEFORE_WALL))s -- something else woke it."
fi

echo
echo "--- GPE counters that moved ---"
join /tmp/.gpe-before /tmp/.gpe-after 2>/dev/null \
  | awk '$2 != $3 {printf "%-10s %12s -> %-12s  (+%d)\n", $1, $2, $3, $3-$2}' \
  || echo "(could not diff)"

echo
echo "--- pm_wakeup_irq ---"
cat /sys/power/pm_wakeup_irq 2>/dev/null || echo "(empty -- not an ordinary IRQ)"

echo
echo "--- active wakeup sources ---"
mount -t debugfs none /sys/kernel/debug 2>/dev/null
if [ -r /sys/kernel/debug/wakeup_sources ]; then
  awk 'NR==1 || $3+0 > 0 {print}' /sys/kernel/debug/wakeup_sources | head -20
else
  echo "(wakeup_sources unavailable)"
fi

CHARGE_AFTER=$(cat $B/charge_now 2>/dev/null || echo 0)
VOLT_AFTER=$(cat $B/voltage_now 2>/dev/null || echo 0)
TEMP_AFTER=$(cat $B/temp 2>/dev/null || echo "")
echo
echo "--- drain ---"
if [ "$BAT_STATUS" = "Discharging" ] && [ "$CHARGE_BEFORE" -gt 0 ]; then
  awk -v cb="$CHARGE_BEFORE" -v ca="$CHARGE_AFTER" -v vb="$VOLT_BEFORE" -v va="$VOLT_AFTER" \
      -v secs="$((AFTER_WALL - BEFORE_WALL))" 'BEGIN {
    dmah = (cb - ca) / 1000.0;
    v    = (vb + va) / 2.0 / 1e6;
    wh   = dmah * v / 1000.0;
    hrs  = secs / 3600.0;
    printf "charge : %.0f -> %.0f mAh  (used %.1f mAh over %d s)\n", cb/1000.0, ca/1000.0, dmah, secs;
    printf "voltage: %.2f V average\n", v;
    if (hrs > 0) printf "drain  : %.2f W   (s2idle measures 4.1 W)\n", wh / hrs;
    if (secs < 900) print  "NOTE   : under 15 minutes the gauge resolution makes this rough.";
    full = 4040 * v / 1000.0;
    if (hrs > 0 && wh/hrs > 0) printf "implies: %.1f h on a full battery at this rate\n", full / (wh/hrs);
  }'
else
  echo "(skipped -- need the battery discharging and a readable charge_now)"
fi

[ -n "$TEMP_BEFORE" ] && awk -v a="$TEMP_BEFORE" -v b="$TEMP_AFTER" \
  'BEGIN{printf "battery: %.1f C -> %.1f C  (rising means real power is going somewhere)\n", a/10, b/10}'
echo "state  : $MEM_SLEEP"

echo
echo "--- kernel messages across the sleep ---"
journalctl -k --no-pager --since "@$BEFORE_WALL" 2>/dev/null \
  | grep -viE 'FWMSG|\[ISP\]' | tail -40 || echo "(none)"

echo
echo "--- battery ---"
grep -H . /sys/class/power_supply/BAT0/{status,capacity,current_now,voltage_now} 2>/dev/null | sed 's|.*/||'
