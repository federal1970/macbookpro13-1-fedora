#!/usr/bin/env bash
# Build dependencies for the Fedora kernel build (vm-build.sh). Run as root.
set -e
dnf install -y gcc make patch flex bison bc openssl-devel elfutils-libelf-devel dwarves perl python3 rsync git dnf-plugins-core xz cpio zstd
echo "done: $(gcc --version | head -1)"
