#!/usr/bin/env bash
# Installs a kernel built by vm-build.sh on this machine. Root.
# Usage: laptop-install.sh <release>.tar.zst [build-tree]
# build-tree: a local kernel source tree built with the same config and
# release, linked as /lib/modules/<release>/build so the DKMS hook can
# rebuild the out-of-tree modules (camera, audio, applesmc).
# Adds a BLS boot entry next to the stock kernel (kernel-install builds the
# initramfs with dracut and runs the dkms hook); the stock entry stays.
set -euo pipefail
tarball=${1:?tarball from vm-build.sh}
buildtree=${2:-}
release=$(basename "$tarball" .tar.zst)
tmp=$(mktemp -d)
tar -C "$tmp" --zstd -xf "$tarball"
[ -d "$tmp/lib/modules/$release" ] || { echo "no modules for $release in $tarball"; exit 1; }
rm -rf "/lib/modules/$release"
cp -a "$tmp/lib/modules/$release" /lib/modules/
cp "$tmp/boot/System.map-$release" "$tmp/boot/config-$release" /boot/
# the tarball was made by an unprivileged user and unpacked under /tmp:
# give the tree root ownership and its proper SELinux label, or the kernel
# refuses module_load after switch-root and the boot ends in emergency mode
chown -R root:root "/lib/modules/$release" "/boot/System.map-$release" "/boot/config-$release"
restorecon -R "/lib/modules/$release" "/boot/System.map-$release" "/boot/config-$release"
if [ -n "$buildtree" ]; then
  [ "$(cat "$buildtree/include/config/kernel.release" 2>/dev/null)" = "$release" ] \
    || { echo "build tree $buildtree is not release $release"; exit 1; }
  ln -sfn "$buildtree" "/lib/modules/$release/build"
fi
depmod -a "$release"
kernel-install add "$release" "$tmp/boot/vmlinuz-$release"
rm -rf "$tmp"
echo "installed $release; entries:"
ls /boot/loader/entries/ 2>/dev/null || grubby --info=ALL | grep -E '^(kernel|title)'
echo "reboot and pick $release in GRUB; the stock kernel stays the default"
