# 實驗 3：Kernel Slab Allocator 觀察

## 實驗目的

本實驗的目的不是重現 kernel exploit，也不是洩漏真實 kernel object 位址，而是觀察 Linux SLUB/slab allocator 的基本行為，建立後續理解論文攻擊鏈所需的 allocator 背景。

本實驗要驗證三件事：

1. Linux kernel 會把小型 kernel objects 放入固定大小的 slab cache。
2. 若已知某個 slab page 的 page-aligned base address，可以根據 cache 的 object size 推導 page 內所有 object slot 的候選位址。
3. User-space 程式可以透過大量建立 kernel objects，例如 pipes，間接改變 kernel slab allocator 的狀態。

這對應論文中的關鍵攻擊鏈：TLB side channel 先洩漏 4kB page-level address；slab allocator 的固定 slot layout 再把 page-level leak 轉換成 object-level candidate addresses。

---

## 背景知識

Linux kernel 的小物件通常不會直接向 page allocator 要一整個 4kB page，而是透過 slab allocator 管理。

簡化架構如下：

```text
Page Allocator
  ↓ 提供 4kB 或多頁連續記憶體
Slab Allocator
  ↓ 把 page 切成固定大小 slot
Kernel Objects
  cred, file, msg_msg, pipe_buffer, seq_file, ...
```

對於一個 slab cache，最重要的欄位是：

```text
object_size      每個 object slot 的大小
objs_per_slab    每個 slab 可容納多少個 object slot
order            每個 slab 使用多少個 page
objects          目前 cache 中的 object 數量
slabs            目前 cache 中的 slab 數量
partial          部分填滿的 slab 數量
cpu_slabs        per-CPU 暫存 slab 狀態
```

其中：

```text
slab_size = 4096 × 2^order
```

例如：

```text
order = 0  → slab_size = 4096 bytes
order = 1  → slab_size = 8192 bytes
```

若某個 cache 是 `object_size = 128` 且 `order = 0`，則：

```text
slab_size = 4096 bytes
objects_per_slab = 4096 / 128 = 32
object_address = slab_base + 128 × n, n = 0..31
```

---

## 實驗環境確認

先確認 kernel 使用的是 SLUB allocator：

```bash
uname -a
grep SLUB /boot/config-$(uname -r)
grep SLAB /boot/config-$(uname -r)
```

預期至少看到：

```text
CONFIG_SLUB=y
```

---

## Step 1：列出 slab cache

執行：

```bash
ls /sys/kernel/slab | grep '^kmalloc' | head -50
ls /sys/kernel/slab | grep '^kmalloc-cg' | head -50
```

目的：確認系統上有哪些 generic kmalloc cache，例如：

```text
kmalloc-64
kmalloc-128
kmalloc-256
kmalloc-cg-64
kmalloc-cg-128
kmalloc-cg-256
```

---

## Step 2：讀取 cache layout

執行 `read_cache_layout.sh`

本機觀察結果：

| cache | object_size | objs_per_slab | order | objects | slabs | partial | cpu_slabs |
|---|---:|---:|---:|---:|---:|---:|---:|
| kmalloc-64 | 64 | 64 | 0 | 45746 | 733 | 92 | 313 |
| kmalloc-128 | 128 | 32 | 0 | 8374 | 279 | 27 | 194 |
| kmalloc-256 | 256 | 32 | 1 | 43053 | 1351 | 39 | 134 |
| kmalloc-cg-64 | 64 | 64 | 0 | 6536 | 112 | 10 | 102 |
| kmalloc-cg-128 | 128 | 32 | 0 | 768 | 24 | 0 | 24 |
| kmalloc-cg-256 | 256 | 32 | 1 | 512 | 16 | 0 | 16 |

重點解讀：

```text
kmalloc-cg-128:
  object_size = 128
  objs_per_slab = 32
  order = 0
```

這表示 `kmalloc-cg-128` 是最適合拿來對應論文範例的 cache：一個 4kB slab page 被切成 32 個 128-byte slots。

---

## Step 3：page base → object slot candidates 推導

假設 TLB side channel 洩漏出某個 slab page base：

```text
slab_base = ffff8ae0f1203000
```

若該 page 屬於 `kmalloc-cg-128`，則：

```text
object_address = slab_base + 128 × n, n = 0..31
```

候選位址：

```text
slot  0: 0xffff8ae0f1203000
slot  1: 0xffff8ae0f1203080
slot  2: 0xffff8ae0f1203100
slot  3: 0xffff8ae0f1203180
...
slot 31: 0xffff8ae0f1203f80
```

這一步的重點：

```text
TLB side channel 給的是 4kB page-level address；
slab allocator 的固定 slot layout 讓我們能推導 page 內所有 object slot candidates。
```

