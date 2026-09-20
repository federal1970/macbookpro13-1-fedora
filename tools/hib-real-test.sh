#!/usr/bin/env bash
# A real hibernation -- and proof of whether the machine actually lost power.
#
# The journal cannot tell a real power-off cycle from an in-memory test_resume:
# the restored image prints the same lines either way, and everything the write
# and the reload printed is rolled back with the ring buffer. Two things outside
# kernel memory can tell them apart:
#
#   power_cycles     on the NVMe controller -- +1 means the SSD lost power
#   unsafe_shutdowns on the NVMe controller -- +1 means it lost power abruptly
#
# Both live on the SSD, so neither a restore nor a hard reset can roll them back.
# A third, free check: the root is LUKS and there is no keyfile or TPM token, so
# a genuine resume from disk cannot happen without the initramfs asking for the
# disk password.
set -u

STATE=${STATE:-/var/tmp/hib-real-baseline}
REPORT=${REPORT:-/var/tmp/hib-real-report.txt}
NVME=${NVME:-/dev/nvme0}
UNIT_BYTES=${UNIT_BYTES:-512}
MODE=${1:-shutdown}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }

smart_field() {
  nvme smart-log "$NVME" 2>/dev/null | awk -F: -v f="$1" '$0 ~ f {gsub(/[^0-9]/,"",$2); print $2; exit}'
}
snapshot_counters() {
  echo "PC=$(smart_field power_cycles)"
  echo "US=$(smart_field unsafe_shutdowns)"
  echo "DU=$(smart_field data_units_written)"
}
say() { echo "$@" | tee -a "$REPORT"; sync; }

if [ "$MODE" = "--compare" ]; then
  [ -r "$STATE" ] || { echo "no baseline at $STATE" >&2; exit 1; }
  # shellcheck disable=SC1090
  . "$STATE"
  eval "$(snapshot_counters | sed 's/^/NOW_/')"
  echo "=== against the baseline of $WHEN (mode was $BASE_MODE) ==="
  printf 'power cycles     : %s -> %s\n' "$PC" "$NOW_PC"
  printf 'unsafe shutdowns : %s -> %s\n' "$US" "$NOW_US"
  printf 'data units       : %s -> %s  (%s MB)\n' "$DU" "$NOW_DU" \
    "$(awk -v d="$((NOW_DU - DU))" -v u="$UNIT_BYTES" 'BEGIN{printf "%.1f", d*u/1048576}')"
  echo
  if [ "$NOW_PC" -gt "$PC" ]; then
    echo "VERDICT: the SSD lost power. This was a real hibernation, not a test_resume."
  else
    echo "VERDICT: the SSD never lost power. The machine stayed up the whole time --"
    echo "         whatever it did, it was not a hibernation that powers off."
  fi
  exit 0
fi

case "$MODE" in
  shutdown|platform|reboot) ;;
  *) echo "usage: $0 [shutdown|platform|reboot|--compare]" >&2; exit 1 ;;
esac

: > "$REPORT"; chmod 644 "$REPORT" 2>/dev/null
say "=== real hibernation, mode '$MODE', $(date '+%F %T') ==="
say

grep -q '\[none\]' /sys/power/pm_test || echo none > /sys/power/pm_test
grep -q '\[none\]' /sys/power/pm_test || { echo "pm_test is not none, refusing" >&2; exit 1; }
echo "$MODE" > /sys/power/disk
grep -q "\[$MODE\]" /sys/power/disk || { echo "could not select '$MODE', refusing" >&2; exit 1; }

say "pm_test : $(cat /sys/power/pm_test)"
say "disk    : $(cat /sys/power/disk)"
say "resume  : $(cat /sys/power/resume) offset $(cat /sys/power/resume_offset)"
say

eval "$(snapshot_counters)"
[ -n "${PC:-}" ] || { echo "could not read SMART from $NVME" >&2; exit 1; }
cat > "$STATE" <<EOS
PC=$PC
US=$US
DU=$DU
WHEN='$(date '+%F %T')'
BASE_MODE=$MODE
EOS
sync

say "power cycles before     : $PC"
say "unsafe shutdowns before : $US"
say "data units before       : $DU"
say
say "Baseline is on disk and synced. However this ends, afterwards run:"
say "  sudo tools/hib-real-test.sh --compare"
say
say "Expect the machine to power off. Press the power button to bring it back;"
say "a genuine resume WILL ask for the disk password before restoring."
say
say "--- hibernating ---"
sync
systemctl hibernate
