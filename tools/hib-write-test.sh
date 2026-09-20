#!/usr/bin/env bash
# Does the kernel actually write the hibernation image?
#
# The kernel log cannot answer this: the printk ring buffer is ordinary memory,
# so it goes into the snapshot, and restoring the image replaces it with the copy
# taken *before* the write. Every message the write produces is wiped by the
# restore. Disk write counters are not, so ask them instead.
set -u

REPORT=${REPORT:-/var/tmp/hib-write-report.txt}
DEV=${DEV:-nvme0n1}
TIMEOUT=${TIMEOUT:-600}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
exec > >(tee "$REPORT") 2>&1
chmod 644 "$REPORT" 2>/dev/null

sectors_written() { awk -v d="$DEV" '$3==d {print $10}' /proc/diskstats; }

echo "=== does hibernation write the image? $(date '+%F %T') ==="
echo

if [ -w /sys/power/pm_test ] && ! grep -q '\[none\]' /sys/power/pm_test; then
  echo "pm_test was $(sed 's/.*\[\(.*\)\].*/\1/' /sys/power/pm_test) -- forcing none"
  echo none > /sys/power/pm_test
fi
echo 1 > /sys/power/pm_debug_messages 2>/dev/null
echo test_resume > /sys/power/disk
grep -q '\[test_resume\]' /sys/power/disk || { echo "disk is not test_resume, refusing" >&2; exit 1; }

echo "pm_test    : $(cat /sys/power/pm_test)"
echo "disk       : $(cat /sys/power/disk)"
echo "image_size : $(cat /sys/power/image_size) bytes"
echo "device     : $DEV"
echo "swapfile   : $(swapon --show=NAME,SIZE,USED --noheadings | tr '\n' ' ')"

W0=$(sectors_written)
CURSOR=$(journalctl -k -n0 --show-cursor 2>/dev/null | sed -n 's/^-- cursor: //p')
T0=$(date +%s)
echo
echo "sectors written before: $W0"
echo
echo "--- hibernating (test_resume: write a real image, read it back, restore) ---"
sync
systemctl hibernate

for _ in $(seq 1 "$TIMEOUT"); do
  journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null | grep -q 'hibernation exit' && break
  sleep 1
done
T1=$(date +%s)
W1=$(sectors_written)

echo
echo "=== result ==="
printf 'cycle took        : %s s\n' "$((T1 - T0))"
printf 'sectors written   : %s -> %s\n' "$W0" "$W1"
awk -v a="$W0" -v b="$W1" -v s="$((T1 - T0))" 'BEGIN {
  mb = (b - a) * 512 / 1048576;
  printf "written during it : %.1f MB\n", mb;
  if (s > 0) printf "average rate      : %.1f MB/s over the whole cycle\n", mb / s;
  print "";
  if (mb > 200)
    printf "VERDICT: the image IS written. %.1f MB went to disk -- the write path works.\n", mb;
  else
    printf "VERDICT: no image was written. Only %.1f MB moved, far less than an image.\n", mb;
}'

echo
echo "--- did it come back through a real restore? ---"
LOG=$(journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null | grep -viE 'FWMSG|\[ISP\]|Marking nosave|Calling |smpboot|CPU[0-9]')
grep -q 'Hibernation image restored successfully' <<<"$LOG" \
  && echo "  yes -- 'Hibernation image restored successfully' (in_suspend was cleared by" \
  && echo "  restore_registers, which is only reached by unpacking an image from disk)" \
  || echo "  NO -- the restore path was not taken"
grep -qE 'Error -?[0-9]+ creating image' <<<"$LOG" \
  && { echo "  but create_image reported an error:"; grep -E 'Error -?[0-9]+ creating image' <<<"$LOG" | sed 's/^/    /'; } \
  || echo "  and create_image reported no error"

echo
echo "--- kernel log of the cycle ---"
echo "$LOG" | tail -35
