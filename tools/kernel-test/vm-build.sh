#!/usr/bin/env bash
# Builds a Fedora kernel with optional extra patches on a build host and
# packages the result for installation on the MacBook.
#
# Usage on the build host:
#   vm-build.sh <fedora-kernel-nvr> <localversion> <config-file> [patch...]
#   e.g. vm-build.sh 7.2.7-200.fc44 -jordantest laptop.config v3-*.patch
#
# Output: ~/kbuild/<release>.tar.zst with boot/vmlinuz-<release>,
# boot/System.map-<release>, boot/config-<release> and lib/modules/<release>/.
set -euo pipefail
nvr=${1:?fedora kernel NVR, e.g. 7.2.7-200.fc44}
localversion=${2:?localversion, e.g. -jordantest}
config=${3:?config file}
shift 3
patches=("$@")

ver=${nvr%%-*}                # 7.2.7
major=${ver%.*}               # 7.2
work=~/kbuild
mkdir -p "$work" && cd "$work"

deps=(gcc make patch flex bison bc openssl-devel elfutils-libelf-devel dwarves perl python3 rsync git dnf-plugins-core xz cpio zstd)
missing=()
for p in "${deps[@]}"; do rpm -q "$p" >/dev/null 2>&1 || missing+=("$p"); done
if [ ${#missing[@]} -gt 0 ]; then
  echo "installing: ${missing[*]}"; sudo dnf install -y "${missing[@]}"
fi

if [ ! -f "kernel-$nvr.src.rpm" ]; then
  dnf download --quiet --source "kernel-$nvr"
fi
if [ ! -d "src-$nvr" ]; then
  mkdir "src-$nvr" && (cd "src-$nvr" && rpm2cpio "../kernel-$nvr.src.rpm" | cpio -idm --quiet)
fi
tree="linux-$ver$localversion"
if [ ! -f "$tree/.fedora-prepared" ]; then
  rm -rf "$tree" "linux-$ver"
  tar -xJf "src-$nvr/linux-$ver.tar.xz" && mv "linux-$ver" "$tree"
  ( cd "$tree"
    patch -p1 -s < "../src-$nvr/patch-$major-redhat.patch"
    cp "../src-$nvr/Makefile.rhelver" .
    touch .fedora-prepared
  )
fi
cd "$tree"
for p in "${patches[@]}"; do
  echo "applying $p"; patch -p1 --forward < "$p"
done
cp "$config" .config
make olddefconfig >/dev/null
release=$(make -s LOCALVERSION="$localversion" kernelrelease)
echo "building $release on $(nproc) cpus"
time make -j"$(nproc)" LOCALVERSION="$localversion"

out="$work/out-$release"; rm -rf "$out"; mkdir -p "$out/boot"
make -s LOCALVERSION="$localversion" INSTALL_MOD_PATH="$out" INSTALL_MOD_STRIP=1 modules_install
cp arch/x86/boot/bzImage "$out/boot/vmlinuz-$release"
cp System.map "$out/boot/System.map-$release"
cp .config "$out/boot/config-$release"
rm -f "$out/lib/modules/$release/build" "$out/lib/modules/$release/source"
tar -C "$out" --zstd -cf "$work/$release.tar.zst" boot lib
ls -l "$work/$release.tar.zst"
echo "DONE $release"
