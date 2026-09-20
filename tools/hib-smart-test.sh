#!/usr/bin/env bash
# Is the hibernation image actually written to disk?
#
# /proc/diskstats CANNOT answer this. Those counters are ordinary kernel memory,
# so they go into the snapshot and a successful restore rolls them back to their
# pre-write values -- the same effect that wipes the printk ring buffer. The
# earlier "11.9 MB, so nothing is ever written" measurement was that artefact.
#
# The NVMe controller's own Data Units Written lives on the SSD. Nothing the
# kernel does to its own memory can roll it back, and it survives a power cycle,
# so this measurement is still valid even if the machine hangs and needs a hard
# reset: reboot and run this script with --compare.
set -u

REPORT=${REPORT:-/var/tmp/hib-smart-report.txt}
STATE=${STATE:-/var/tmp/hib-smart-baseline}
NVME=${NVME:-/dev/nvme0}
DEV=${DEV:-nvme0n1}
TIMEOUT=${TIMEOUT:-600}
MODE=${1:-run}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }

# Data Units Written: one unit is 1000 * 512 bytes.
units_written() {
  local v
  v=$(nvme smart-log "$NVME" 2>/dev/null | awk -F: '/data_units_written/ {gsub(/[^0-9]/,"",$2); print $2; exit}')
  [ -z "$v" ] && v=$(smartctl -A "$NVME" 2>/dev/null | awk -F: '/Data Units Written/ {gsub(/[^0-9]/,"",$2); print $2; exit}')
  echo "${v:-}"
}
units_to_mb() { awk -v u="$1" 'BEGIN{printf "%.1f", u*512*1000/1048576}'; }
sectors_written() { awk -v d="$DEV" '$3==d {print $10}' /proc/diskstats; }

say() { echo "$@" | tee -a "$REPORT"; sync; }

# --- compare mode: run this after a hard reset ----------------------------
if [ "$MODE" = "--compare" ]; then
  [ -r "$STATE" ] || { echo "no baseline at $STATE" >&2; exit 1; }
  # shellcheck disable=SC1090
  . "$STATE"
  U1=$(units_written)
  [ -n "$U1" ] || { echo "could not read SMART" >&2; exit 1; }
  echo "=== comparing against the baseline of $BASE_WHEN ==="
  printf 'data units written : %s -> %s\n' "$BASE_UNITS" "$U1"
  D=$(awk -v a="$BASE_UNITS" -v b="$U1" 'BEGIN{print b-a}')
  printf 'written since      : %s MB\n' "$(units_to_mb "$D")"
  echo
  echo "An image is expected to be on the order of 1000-3000 MB (it is compressed)."
  awk -v mb="$(units_to_mb "$D")" 'BEGIN{
    if (mb+0 > 500) print "VERDICT: a hibernation image WAS written to the SSD.";
    else print "VERDICT: no image-sized write reached the SSD.";
  }'
  exit 0
fi

# --- run mode -------------------------------------------------------------
: > "$REPORT"; chmod 644 "$REPORT" 2>/dev/null
say "=== does the image reach the SSD? $(date '+%F %T') ==="
say

if [ -w /sys/power/pm_test ] && ! grep -q '\[none\]' /sys/power/pm_test; then
  echo none > /sys/power/pm_test
fi
echo 1 > /sys/power/pm_debug_messages 2>/dev/null
echo test_resume > /sys/power/disk
grep -q '\[test_resume\]' /sys/power/disk || { echo "disk is not test_resume, refusing" >&2; exit 1; }

say "pm_test     : $(cat /sys/power/pm_test)"
say "disk        : $(cat /sys/power/disk)"
say "resume dev  : $(cat /sys/power/resume) offset $(cat /sys/power/resume_offset)"
say "image_size  : $(cat /sys/power/image_size) bytes"
say "swap        : $(swapon --show=NAME,SIZE,USED,PRIO --noheadings | tr '\n' ';')"
say

U0=$(units_written)
[ -n "$U0" ] || { echo "could not read SMART from $NVME" >&2; exit 1; }
W0=$(sectors_written)
T0=$(date +%s)

# Written to disk and synced, so it survives a hang plus a hard reset.
cat > "$STATE" <<EOS
BASE_UNITS=$U0
BASE_WHEN='$(date '+%F %T')'
EOS
sync

say "SMART data units written before : $U0  ($(units_to_mb "$U0") MB lifetime)"
say "diskstats sectors before        : $W0"
say
say "If the machine hangs and you have to hard reset, that baseline is on disk."
say "After the reboot run:  sudo tools/hib-smart-test.sh --compare"
say
say "--- hibernating (test_resume) ---"
sync

CURSOR=$(journalctl -k -n0 --show-cursor 2>/dev/null | sed -n 's/^-- cursor: //p')
systemctl hibernate
for _ in $(seq 1 "$TIMEOUT"); do
  journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null | grep -q 'hibernation exit' && break
  sleep 1
done
T1=$(date +%s)
U1=$(units_written)
W1=$(sectors_written)

say
say "=== result ==="
say "cycle took : $((T1 - T0)) s"
say
D=$(awk -v a="$U0" -v b="$U1" 'BEGIN{print b-a}')
say "SMART (on the SSD, cannot be rolled back):"
say "  data units written : $U0 -> $U1"
say "  written during it  : $(units_to_mb "$D") MB"
say
say "diskstats (kernel memory, rolled back by a restore -- shown only to contrast):"
say "  sectors written    : $W0 -> $W1"
say "  apparent           : $(awk -v a="$W0" -v b="$W1" 'BEGIN{printf "%.1f", (b-a)*512/1048576}') MB"
say
awk -v mb="$(units_to_mb "$D")" 'BEGIN{
  if (mb+0 > 500) print "VERDICT: the image IS written. The write path works; the remaining";
  else           print "VERDICT: no image-sized write reached the SSD.";
}' | tee -a "$REPORT"
awk -v mb="$(units_to_mb "$D")" 'BEGIN{ if (mb+0 > 500) print "         symptom is only the multi-minute stall inside the snapshot."; }' | tee -a "$REPORT"
sync

say
say "--- what the kernel logged ---"
journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null \
  | grep -iE 'Creating image|Need to copy|Image created|Image saving|Image loading|restored successfully|Timekeeping suspended|HibernateLocation|hibernation exit|Error' \
  | sed 's/^/  /' | tee -a "$REPORT"
sync
echo
echo "report: $REPORT"
