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
tmp=$(mktemp -d)
tar -C "$tmp" --zstd -xf "$tarball"
# the release is whatever lib/modules/ in the tarball says, not the file name
release=$(ls "$tmp/lib/modules/" | head -1)
[ -n "$release" ] && [ -f "$tmp/boot/vmlinuz-$release" ] || { echo "no kernel in $tarball"; exit 1; }
echo "installing $release"
# DKMS keeps per-kernel state and an archive of the "original" in-tree module
# it replaced; if the tree is wiped underneath it, a later `dkms remove`
# restores that stale archive over the new kernel's module (it did: a
# control-build applesmc.ko landed on top of the one carrying the series).
# So forget every DKMS module for this release before replacing the tree.
for m in $(dkms status 2>/dev/null | awk -F'[,: ]+' -v k="$release" '$3==k {print $1"/"$2}' | sort -u); do
  dkms remove "$m" -k "$release" >/dev/null 2>&1 || true
done
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
# out-of-tree modules this machine needs; applesmc-bclm is deliberately NOT
# here so the in-tree applesmc (with whatever series is under test) runs
if [ -n "$buildtree" ]; then
  for m in facetimehd/0.6.13 snd_hda_macbookpro/0.1; do
    dkms install --force "$m" -k "$release" || echo "dkms $m failed"
  done
  depmod -a "$release"
fi
echo "installed $release; entries:"
ls /boot/loader/entries/ 2>/dev/null || grubby --info=ALL | grep -E '^(kernel|title)'
echo "reboot and pick $release in GRUB; the stock kernel stays the default"
