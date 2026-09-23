#!/usr/bin/env bash
# Read-only: lists every SMC key with its type, length and current data, and
# flags the ones whose names look like clock, wake, sleep or lid keys.
d=/sys/devices/platform/applesmc.768
n=$(cat $d/key_count)
out=${1:-/var/tmp/smc-all-keys.txt}
: > "$out"
for i in $(seq 0 $((n-1))); do
  echo $i > $d/key_at_index
  k=$(cat $d/key_at_index_name)
  printf '%s type=%s len=%s data=%s\n' "$k" "$(cat $d/key_at_index_type)" "$(cat $d/key_at_index_data_length)" "$(xxd -p $d/key_at_index_data 2>/dev/null)" >> "$out"
done
echo "all $n keys written to $out; candidates:"
grep -E '^(CL|MS|W|AW|SW|RT|TC|ALRM|CLW)' "$out"
