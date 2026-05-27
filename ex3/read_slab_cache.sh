sudo -v
for c in kmalloc-64 kmalloc-128 kmalloc-256 kmalloc-cg-64 kmalloc-cg-128 kmalloc-cg-256; do                     
  if [ -d /sys/kernel/slab/$c ]; then
    echo "===== $c baseline ====="                                             
    echo -n "objects: "
    sudo cat /sys/kernel/slab/$c/objects 2>/dev/null
    echo -n "slabs:   "
    sudo cat /sys/kernel/slab/$c/slabs 2>/dev/null
    echo -n "partial: "
    sudo cat /sys/kernel/slab/$c/partial 2>/dev/null
  fi
done