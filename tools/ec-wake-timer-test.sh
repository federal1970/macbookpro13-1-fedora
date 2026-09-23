#!/usr/bin/env bash
# Experiment: can the EC's "enable wake timer" bit let an RTC alarm wake S3
# with the lid shut? Sets ENWT (EC RAM 0x69, bit 3), arms a plain RTC alarm
# N seconds ahead without sleeping, prints the EC wake banks. Root.
# Usage: ec-wt-arm.sh 180
n=${1:-180}
p=/sys/kernel/ec_poke
[ -d $p ] || insmod "$(dirname "$0")/ec_poke/ec_poke.ko" || exit 1
rd() { echo "$1" > $p/addr; cat $p/data; }
bank() { echo -n "$1  SW(64,65)=$(rd 64)$(rd 65)  EW(68,69)=$(rd 68)$(rd 69)  LW(6c,6d)=$(rd 6c)$(rd 6d)  WKRS(06)=$(rd 06)"; echo; }
bank before
for a in 65 69; do
  b=$(rd $a)
  echo $a > $p/addr; printf '%02x' $(( 0x$b | 0x08 )) > $p/data
  echo "0x$a: $b -> $(rd $a)   (bit 3 = WT; 0x65 is the SW bank, 0x69 the EW bank)"
done
rtcwake -m no -s "$n" && echo "$(date +%H:%M:%S) RTC alarm armed for +$n s ($(cat /sys/class/rtc/rtc0/wakealarm))"
bank after
echo "Now shut the lid and leave it shut for 6 minutes."
