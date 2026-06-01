# 實驗 3：觀察 Kernel Slab Allocator

## 目的

不寫攻擊程式，直接用 Linux 核心提供的介面觀察 Slab Allocator 的狀態，
理解「同類物件住在同一個 slab page」的具體結構，

---

## 使用的介面

```
/proc/slabinfo            → 所有 cache 的動態統計（active 物件數等）
/sys/kernel/slab/<name>/  → 單一 cache 的靜態結構資訊
```

兩者互補：`/proc/slabinfo` 看變化，`/sys/kernel/slab/` 看結構。

---

## 背景：msg_msg 落入哪個 cache？

Slab allocator 依照物件大小分配到不同的 cache：

```
msg_msg header：約 48 bytes
+ 我們的 payload：  72 bytes
= 總計：           120 bytes

→ 向上取整至最近的 2 的冪次：128 bytes
→ 落入 kmalloc-cg-128
```

`-cg-` 代表 Control Group aware，Ubuntu 預設啟用，
同一 cgroup 的物件放在同一個 cache，提高安全性隔離。

---

## 手動觀察指令

### 查看所有 cache

```bash
# 欄位：name / active_objs / num_objs / objsize / objs_per_slab / pages_per_slab
cat /proc/slabinfo | grep kmalloc-cg

# 只看 msg_msg 相關的 cache
cat /proc/slabinfo | grep -E "^(kmalloc-cg-128|msg_msg)"
```

### 查看 cache 的詳細結構

```bash
# 每個物件的大小
cat /sys/kernel/slab/kmalloc-cg-128/object_size

# 每個 slab page 有幾個物件
cat /sys/kernel/slab/kmalloc-cg-128/objs_per_slab

# slab page 的 order（大小 = 2^order × 4kB）
cat /sys/kernel/slab/kmalloc-cg-128/order

# 物件的對齊要求
cat /sys/kernel/slab/kmalloc-cg-128/align
```

### 分配前後對比

```bash
# 分配前
grep kmalloc-cg-128 /proc/slabinfo

# 執行分配程式
sudo ./slab_observe

# 分配後
grep kmalloc-cg-128 /proc/slabinfo
```

---

## 編譯與執行

```bash
gcc -O0 -o slab_observe slab_observe.c
sudo ./slab_observe
```

---

## 實際觀測結果

### /sys/kernel/slab/kmalloc-cg-128/ 的結構

```
object_size   = 128 bytes   （每個物件佔用的空間）
objs_per_slab = 32 個        （每個 slab page 有 32 個 slot）
order         = 0            （slab page 大小 = 2^0 × 4kB = 4kB）
align         = 128 bytes   （物件的對齊要求）

驗算：4096B ÷ 128B = 32 個物件  ✓
```

> **VM 注意事項：** VirtualBox 環境下，`/proc/slabinfo` 的 `active_objs`
> 可能不即時反映變化。在實體機上，分配 64 個 msg_msg 後應看到
> `active_objs` 增加約 64。核心結構資訊（object_size、objs_per_slab）
> 來自 `/sys/kernel/slab/`，不受此影響，數字是準確的。

---

## Slab Page 的記憶體佈局

根據觀測結果，kmalloc-cg-128 的一個 slab page 長這樣：

```
一個 slab page（4096 bytes = 4kB）
┌─────────────────────────────────────────────────────────┐
│ slot 0  │ slot 1  │ slot 2  │ ... │ slot 31             │
│ 128B    │ 128B    │ 128B    │ ... │ 128B                │
└─────────────────────────────────────────────────────────┘
  位址：                                    
  slab_base + 0      slab_base + 128  ...  slab_base + 128×31

每個 slot 可放一個 msg_msg 物件（120B，對齊到 128B）
```

攻擊者只要知道 `slab_base`，每個物件的位址就能直接計算：

```
msg_msg[n].addr = slab_base + 128 × n   （n = 0, 1, ..., 31）
```

---

## 用指令直接看 cache 清單

```bash
# 列出所有 kmalloc 相關的 cache，確認 -cg- 版本存在
cat /proc/slabinfo | head -2   # 印出欄位標題
cat /proc/slabinfo | grep kmalloc-cg

# 典型輸出（欄位：name active_objs num_objs objsize objs_per_slab）：
# kmalloc-cg-8192    4       4       8192    4       1
# kmalloc-cg-4096    8       8       4096    8       1
# kmalloc-cg-2048    8       8       2048    8       1
# kmalloc-cg-1024   16      16       1024   16       1
# kmalloc-cg-512    32      32        512   32       1
# kmalloc-cg-256    64      64        256   16       1
# kmalloc-cg-192    63      63        192   21       1
# kmalloc-cg-128   240     256        128   32       1  ← msg_msg 在這裡
# kmalloc-cg-96    126     126         96   42       1
# kmalloc-cg-64    448     448         64   64       1
```

---

## 與論文的關係

| 本實驗的觀察 | 在論文攻擊中的作用 |
|------------|-----------------|
| msg_msg 落入 kmalloc-cg-128 | 決定攻擊要針對哪個 cache |
| 每頁 32 個 slot | 知道 massaging 需要分配幾個物件 |
| 物件間距 128 bytes | TLB 洩漏頁面位址後，計算每個物件的位址 |
| slab page 大小 = 4kB | 確認是 4kB 映射，TLB 側通道才有效 |

最後一點特別重要：slab page 是 4kB（order=0），所以它在 DPM 中佔用一個 4kB 頁面。論文的防禦機制（D1/D2/D3）把這個 4kB 頁面暴露在 TLB 側通道下，讓攻擊者能找到 `slab_base`。
