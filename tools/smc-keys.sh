#!/usr/bin/env bash
# Lists SMC keys related to battery charge limits. Read-only apart from the
# index selector; needs root because key_at_index is root-writable.
d=/sys/devices/platform/applesmc.768
n=$(cat $d/key_count)
for i in $(seq 0 $((n-1))); do
  echo $i > $d/key_at_index
  k=$(cat $d/key_at_index_name)
  case "$k" in BCLM|BFCL|CH0*|CHTE|BBIF|BRSC|B0FC|B0RM)
    printf '%s type=%s len=%s data=%s\n' "$k" "$(cat $d/key_at_index_type)" "$(cat $d/key_at_index_data_length)" "$(xxd -p $d/key_at_index_data 2>/dev/null)";;
  esac
done