這些是候選位址，不代表已經知道每個 slot 對應哪個具體 object。論文透過 allocator massaging 解決這個問題：讓整個 slab page 盡量都放入攻擊者控制的同類 object。

---

## Step 4：動態觀察 user-space 對 slab allocator 的影響

1. 執行 `read_slab_cache.sh`

觀察結果：

| cache | objects before | slabs before | partial before |
|---|---:|---:|---:|
| kmalloc-64 | 45337 | 733 | 135 |
| kmalloc-128 | 7817 | 272 | 45 |
| kmalloc-256 | 43053 | 1361 | 52 |
| kmalloc-cg-64 | 6536 | 112 | 10 |
| kmalloc-cg-128 | 768 | 24 | 0 |
| kmalloc-cg-256 | 512 | 16 | 0 |

2. 執行 `fake_pipe.py`， 建立 20000 個 pipes

先不要按 Enter 關掉

3. 再次執行 `read_slab_cache.sh`

觀察結果：

| cache | objects after | slabs after | partial after |
|---|---:|---:|---:|
| kmalloc-64 | 45337 | 733 | 135 |
| kmalloc-128 | 7817 | 272 | 45 |
| kmalloc-256 | 44992 | 1406 | 0 |
| kmalloc-cg-64 | 6536 | 112 | 10 |
| kmalloc-cg-128 | 768 | 24 | 0 |
| kmalloc-cg-256 | 512 | 16 | 0 |

4. 前後差異

| cache | Δ objects | Δ slabs | Δ partial |
|---|---:|---:|---:|
| kmalloc-64 | 0 | 0 | 0 |
| kmalloc-128 | 0 | 0 | 0 |
| kmalloc-256 | +1939 | +45 | -52 |
| kmalloc-cg-64 | 0 | 0 | 0 |
| kmalloc-cg-128 | 0 | 0 | 0 |
| kmalloc-cg-256 | 0 | 0 | 0 |

---

## 結果解讀

主要變化集中在 `kmalloc-256`：

```text
kmalloc-256:
  objects: 43053 → 44992  (+1939)
  slabs:   1361  → 1406   (+45)
  partial: 52    → 0      (-52)
```

這表示大量建立 pipes 後，kernel 在 `kmalloc-256` cache 中配置了大量物件，並且新增了 45 個 slabs。

由於 `kmalloc-256` 在 Step 2 中觀察到：

```text
object_size = 256
objs_per_slab = 32
order = 1
```

新增 45 個 slabs 代表新增容量最多：

```text
45 × 32 = 1440 slots
```

但 objects 實際增加了 1939。這不是矛盾，因為增加的 objects 來源包含：

```text
1. 新增 slab 提供的新 slots
2. 原本 partial slabs 中尚未使用的空 slots
```

`partial` 從 52 降到 0 支持這個解釋：原本部分填滿的 slabs 被進一步填滿，甚至移出 partial list。

---

## 實驗結論

### 結論 1：本機 slab cache 具有固定 slot layout

例如：

```text
kmalloc-cg-128:
  object_size = 128
  order = 0
  objs_per_slab = 32
```

因此，若知道某個 `kmalloc-cg-128` slab page 的 base address，便可推導：

```text
object_address = slab_base + 128 × n, n = 0..31
```

這說明 slab allocator 的 layout 規律可以把 4kB page-level leak 轉換為 page 內 object slot candidate addresses。

### 結論 2：page-level leak 足以產生 object-level candidates

TLB side channel 的自然解析度是 4kB page。若目標 object 位於 order-0 slab cache，則洩漏 page base 後，可以根據 slot size 推出該 page 內所有 candidate object addresses。

這不是直接知道「哪個 slot 是哪個具體 object」，但若 allocator massaging 讓整頁都是攻擊者控制的同類 object，候選位址就已經足以支撐後續 exploit。

### 結論 3：user-space 行為可以改變 kernel slab allocator 狀態

建立 20000 個 pipes 後，`kmalloc-256` 的狀態明顯變化：

```text
objects +1939
slabs   +45
partial -52
```

這說明 user-space 程式可以透過 syscall/object creation 觸發大量 kernel allocation，進而改變 kernel heap layout。這支撐 allocator massaging 的基本前提。

### 結論 4：本實驗的限制

本實驗沒有：

```text
1. 洩漏真實 kernel object address
2. 定位具體 pipe_buffer 或 msg_msg object
3. 進行 UAF/OOB exploit
4. 重現論文完整 TLB side-channel attack
```

本實驗的定位是 allocator 結構觀察，用於理解論文攻擊鏈中「TLB page leak → slab slot inference → object candidate addresses」這一段。