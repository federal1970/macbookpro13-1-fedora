#!/usr/bin/env bash
# The whole point of all of this: close the lid, walk away, come back to a
# machine that spent the time at zero draw with the session intact.
#
# Records everything that can answer that afterwards, from sources outside
# kernel memory, and syncs them to disk so the answer survives a cycle that
# never comes back. See tools/hib-real-test.sh for why the journal cannot.
set -u

STATE=${STATE:-/var/tmp/lid-test-baseline}
NVME=${NVME:-/dev/nvme0}
BAT=${BAT:-/sys/class/power_supply/BAT0}
AC=${AC:-/sys/class/power_supply/ADP1}
MODE=${1:-baseline}

[ "$(id -u)" -eq 0 ] || { echo "run as root (the NVMe counters need it)" >&2; exit 1; }

read_all() {
  nvme smart-log "$NVME" 2>/dev/null | awk -F: '
    /power_cycles/     { gsub(/[^0-9]/,"",$2); pc=$2 }
    /unsafe_shutdowns/ { gsub(/[^0-9]/,"",$2); us=$2 }
    END { printf "PC=%s\nUS=%s\n", pc, us }'
  echo "CHARGE=$(cat "$BAT/charge_now")"
  echo "VOLT=$(cat "$BAT/voltage_now")"
  echo "ONAC=$(cat "$AC/online")"
  echo "TS=$(date +%s)"
}

if [ "$MODE" = "--compare" ]; then
  [ -r "$STATE" ] || { echo "no baseline at $STATE" >&2; exit 1; }
  # shellcheck disable=SC1090
  . "$STATE"
  eval "$(read_all | sed 's/^/NOW_/')"
  DT=$((NOW_TS - TS))
  echo "=== lid test, $((DT / 60)) min $((DT % 60)) s since the baseline of $WHEN ==="
  echo
  printf 'power cycles     : %s -> %s\n' "$PC" "$NOW_PC"
  printf 'unsafe shutdowns : %s -> %s\n' "$US" "$NOW_US"
  echo
  if [ "$NOW_PC" -gt "$PC" ]; then
    echo "  the SSD lost power -- the machine really did hibernate"
  else
    echo "  the SSD never lost power -- it suspended but never hibernated"
  fi
  echo
  if [ "$ONAC" = "1" ] || [ "$NOW_ONAC" = "1" ]; then
    echo "battery          : charger was connected, drain figure is meaningless"
  else
    awk -v c0="$CHARGE" -v c1="$NOW_CHARGE" -v v="$VOLT" -v dt="$DT" 'BEGIN{
      wh = (c0-c1)/1e6 * v/1e6
      printf "battery          : %.0f -> %.0f uAh  (%.2f Wh over %.2f h)\n", c0, c1, wh, dt/3600
      if (dt > 0) printf "average draw     : %.2f W\n", wh/(dt/3600)
      print  ""
      print  "For scale: s2idle on this machine is about 3.8 W, awake and idle 8.2 W."
      print  "A real hibernation should land near zero -- only the seconds spent"
      print  "awake at each end and the firmware boot cost anything."
    }'
  fi
  exit 0
fi

eval "$(read_all)"
[ -n "${PC:-}" ] || { echo "could not read SMART from $NVME" >&2; exit 1; }
cat > "$STATE" <<EOS
PC=$PC
US=$US
CHARGE=$CHARGE
VOLT=$VOLT
ONAC=$ONAC
TS=$TS
WHEN='$(date '+%F %T')'
EOS
sync

DELAY=$(systemd-analyze cat-config systemd/sleep.conf 2>/dev/null | awk -F= '/^HibernateDelaySec=/ {d=$2} END {print d}')
SLEEPMODE=$(awk '/^\[Battery\]\[SuspendAndShutdown\]/{f=1;next} /^\[/{f=0} f && /^SleepMode=/{print $0}' \
              /home/*/.config/powerdevilrc 2>/dev/null | head -1)

echo "=== lid test baseline, $(date '+%F %T') ==="
echo
echo "power cycles     : $PC"
echo "unsafe shutdowns : $US"
echo "battery          : $CHARGE uAh at $(awk -v v="$VOLT" 'BEGIN{printf "%.2f", v/1e6}') V"
echo "on AC            : $ONAC"
echo "HibernateDelaySec: ${DELAY:-unknown}"
echo "KDE battery lid  : ${SLEEPMODE:-not set}"
echo
if [ "$ONAC" = "1" ]; then
  echo "!! THE CHARGER IS PLUGGED IN."
  echo "   SleepMode=3 (suspend-then-hibernate) is set only for the battery"
  echo "   profile -- there is no [AC][SuspendAndShutdown] group -- so on mains"
  echo "   the lid does a plain suspend and will never hibernate."
  echo "   Unplug the charger before closing the lid, or this test proves nothing."
  echo
fi
echo "Now: close the lid, wait well past ${DELAY:-the delay}, and check that the"
echo "machine is really off -- the Force Touch trackpad does not click without"
echo "power. Then open it, press the power button, enter the disk passphrase."
echo
echo "Afterwards:  sudo tools/lid-hibernate-test.sh --compare"
