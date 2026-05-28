#!/usr/bin/env bash
set -euo pipefail

out="${1:-slab_snapshot.txt}"

if [ "$(id -u)" -ne 0 ]; then
  echo "Please run with sudo:"
  echo "  sudo $0 $out"
  exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

declare -A seen

read_field() {
  local cache_dir="$1"
  local field="$2"

  if [ -r "$cache_dir/$field" ]; then
    awk '{print $1}' "$cache_dir/$field"
  else
    echo "NA"
  fi
}

# Only scan first-level real directories under /sys/kernel/slab.
# This avoids duplicate matches caused by overlapping shell globs.
while IFS= read -r -d '' c; do
  name="$(basename "$c")"

  case "$name" in
    kmalloc-*|kmalloc-cg-*|kmalloc-rnd-*|kmalloc-rcl-*)
      ;;
    *)
      continue
      ;;
  esac

  # De-duplicate by cache name.
  if [[ -n "${seen[$name]+x}" ]]; then
    continue
  fi
  seen[$name]=1

  object_size="$(read_field "$c" object_size)"
  objs_per_slab="$(read_field "$c" objs_per_slab)"
  order="$(read_field "$c" order)"
  objects="$(read_field "$c" objects)"
  slabs="$(read_field "$c" slabs)"
  partial="$(read_field "$c" partial)"

  printf "%s %s %s %s %s %s %s\n" \
    "$name" "$object_size" "$objs_per_slab" "$order" "$objects" "$slabs" "$partial" \
    >> "$tmp"

done < <(find /sys/kernel/slab -mindepth 1 -maxdepth 1 -type d -print0)

sort -u "$tmp" > "$out"

echo "wrote $out"
echo "rows: $(wc -l < "$out")"
