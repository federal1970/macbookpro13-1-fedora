#!/usr/bin/env bash
# Are pci=noaer and i915.enable_fbc=0 still needed? Boot WITHOUT them, run
# "boot" right away, use the machine normally for a while (watch the screen
# for flicker or torn areas), shut the lid for a minute once or twice, then
# run "after". Root.
log=/home/${SUDO_USER:-user}/dev/kernel-test/aer-fbc-test.log
exec > >(tee -a "$log") 2>&1
echo "### $(date '+%F %T') kernel $(uname -r) ${1:-?}"
echo "cmdline has: $(grep -oE 'pci=noaer|i915.enable_fbc=[0-9-]+' /proc/cmdline | tr '\n' ' ')(none = the test is valid)"
aer() { journalctl -k -b --no-pager | grep -ciE 'AER:|aer_|pcieport.*(error|correct)'; }
fbc() {
  echo "i915 enable_fbc parameter: $(cat /sys/module/i915/parameters/enable_fbc)"
  cat /sys/kernel/debug/dri/0/i915_fbc_status 2>/dev/null | head -3 || echo "(no debugfs fbc status)"
  echo "underrun/flip/fifo lines: $(journalctl -k -b --no-pager | grep -iE 'i915|drm' | grep -ciE 'underrun|flip_done|fifo|corrupt|timed out')"
  journalctl -k -b --no-pager | grep -iE 'i915|drm' | grep -iE 'underrun|flip_done|fifo|corrupt|timed out|error' | tail -5
}
case "${1:-}" in
boot)
  echo "### AER: ports that report, and lines so far"
  for d in /sys/bus/pci/devices/*; do
    [ -e "$d/aer_dev_correctable" ] && echo "$(basename $d) correctable: $(grep TOTAL_ERR_COR $d/aer_dev_correctable | awk '{print $2}') fatal: $(grep TOTAL_ERR_FATAL $d/aer_dev_fatal | awk '{print $2}')"
  done
  echo "AER journal lines this boot: $(aer)"
  echo "### FBC"; fbc
  date +%s > /run/aer-fbc-test.mark
  echo "### now use the machine, do a lid cycle or two, then: $0 after";;
after)
  since=$(cat /run/aer-fbc-test.mark 2>/dev/null || echo 0)
  echo "uptime: $(awk '{printf "%d min", $1/60}' /proc/uptime); sleeps this boot: $(journalctl -k -b --no-pager | grep -c 'suspend exit')"
  echo "### AER counters now (per device, correctable / fatal)"
  for d in /sys/bus/pci/devices/*; do
    [ -e "$d/aer_dev_correctable" ] && echo "$(basename $d) correctable: $(grep TOTAL_ERR_COR $d/aer_dev_correctable | awk '{print $2}') fatal: $(grep TOTAL_ERR_FATAL $d/aer_dev_fatal | awk '{print $2}')"
  done | grep -vE 'correctable: 0 fatal: 0' || echo "(all zero)"
  echo "AER journal lines this boot: $(aer)"
  journalctl -k -b --no-pager | grep -iE 'AER:|aer_' | tail -5
  echo "### FBC"; fbc
  echo "### verdict: zero AER lines = drop pci=noaer; no underruns and a clean screen = drop i915.enable_fbc=0";;
*) echo "usage: $0 boot|after"; exit 1;;
esac
