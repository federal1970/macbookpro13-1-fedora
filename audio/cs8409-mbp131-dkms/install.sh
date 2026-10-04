#!/usr/bin/env bash
# Install this driver through DKMS, in place of the out-of-tree replay driver
# (snd_hda_macbookpro). Run with sudo.
#
#   sudo ./install.sh              for the newest installed 7.2 kernel
#   sudo ./install.sh 7.2.8-...    for that kernel
#
# Only one kernel is built here: every `dkms install` makes Fedora rebuild that
# kernel's initramfs, which takes minutes. Other kernels get the module from
# DKMS itself (AUTOINSTALL) when they are installed or booted.
#
# It does not unload or load any module: the running sound driver stays as it
# is until a reboot (or `cs8409-dev stock`, the development helper in
# ../upstream).
set -eu

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

SRC=$(dirname "$(readlink -f "$0")")
PACKAGE_NAME=$(sed -n 's/^PACKAGE_NAME="\(.*\)"$/\1/p' "$SRC/dkms.conf")
PACKAGE_VERSION=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"$/\1/p' "$SRC/dkms.conf")
[ -n "$PACKAGE_NAME" ] && [ -n "$PACKAGE_VERSION" ] || { echo "bad dkms.conf"; exit 1; }
NEW="$PACKAGE_NAME/$PACKAGE_VERSION"

if [ $# -ge 1 ]; then
	KVER=$1
else
	KVER=$(for b in /lib/modules/7.2.*/build; do
		[ -d "$b" ] && basename "$(dirname "$b")"
	done | sort -V | tail -n 1)
fi
[ -n "$KVER" ] && [ -d "/lib/modules/$KVER/build" ] || { echo "no build tree for kernel '$KVER'"; exit 1; }
echo "kernel: $KVER"

echo "== 1. the old replay driver out of DKMS (its checkout is not touched)"
if dkms status snd_hda_macbookpro | grep -q .; then
	dkms remove snd_hda_macbookpro/0.1 --all
fi
if [ -L /usr/src/snd_hda_macbookpro-0.1 ]; then
	rm /usr/src/snd_hda_macbookpro-0.1
fi

echo "== 2. register $NEW"
if ! dkms status "$NEW" | grep -q .; then
	if [ -d "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION" ] && [ ! -L "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION" ]; then
		rm -rf "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION"
	fi
	dkms add "$SRC"
	# dkms keeps the owner of the files it copies; root builds from this copy
	chown -R root:root "/usr/src/$PACKAGE_NAME-$PACKAGE_VERSION"
fi
# older versions of this package, on every kernel
dkms status "$PACKAGE_NAME" | sed -n "s|^$PACKAGE_NAME/\([^,]*\),.*|\1|p" | sort -u | while read -r v; do
	if [ "$v" != "$PACKAGE_VERSION" ]; then
		dkms remove "$PACKAGE_NAME/$v" --all
		if [ -d "/usr/src/$PACKAGE_NAME-$v" ]; then
			rm -rf "/usr/src/$PACKAGE_NAME-$v"
		fi
	fi
done

echo "== 3. build and install for $KVER"
if dkms status "$NEW" -k "$KVER" | grep -q installed; then
	echo "already installed"
else
	dkms install "$NEW" -k "$KVER"
fi

echo "== result"
dkms status | grep -E 'cs8409|macbookpro' || true
echo "$KVER: $(modinfo -k "$KVER" -F filename snd_hda_codec_cs8409 2>/dev/null || echo 'no module')"
echo "The running driver is unchanged. Reboot to switch to the installed module."
