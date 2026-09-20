#!/usr/bin/env bash
# Does the out-of-tree Cirrus CS8409 audio driver break the hibernation snapshot?
# It is the one OE module that was loaded during every failed attempt so far.
# Runs test_resume, so nothing is powered off and the session survives.
set -u

REPORT=${REPORT:-/var/tmp/hib-noaudio-report.txt}
TARGET_UID=${TARGET_UID:-1000}
TIMEOUT=${TIMEOUT:-300}

[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
exec > >(tee "$REPORT") 2>&1
chmod 644 "$REPORT" 2>/dev/null

TARGET_USER=$(getent passwd "$TARGET_UID" | cut -d: -f1)
uctl() { runuser -u "$TARGET_USER" -- env XDG_RUNTIME_DIR="/run/user/$TARGET_UID" systemctl --user "$@"; }

AUDIO_UNITS="wireplumber pipewire-pulse pipewire"
AUDIO_SOCKETS="pipewire.socket pipewire-pulse.socket"
STOPPED_AUDIO=0
STOPPED_ALSA=0
REMOVED_MODS=""

cleanup() {
  echo
  echo "--- restoring ---"
  for m in $REMOVED_MODS; do modprobe "$m" 2>/dev/null && echo "reloaded $m"; done
  if [ "$STOPPED_ALSA" = 1 ]; then
    systemctl start alsa-state.service 2>/dev/null && echo "alsa-state started"
  fi
  if [ "$STOPPED_AUDIO" = 1 ]; then
    uctl start $AUDIO_SOCKETS 2>/dev/null
    uctl start $AUDIO_UNITS 2>/dev/null && echo "audio services started"
  fi
  echo "loaded now: $(lsmod | grep -cE '^snd_hda_codec_cs8409') cs8409"
  echo "report: $REPORT"
}
trap cleanup EXIT INT TERM

echo "=== hibernation without the out-of-tree audio driver, $(date '+%F %T') ==="
echo

# --- guards ---------------------------------------------------------------
if [ -w /sys/power/pm_test ] && ! grep -q '\[none\]' /sys/power/pm_test; then
  echo "pm_test was $(sed 's/.*\[\(.*\)\].*/\1/' /sys/power/pm_test) -- forcing none"
  echo none > /sys/power/pm_test
fi
echo 1 > /sys/power/pm_debug_messages 2>/dev/null
echo test_resume > /sys/power/disk
echo "pm_test : $(cat /sys/power/pm_test)"
echo "disk    : $(cat /sys/power/disk)"
echo "debug   : $(cat /sys/power/pm_debug_messages)"
grep -q '\[test_resume\]' /sys/power/disk || { echo "disk is not test_resume, refusing" >&2; exit 1; }

echo
echo "--- out-of-tree modules before ---"
for m in $(lsmod | awk 'NR>1{print $1}'); do
  t=$(cat "/sys/module/$m/taint" 2>/dev/null); [ -n "$t" ] && echo "  $m: $t"
done

# --- take the audio stack out --------------------------------------------
echo
echo "--- stopping audio ---"
uctl stop $AUDIO_SOCKETS 2>/dev/null
uctl stop $AUDIO_UNITS 2>/dev/null && STOPPED_AUDIO=1
# alsactl from alsa-state.service holds /dev/snd/controlC0 and pins the card,
# which pins snd_hda_intel, which pins the codec module we are trying to remove.
systemctl stop alsa-state.service 2>/dev/null && STOPPED_ALSA=1 && echo "alsa-state stopped"
sleep 1
echo "still holding /dev/snd:"
fuser -v /dev/snd/* 2>&1 | head -5 || echo "  (nobody)"

echo
echo "--- unloading (controller first, then the codecs it pins) ---"
for m in snd_hda_intel snd_hda_codec_cs8409 snd_hda_scodec_component snd_hda_codec_generic; do
  lsmod | grep -q "^$m " || continue
  if modprobe -r "$m" 2>/dev/null; then
    REMOVED_MODS="$m $REMOVED_MODS"
    echo "removed $m"
  else
    echo "could not remove $m (in use)"
  fi
done
lsmod | grep -q '^snd_hda_codec_cs8409 ' && {
  echo
  echo "ABORT: snd_hda_codec_cs8409 is still loaded, the test would prove nothing."
  exit 1
}
echo "cs8409 is gone"

echo
echo "--- out-of-tree modules now ---"
FOUND=0
for m in $(lsmod | awk 'NR>1{print $1}'); do
  t=$(cat "/sys/module/$m/taint" 2>/dev/null); [ -n "$t" ] && { echo "  $m: $t"; FOUND=1; }
done
[ "$FOUND" = 0 ] && echo "  (none -- no out-of-tree module is loaded)"

# --- run it ---------------------------------------------------------------
CURSOR=$(journalctl -k -n0 --show-cursor 2>/dev/null | sed -n 's/^-- cursor: //p')
START=$(date +%s)
echo
echo "--- hibernating (test_resume: writes a real image, restores it, no power-off) ---"
sync
systemctl hibernate

for _ in $(seq 1 "$TIMEOUT"); do
  journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null | grep -q 'hibernation exit' && break
  sleep 1
done
END=$(date +%s)

echo
echo "=== result ==="
printf 'cycle took %s s\n' "$((END - START))"
LOG=$(journalctl -k --after-cursor="$CURSOR" --no-pager 2>/dev/null | grep -viE 'FWMSG|\[ISP\]|Marking nosave|Calling |smpboot|CPU[0-9]')

echo
echo "--- the decisive lines ---"
if grep -q 'Image created' <<<"$LOG"; then
  echo "SNAPSHOT COMPLETED:"
  grep -E 'Image created|Wrote|Image saving|Saving image' <<<"$LOG" | sed 's/^/  /'
else
  echo "STILL BROKEN -- no 'Image created (N pages copied)'."
fi
grep -q 'Hibernation image restored successfully' <<<"$LOG" \
  && echo "  and the kernel still took the 'restored' branch." \
  || echo "  the 'restored' branch was NOT taken."

echo
echo "--- kernel log of the cycle ---"
echo "$LOG" | tail -45
