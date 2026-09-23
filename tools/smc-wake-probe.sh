#!/usr/bin/env bash
# Read-only: prints type, length and R/W flags of the wake/clock candidates.
d=/sys/devices/platform/applesmc.768
for k in CLWK CLKT CLSD WKEN WKTP MSWr MSSF MSPS MSLD MSDW BCLM; do
  echo "$k" > $d/key_name; printf '%-40s data=%s\n' "$(cat $d/key_name)" "$(cat $d/key_data)"
done
