#!/usr/bin/env bash
# Backlog item 3: does the lid read "closed" at boot without
# button.lid_init_state=open? Boot WITHOUT that parameter and with
# systemd.unit=multi-user.target (no desktop to react to a wrong lid state),
# log in on the console, run this as root, reboot normally. The answer is
# the state the kernel reports right after boot with the lid open; a lid
# cycle is not needed.
log=/home/${SUDO_USER:-user}/dev/kernel-test/params-test.log
exec > >(tee -a "$log") 2>&1
echo "### $(date '+%F %T') kernel $(uname -r)"
echo "cmdline: $(cat /proc/cmdline)"
grep -oE 'lid_init_state=[a-z]+' /proc/cmdline || echo "(no lid_init_state parameter: kernel default 'method', i.e. ask _LID at boot)"
echo "### lid state the kernel reports now (the lid IS open):"
cat /proc/acpi/button/lid/LID0/state
echo "### lid events since boot (kernel and logind):"
journalctl -b --no-pager -o short-iso | grep -iE 'lid (closed|opened)|button.*lid|LID0' | grep -vE 'audit|Watching' | cut -c12-120 | head -8
echo "(a 'closed' state or a 'Lid closed' event here, with the lid open, is the bug the DMI quirk would fix)"
echo "### firmware side: _LID returns EC.ELSW; EC ready flag and lid switch raw value if exposed"
cat /sys/bus/acpi/devices/PNP0C0D:00/path 2>/dev/null | sed "s/^/lid device: /"
echo "### done; reboot into the normal entry"
