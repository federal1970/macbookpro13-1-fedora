#!/usr/bin/env bash
# One boot in ~7 the BCM4350 answers 0xffffffff to brcmfmac's first read and
# the probe fails ("brcmf_chip_recognition: MMIO read failed"). Run this as
# root in such a boot BEFORE rebooting. It records what the chip looks like
# and tries, in order of force, to bring it back without a reboot:
#   1. plain module reload           2. PCI function reset + reload
#   3. remove the device, rescan the root port, let the driver re-probe
# Stops at the first step that gives a working interface.
D=0000:02:00.0; RP=0000:00:1d.1
log=/home/${SUDO_USER:-user}/dev/kernel-test/wifi-dead-at-boot.log
exec > >(tee -a "$log") 2>&1
echo "### $(date '+%F %T') kernel $(uname -r) boot $(cat /proc/sys/kernel/random/boot_id)"
state() {
  echo "-- $1"
  echo "config space vendor:device: $(setpci -s $D vendor_id 2>&1):$(setpci -s $D device_id 2>&1)  power state: $(setpci -s $D CAP_PM+4.w 2>&1)"
  echo "link: root port $(cat /sys/bus/pci/devices/$RP/current_link_speed 2>&1) / device $(cat /sys/bus/pci/devices/$D/current_link_speed 2>&1); driver: $(basename "$(readlink /sys/bus/pci/devices/$D/driver 2>/dev/null)" 2>/dev/null || echo none)"
  echo "interface: $(ip -br link show 2>/dev/null | grep -oE '^wl[a-z0-9]+' | head -1 || echo none)"
}
ok() { ip -br link show 2>/dev/null | grep -qE '^wl'; }
mark() { journalctl -k -o cat --no-pager | wc -l; }
dm() { journalctl -k -o short-iso --no-pager | tail -n +$(($1+1)) | grep -iE 'brcmf|02:00.0|pcieport' | cut -c12-150 | tail -8; }
echo "### probe failure this boot:"; journalctl -k -b --no-pager | grep -iE 'brcmf' | head -4 | cut -c32-
state "as found"
if ok; then echo "interface exists; nothing to do"; exit 0; fi
echo "### step 1: module reload"; m=$(mark)
modprobe -r brcmfmac_wcc brcmfmac 2>&1; sleep 1; modprobe brcmfmac; sleep 4; dm $m; state "after reload"
if ok; then echo "### RESULT: a plain reload is enough"; exit 0; fi
echo "### step 2: PCI function reset, then reload"; m=$(mark)
modprobe -r brcmfmac_wcc brcmfmac 2>&1
echo 1 > /sys/bus/pci/devices/$D/reset 2>&1 && echo "reset written" || echo "reset not supported/failed"
sleep 1; modprobe brcmfmac; sleep 4; dm $m; state "after function reset"
if ok; then echo "### RESULT: a function reset revives it"; exit 0; fi
echo "### step 3: remove the device and rescan the root port"; m=$(mark)
modprobe -r brcmfmac_wcc brcmfmac 2>&1
echo 1 > /sys/bus/pci/devices/$D/remove; sleep 2
echo 1 > /sys/bus/pci/devices/$RP/rescan; sleep 3
modprobe brcmfmac; sleep 5; dm $m; state "after remove+rescan"
if ok; then echo "### RESULT: remove+rescan revives it"; else echo "### RESULT: nothing short of a reboot; the chip needs a power cycle"; fi
echo "### log: $log"
