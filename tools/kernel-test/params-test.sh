#!/usr/bin/env bash
# Backlog items 3 and 4 (plus the two cleanup candidates): run after booting
# the test kernel WITHOUT button.lid_init_state=open, nvme.noacpi=1,
# nvme_core.default_ps_max_latency_us=0, pci=noaer and i915.enable_fbc=0,
# in multi-user.target so no desktop can react to a wrong lid state. Root.
log=/home/${SUDO_USER:-user}/dev/kernel-test/params-test.log
exec > >(tee -a "$log") 2>&1
echo "### $(date '+%F %T') kernel $(uname -r) mode=${1:-boot}"
if [ "${1:-}" = after ]; then
  echo "### after the lid cycle: did it sleep in S3 and come back clean?"
  journalctl -k -b --no-pager -o short-iso | grep -E 'suspend entry|Waking up from system sleep state|suspend exit' | tail -3 | cut -c12-100
  echo "nvme error lines after resume: $(journalctl -k -b --no-pager | grep -iE 'nvme' | grep -ciE 'error|timeout|abort|reset|fail')"
  journalctl -k -b -p err --no-pager --since '-5min' | grep -vE 'ACPI (BIOS )?Error|DMAR|Bluetooth|bpf-restrict' | tail -5
  cat /proc/acpi/button/lid/LID0/state
  echo "### done; reboot into the stock kernel and remove the logind drop-in"
  exit 0
fi
echo "cmdline: $(cat /proc/cmdline)"
echo "### lid: what the kernel reports at boot without the init override"
cat /proc/acpi/button/lid/LID0/state
grep -oE 'lid_init_state=[a-z]+' /proc/cmdline || echo "(no lid_init_state parameter: kernel default 'method')"
journalctl -b --no-pager -o short-iso | grep -E 'Lid (closed|opened)|lid' | grep -vE 'audit|Watching' | cut -c12-120 | head -5
echo "(a 'Lid closed' line here with the lid open is the bug the quirk would fix)"
echo "### nvme without noacpi / APST limit"
cat /sys/class/nvme/nvme0/power/control 2>/dev/null; cat /sys/module/nvme_core/parameters/default_ps_max_latency_us
nvme_ps=$(cat /sys/class/nvme/nvme0/device/power_state 2>/dev/null); echo "nvme pci power_state: ${nvme_ps:-?}"
journalctl -k -b --no-pager | grep -iE 'nvme' | grep -iE 'error|timeout|abort|reset|fail|I/O' | head -5 || true
echo "nvme error lines: $(journalctl -k -b --no-pager | grep -iE 'nvme' | grep -ciE 'error|timeout|abort|reset|fail')"
echo "### AER without pci=noaer"
journalctl -k -b --no-pager | grep -iE 'AER|pcieport.*(error|correct)' | head -5 || true
echo "AER lines: $(journalctl -k -b --no-pager | grep -ciE 'AER:|aer_')"
echo "### i915 without enable_fbc=0"
cat /sys/module/i915/parameters/enable_fbc 2>/dev/null; journalctl -k -b --no-pager | grep -iE 'i915|drm' | grep -iE 'error|fail|warn|flip|underrun' | head -5 || true
echo "### kernel errors this boot (filtered)"
journalctl -k -b -p err --no-pager | grep -vE 'ACPI (BIOS )?Error|DMAR|Bluetooth|SGX|TDX|bpf-restrict' | tail -8
echo "### now a lid cycle: shut the lid, wait 60 s, open it, then run: $0 after"
