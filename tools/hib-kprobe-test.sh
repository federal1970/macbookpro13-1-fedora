#!/usr/bin/env bash
# Where does swsusp_save() stop?
#
# The ftrace ring buffer lives in ordinary memory. Since this machine never
# writes an image and never restores one, that memory is not replaced -- so the
# trace survives the cycle and can say what the kernel log cannot.
set -u

REPORT=${REPORT:-/var/tmp/hib-kprobe-report.txt}
T=/sys/kernel/tracing
DEV=${DEV:-nvme0n1}
TIMEOUT=${TIMEOUT:-600}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
[ -d "$T" ] || { echo "no tracefs at $T" >&2; exit 1; }
exec > >(tee "$REPORT") 2>&1
chmod 644 "$REPORT" 2>/dev/null

sectors_written() { awk -v d="$DEV" '$3==d {print $10}' /proc/diskstats; }

cleanup() {
  echo
  echo "--- removing probes ---"
  echo 0 > $T/tracing_on 2>/dev/null
  echo 0 > $T/events/kprobes/enable 2>/dev/null
  echo 0 > $T/events/power/suspend_resume/enable 2>/dev/null
  echo > $T/kprobe_events 2>/dev/null && echo "kprobes removed" || echo "WARNING: could not clear kprobe_events"
  echo "report: $REPORT"
}
trap cleanup EXIT INT TERM

echo "=== where does swsusp_save() stop? $(date '+%F %T') ==="
echo

if [ -w /sys/power/pm_test ] && ! grep -q '\[none\]' /sys/power/pm_test; then
  echo none > /sys/power/pm_test
fi
echo 1 > /sys/power/pm_debug_messages 2>/dev/null
echo test_resume > /sys/power/disk
grep -q '\[test_resume\]' /sys/power/disk || { echo "disk is not test_resume, refusing" >&2; exit 1; }
echo "pm_test: $(cat /sys/power/pm_test)"
echo "disk   : $(cat /sys/power/disk)"

# --- probes ---------------------------------------------------------------
echo 0 > $T/tracing_on
echo > $T/kprobe_events 2>/dev/null
echo > $T/trace

echo
echo "--- installing probes ---"
add() {  # add <event> <spec>
  if echo "$2" >> $T/kprobe_events 2>/dev/null; then
    echo "  ok   $1"
  else
    echo "  FAIL $1   ($2)"
  fi
}
add "hibernation_snapshot entry"  'p:hib_snap_in hibernation_snapshot'
add "hibernation_snapshot return" 'r:hib_snap_out hibernation_snapshot ret=$retval'
add "create_image entry"          'p:hib_create_in create_image'
add "create_image return"         'r:hib_create_out create_image ret=$retval'
add "swsusp_arch_suspend entry"   'p:hib_arch_in swsusp_arch_suspend'
add "swsusp_arch_suspend return"  'r:hib_arch_out swsusp_arch_suspend ret=$retval'
add "swsusp_save entry"           'p:hib_save_in swsusp_save'
add "swsusp_save return"          'r:hib_save_out swsusp_save ret=$retval'
add "copy_data_pages entry"       'p:hib_copy_in copy_data_pages.constprop.0'
add "copy_data_pages return"      'r:hib_copy_out copy_data_pages.constprop.0 ret=$retval'
add "swsusp_write entry"          'p:hib_write_in swsusp_write'
add "swsusp_write return"         'r:hib_write_out swsusp_write ret=$retval'

echo 1 > $T/events/kprobes/enable
echo 1 > $T/events/power/suspend_resume/enable 2>/dev/null
echo 1 > $T/tracing_on
echo
echo "probes armed: $(grep -c . $T/kprobe_events)"

W0=$(sectors_written)
CURSOR=$(journalctl -k -n0 --show-cursor 2>/dev/null | sed -n 's/^-- cursor: //p')
T0=$(date +%s)

echo
echo "--- hibernating ---"
sync
systemctl hibernate
for _ in $(seq 1 "$TIMEOUT"); do
  journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null | grep -q 'hibernation exit' && break
  sleep 1
done
T1=$(date +%s)
W1=$(sectors_written)
echo 0 > $T/tracing_on

echo
echo "=== result ==="
printf 'cycle took      : %s s\n' "$((T1 - T0))"
awk -v a="$W0" -v b="$W1" 'BEGIN{printf "written to disk : %.1f MB\n", (b-a)*512/1048576}'

echo
echo "--- trace ---"
grep -vE '^#' $T/trace | sed 's/^[[:space:]]*//' || echo "(trace is empty)"

echo
echo "--- reading it ---"
TR=$(grep -vE '^#' $T/trace)
has() { grep -q "$1" <<<"$TR"; }
has hib_save_in   && echo "  swsusp_save was entered"            || echo "  swsusp_save was NEVER entered"
has hib_copy_in   && echo "  copy_data_pages was entered"        || echo "  copy_data_pages was NEVER entered"
has hib_copy_out  && echo "  copy_data_pages RETURNED: $(grep -o 'hib_copy_out.*' <<<"$TR" | head -1)" \
                  || echo "  copy_data_pages never returned  <-- it stops inside the page copy"
has hib_save_out  && echo "  swsusp_save RETURNED: $(grep -o 'hib_save_out.*' <<<"$TR" | head -1)" \
                  || echo "  swsusp_save never returned"
has hib_arch_out  && echo "  swsusp_arch_suspend RETURNED: $(grep -o 'hib_arch_out.*' <<<"$TR" | head -1)" \
                  || echo "  swsusp_arch_suspend never returned"
has hib_create_out && echo "  create_image RETURNED: $(grep -o 'hib_create_out.*' <<<"$TR" | head -1)" \
                  || echo "  create_image never returned"
has hib_write_in  && echo "  swsusp_write WAS called" \
                  || echo "  swsusp_write was never called  <-- nothing was ever asked to be written"
