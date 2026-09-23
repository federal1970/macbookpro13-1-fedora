#!/usr/bin/env bash
# Read-only: the EC wake banks and the wake reason, for after a test. Root.
p=/sys/kernel/ec_poke
[ -d $p ] || insmod "$(dirname "$0")/ec_poke/ec_poke.ko" || exit 1
rd() { echo "$1" > $p/addr; cat $p/data; }
echo "SW(64,65)=$(rd 64)$(rd 65)  EW(68,69)=$(rd 68)$(rd 69)  LW(6c,6d)=$(rd 6c)$(rd 6d)  WKRS(06)=$(rd 06)  ECSS(11)=$(rd 11)"
echo "bits, low byte: 0 LO lid-open, 1 LC lid-close, 2 AI ac-in, 3 AR ac-out, 4 CI, 5 CE, 6 MI, 7 MR"
echo "bits, high byte: 0 PB power-button, 1 GP, 2 PM, 3 WT wake-timer, 4 LB, 5 DK"
echo "RTC alarm now: $(cat /sys/class/rtc/rtc0/wakealarm 2>/dev/null || echo none)"
