sudo -v
for c in kmalloc-64 kmalloc-128 kmalloc-256 kmalloc-cg-64 kmalloc-cg-128 kmalloc-cg-256; do
  if [ -d /sys/kernel/slab/$c ]; then
    echo "===== $c ====="
    for f in object_size objs_per_slab order objects slabs partial cpu_slabs; do
      if [ -f /sys/kernel/slab/$c/$f ]; then
        printf "%-16s " "$f"
        sudo cat /sys/kernel/slab/$c/$f
      fi
    done
  fi
done