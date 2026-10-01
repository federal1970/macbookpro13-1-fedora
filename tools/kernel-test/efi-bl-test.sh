#!/usr/bin/env bash
# Second review point on the applespi patch: at power-off after hibernation
# the driver must save the real backlight level to EFI, not the blanked 0.
# Root. The level is read back straight from the EFI variable, so no cold
# boot is needed: resume from the image, run "after".
#   efi-bl-test.sh before    set the backlight to 200, record the EFI value
#   systemctl hibernate      (machine powers off, HibernateMode=shutdown)
#   power on, resume, then:  efi-bl-test.sh after
L=/sys/class/leds/spi::kbd_backlight/brightness
V=/sys/firmware/efi/efivars/KeyboardBacklightLevel-a076d2af-9678-4386-8b58-1fc8ef041619
log=/home/${SUDO_USER:-user}/dev/kernel-test/efi-bl-test.log
exec > >(tee -a "$log") 2>&1
efi() { od -An -tu2 -j4 "$V" | tr -d ' '; }
echo "### $(date '+%F %T') kernel $(uname -r) ${1:-?}"
case "${1:-}" in
before)
  echo 200 > "$L"; sleep 1
  echo "brightness now $(cat $L); EFI value before hibernation: $(efi); boot_id $(cat /proc/sys/kernel/random/boot_id)"
  echo "now: systemctl hibernate; power on; after the resume run: $0 after";;
after)
  echo "brightness now $(cat $L); boot_id $(cat /proc/sys/kernel/random/boot_id) (same as before = real resume)"
  echo "EFI value saved at the hibernation power-off: $(efi)   (206 = fixed: the driver stores the hardware level, 200*223/255+32; 32 = the bug, blanked)"
  journalctl -k -b --no-pager -o short-iso | grep -E 'hibernation entry|hibernation exit|PM: Image|applespi' | tail -5 | cut -c12-120;;
*) echo "usage: $0 before|after"; exit 1;;
esac
