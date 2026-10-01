#!/usr/bin/env bash
# Send the kernel log over the USB Ethernet adapter to a listener on the LAN.
#   netconsole-up.sh <receiver IP> [port]      (root)
# The receiver runs:  nc -ulk 6666 | tee netconsole.log
# Survives hibernation inside the image: whatever the restored kernel prints
# after the adapter is back goes out again. Also turns on PM debug messages
# and brcmfmac PCIe/INFO traces so the restore path is visible.
rx=${1:?usage: $0 <receiver IP> [port]}; port=${2:-6666}
dev=$(ls /sys/class/net | grep -E '^enp.*u' | head -1); [ -n "$dev" ] || { echo "no USB ethernet interface"; exit 1; }
src=$(ip -4 -o addr show dev "$dev" | awk '{print $4}' | cut -d/ -f1); [ -n "$src" ] || { echo "$dev has no IPv4 address"; exit 1; }
# the receiver's MAC: without it netconsole sends to ff:ff:ff:ff:ff:ff, and a
# receiver on Wi-Fi may never see broadcast frames from the wired side
ping -c1 -W1 "$rx" >/dev/null 2>&1
mac=$(ip neigh show "$rx" dev "$dev" | awk '{for(i=1;i<=NF;i++) if($i=="lladdr") print $(i+1)}' | head -1)
[ -n "$mac" ] || { echo "no MAC for $rx in the neighbour table"; exit 1; }
modprobe -r netconsole 2>/dev/null
modprobe netconsole netconsole="+6665@$src/$dev,$port@$rx/$mac" || { echo "netconsole failed"; exit 1; }
echo 1 > /sys/power/pm_debug_messages
echo $((0x2 | 0x4 | 0x80000)) > /sys/module/brcmfmac/parameters/debug 2>/dev/null
echo 8 > /proc/sys/kernel/printk
echo "netconsole: $src ($dev) -> $rx ($mac):$port, extended format; printk level 8, pm_debug on, brcmfmac debug $(cat /sys/module/brcmfmac/parameters/debug 2>/dev/null)"
echo "netconsole test $(date '+%T') from $(uname -r)" > /dev/kmsg
echo "a 'netconsole test' line must have appeared on the receiver"
