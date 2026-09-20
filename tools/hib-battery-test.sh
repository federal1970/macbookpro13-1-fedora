#!/usr/bin/env bash
# What does an hour of hibernation actually cost?
#
# Reads the battery coulomb counter, hibernates, and reads it again the moment
# the machine comes back -- `systemctl hibernate` only returns after the resume,
# and this script goes into the image along with its variables, so the second
# reading needs no second invocation and is taken seconds after the restore.
#
# No root needed: the battery counters are world-readable.
set -u

BAT=${BAT:-/sys/class/power_supply/BAT0}
AC=${AC:-/sys/class/power_supply/ADP1}
REPORT=${REPORT:-/var/tmp/hib-battery-report.txt}
STATE=${STATE:-/var/tmp/hib-battery-baseline}

# Measured on this machine, 2026-09-20: awake idle 8.19 W, s2idle 3.83 W, and one
# hibernate+resume transition (write, power-off, firmware boot, read-back).
TRANSITION_WH=${TRANSITION_WH:-0.32}

say() { echo "$@" | tee -a "$REPORT"; sync; }

[ "$(cat "$AC/online")" = "0" ] || {
  echo "The charger is plugged in. Unplug it -- otherwise this measures nothing." >&2
  exit 1
}

: > "$REPORT"
C0=$(cat "$BAT/charge_now"); V0=$(cat "$BAT/voltage_now"); T0=$(date +%s)
CF=$(cat "$BAT/charge_full")

cat > "$STATE" <<EOS
C0=$C0
V0=$V0
T0=$T0
WHEN='$(date '+%F %T')'
EOS
sync

say "=== what does hibernation cost? $(date '+%F %T') ==="
say
say "charge now : $C0 uAh of $CF  ($(awk -v a="$C0" -v b="$CF" 'BEGIN{printf "%.0f", a*100/b}')%)"
say "voltage    : $(awk -v v="$V0" 'BEGIN{printf "%.2f", v/1e6}') V"
say "on AC      : no"
say
say "Hibernating now. Leave it off as long as you like -- an hour is plenty."
say "Bring it back with the power button, enter the disk passphrase, and the"
say "second reading is taken automatically within seconds of the restore."
say
sync

systemctl hibernate

C1=$(cat "$BAT/charge_now"); V1=$(cat "$BAT/voltage_now"); T1=$(date +%s)

say "--- back at $(date '+%F %T') ---"
say
awk -v c0="$C0" -v c1="$C1" -v v="$V0" -v t0="$T0" -v t1="$T1" -v tr="$TRANSITION_WH" '
BEGIN{
  dt = t1 - t0
  wh = (c0-c1)/1e6 * v/1e6
  printf "off for        : %d min %d s\n", dt/60, dt%60
  printf "charge         : %d -> %d uAh\n", c0, c1
  printf "energy used    : %.3f Wh total\n", wh
  printf "raw average    : %.2f W across the whole window\n\n", wh/(dt/3600)
  rest = wh - tr
  if (rest < 0) rest = 0
  printf "One hibernate+resume transition was measured at %.2f Wh. Taking that off\n", tr
  printf "leaves %.3f Wh for the %.2f h the machine actually spent powered down:\n\n", rest, dt/3600
  printf "  drain while hibernated : %.3f W\n\n", rest/(dt/3600)
  print  "For scale, measured on this machine: s2idle 3.83 W (about 12 h from full),"
  print  "awake and idle 8.19 W. Anything under ~0.2 W here means the machine really"
  print  "was off and the battery only lost what the write and the boot cost."
}' | tee -a "$REPORT"
sync
echo
echo "report: $REPORT"
