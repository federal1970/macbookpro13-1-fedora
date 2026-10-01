#!/usr/bin/env bash
# Remove a test kernel installed by laptop-install.sh: DKMS modules built
# for it, the BLS entry + initramfs (kernel-install remove), the modules
# tree, System.map and config. Root. Refuses stock Fedora releases.
set -u
release=${1:?usage: $0 <release, e.g. 7.2.7-applespi>}
case "$release" in *.fc[0-9]*) echo "$release looks like a stock kernel; use dnf for those"; exit 1;; esac
[ "$(uname -r)" = "$release" ] && { echo "$release is the running kernel; boot something else first"; exit 1; }
[ -d "/lib/modules/$release" ] || echo "note: /lib/modules/$release does not exist"
for m in $(dkms status 2>/dev/null | awk -F'[,: ]+' -v k="$release" '$3==k {print $1"/"$2}' | sort -u); do
  echo "dkms remove $m for $release"; dkms remove "$m" -k "$release" >/dev/null 2>&1 || true
done
kernel-install remove "$release" && echo "boot entry and initramfs removed"
rm -rf "/lib/modules/$release"; rm -f "/boot/System.map-$release" "/boot/config-$release" "/boot/vmlinuz-$release" "/boot/initramfs-$release.img"
rm -rf "/var/lib/dkms/"*"/"*"/$release" 2>/dev/null
echo "left in /boot:"; ls /boot/vmlinuz-* | sed 's|/boot/vmlinuz-||'
echo "default: $(grubby --default-kernel)"
