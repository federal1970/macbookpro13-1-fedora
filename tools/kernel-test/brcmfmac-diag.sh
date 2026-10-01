#!/usr/bin/env bash
# Diagnose why brcmfmac (BCM4350) does not come back after deep (S3) on
# MacBookPro13,1 without the unload/reload sleep hook. Root.
#
#   brcmfmac-diag.sh prep      disable the hook, turn on PCIe debug output
#   (shut the lid for about a minute, open it)
#   brcmfmac-diag.sh collect   dump what the driver did, restore the hook,
#                              bring Wi-Fi back by reloading the module
HOOK=/usr/lib/systemd/system-sleep/brcmfmac-reload
# systemd-sleep runs EVERY executable in that directory whatever its name, so
# the hook has to leave the directory, not just be renamed
PARK=/var/tmp/brcmfmac-reload.parked
LOG=/home/${SUDO_USER:-user}/dev/kernel-test/brcmfmac-diag.log
case "${1:-}" in
prep)
  [ -x "$HOOK" ] && mv "$HOOK" "$PARK" && echo "hook moved out of the way"
  ls /usr/lib/systemd/system-sleep/
  # BRCMF_TRACE_VAL|BRCMF_INFO_VAL|BRCMF_PCIE_VAL
  echo $((0x2 | 0x4 | 0x80000)) > /sys/module/brcmfmac/parameters/debug
  echo "brcmfmac debug=$(cat /sys/module/brcmfmac/parameters/debug)"
  date +%s > /run/brcmfmac-diag.mark
  echo "now shut the lid for a minute, open it, then run: $0 collect"
  ;;
collect)
  exec > >(tee "$LOG") 2>&1
  since=$(cat /run/brcmfmac-diag.mark 2>/dev/null || echo 0)
  echo "### kernel $(uname -r), $(date)"
  echo "### suspend/resume and brcmfmac messages since prep"
  journalctl -k -o short-iso --no-pager --since "@$since" | grep -iE 'suspend|resume|sleep state|brcmf|wlp2s0|02:00.0|pcieport 0000:00:1d.0' | cut -c12-
  echo "### device state now"
  lspci -s 02:00.0 -vv 2>/dev/null | grep -E 'Status|LnkSta|Unknown header|Kernel driver' | head -4
  setpci -s 02:00.0 VENDOR_ID 2>&1
  cat /sys/bus/pci/devices/0000:02:00.0/power/runtime_status 2>&1
  echo "### interface / NetworkManager"
  ip -br link show wlp2s0 2>&1; nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep wlp || true
  echo "### a scan attempt (10 s max)"
  timeout 10 iw dev wlp2s0 scan 2>&1 | grep -cE '^BSS' || echo "scan failed"
  echo "### restore: hook back, debug off, module reloaded"
  echo 0 > /sys/module/brcmfmac/parameters/debug
  [ -f "$PARK" ] && mv "$PARK" "$HOOK" && echo "hook restored"
  ls /usr/lib/systemd/system-sleep/
  systemctl stop NetworkManager; modprobe -r brcmfmac_wcc brcmfmac 2>&1; sleep 1; modprobe brcmfmac; systemctl start NetworkManager
  sleep 8; nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep wlp || true
  echo "### log: $LOG"
  ;;
*) echo "usage: $0 prep|collect"; exit 1;;
esac
