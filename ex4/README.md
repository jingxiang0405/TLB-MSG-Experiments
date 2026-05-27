# 實驗 4：模擬 Allocator Massaging 行為

## 實驗目的

本實驗的目標是在 user space 建立一個簡化版 slab allocator 模型，模擬論文中的 allocator massaging 行為。

本實驗不會：

- 操作真實 kernel allocator
- 洩漏 kernel address
- 觸發 UAF / OOB vulnerability
- 執行 kernel exploit

本實驗要展示的是：

```text
初始 heap layout 可能混有不同 object
→ 先用 dummy allocation 填滿舊的 partial slab
→ 再大量 spray attacker-controlled objects
→ 讓新的 slab page 整頁都是 attacker-controlled objects
→ 若 TLB side channel 洩漏該 slab page base，就能推導頁內 object candidates
```

這對應論文中的核心概念：location disclosure attack 不只靠 TLB side channel，也需要先用 allocator massaging 讓目標 object 所在的 slab page 具有可預測 layout。

---

## 實驗背景

前一個實驗已經觀察到 Linux slab allocator 會將小型 kernel objects 放入固定大小的 cache。例如：

```text
kmalloc-cg-128:
  object_size = 128
  order = 0
  objs_per_slab = 32
```

這代表一個 4kB slab page 可以被切成 32 個 128-byte object slots。

若 TLB side channel 洩漏出某個 slab page base：

```text
slab_base = 0xffff8ae0f1201000
```

則該頁內候選 object 位址為：

```text
object_addr = slab_base + 128 × n, n = 0..31
```

但這個推導只有在「該 slab page 內的 object layout 可控」時才有 exploit 價值。若該頁混有 unrelated objects，即使知道 page base，也不能穩定知道哪些 slot 是攻擊者控制的 object。

Allocator massaging 的目的就是把 layout 整理成：

```text
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
```

其中每個 `A` 都是 attacker-controlled object。

---

## 模擬模型

本實驗使用簡化模型：

```text
PAGE_SIZE = 4096
SLOT_SIZE = 128
SLOTS_PER_SLAB = 4096 / 128 = 32
```

每個 slot 用一個字元表示：

```text
. = free slot
V = victim / unrelated object
D = dummy object
A = attacker-controlled object
```

這些符號對應到真實 kernel exploit 概念：

| 符號 | 模擬意義 | 真實攻擊中的類比 |
|---|---|---|
| `.` | free slot | allocator 中尚未使用的空位 |
| `V` | victim / unrelated object | 系統中原本存在、非攻擊者控制的 kernel object |
| `D` | dummy object | padding allocation，用來填滿舊洞 |
| `A` | attacker-controlled object | `msg_msg`、`pipe_buffer`、`file`、`seq_file` 等可大量建立的物件 |

---

## 編譯與執行

編譯：

```bash
gcc -O0 allocator_massaging_sim.c -o allocator_massaging_sim
```

執行：

```bash
./allocator_massaging_sim
```

---

## 6. 預期輸出與解讀

### Phase 1：初始混雜 slab

預期看到類似：

```text
slab 00 base=0xffff8ae0f1200000  VVVVVVVVVVDDDDDDDDAAAAAA........  .:8 A:6 D:8 V:10
```

解讀：

```text
V = 10 個 unrelated / victim objects
D = 8 個 dummy objects
A = 6 個 attacker-controlled objects
. = 8 個 free slots
```

這是一個 partial slab。它還有空位，而且混有多種 object。這種狀態不適合作為穩定 exploit target。

若現在直接 spray attacker objects，新的 `A` 會先落進這 8 個 free slots，使該 page 繼續保持混雜狀態。

---

### Phase 2：用 dummy objects 填滿 partial slab

預期看到：

```text
slab 00 base=0xffff8ae0f1200000  VVVVVVVVVVDDDDDDDDAAAAAADDDDDDDD  .:0 A:6 D:16 V:10
```

解讀：

```text
原本的 8 個 free slots 被 D 填滿
slab 00 不再有空位
```

這一步稱為 drain 或 padding。

目的：

```text
消耗掉 allocator 既有 partial slab 的空位
避免下一波 attacker objects 被塞進混雜 slab
迫使後續 allocation 開新的 slab page
```

---

### Phase 3：target spray 產生 all-A slab

預期看到：

```text
slab 00 base=0xffff8ae0f1200000  VVVVVVVVVVDDDDDDDDAAAAAADDDDDDDD  .:0 A:6 D:16 V:10
slab 01 base=0xffff8ae0f1201000  AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA  .:0 A:32 D:0 V:0
```

解讀：

```text
slab 00 已滿，且不是攻擊目標
slab 01 是新開的 slab page
slab 01 內 32 個 slots 全部都是 attacker-controlled object A
```

這代表 allocator massaging 成功。

---

## 7. 候選 object 位址推導

程式最後會列出：

```text
Assume TLB side channel leaks slab_base = 0xffff8ae0f1201000
slot_size = 128 bytes
formula: object_addr = slab_base + slot_size * n
```

並輸出：

```text
slot 00: 0xffff8ae0f1201000
slot 01: 0xffff8ae0f1201080
slot 02: 0xffff8ae0f1201100
...
slot 31: 0xffff8ae0f1201f80
```

推導邏輯是：

```text
slot_size = 128 bytes = 0x80
slot n = slab_base + 0x80 × n
```

這對應論文中的 sub-page object location inference：TLB side channel 先洩漏 page-aligned address，allocator layout 再推導 page 內所有 object slots。

---

## 8. 為什麼不能省略 Phase 2

若省略 Phase 2，直接執行 attacker spray，結果可能會變成：

```text
slab 00:
VVVVVVVVVVDDDDDDDDAAAAAAAAAAAAAA
```

這時 page 內仍混有：

```text
V object
D object
A object
```

即使 TLB side channel 洩漏了 `slab 00` 的 base，也不能保證所有 candidate addresses 都是 attacker-controlled objects。

這會造成：

```text
1. 可能誤判 object location
2. 可能打到 unrelated object
3. exploit 穩定性下降
4. crash 風險上升
```

因此 allocator massaging 需要先 drain/pad partial slab，再做 target spray。

---

## 9. 與論文攻擊流程的對應

論文中的 location disclosure attack 可以拆成兩部分：

```text
Allocator massaging:
  讓目標 object 所在 slab page 具有可預測 layout

TLB side channel:
  偵測 access primitive 觸碰了哪個 4kB page
```

本實驗模擬的是第一部分。

完整攻擊鏈可寫成：

```text
1. 攻擊者透過 allocation primitive 建立大量同類 kernel objects
2. 用 dummy allocation 消耗舊 partial slab 的 free slots
3. spray attacker-controlled objects，產生整頁 all-attacker-controlled slab
4. access primitive 觸碰目標 object
5. TLB Evict+Reload 洩漏該 object 所在的 4kB page base
6. 根據 slot_size 推導 page 內 object candidate addresses
```

本實驗只做第 1 到第 3，以及第 6 的概念推導。

---

## 10. 實驗結論

### 結論 1：初始 slab layout 通常不可控

Phase 1 顯示，一個 partial slab 可能同時包含 victim、dummy、attacker object，以及 free slots。這種 layout 無法直接支撐穩定 exploit。

```text
VVVVVVVVVVDDDDDDDDAAAAAA........
```

---

### 結論 2：dummy allocation 可用來 drain partial slab

Phase 2 顯示，使用 dummy objects 可以填滿既有 partial slab 的 free slots。

```text
VVVVVVVVVVDDDDDDDDAAAAAADDDDDDDD
```

這一步的作用是消耗舊洞，讓下一波 allocation 不再落入混雜 slab。

---

### 結論 3：target spray 可產生 all-attacker-controlled slab

Phase 3 顯示，在舊 partial slab 被填滿後，新的 attacker objects 會落入新 slab page。

```text
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
```

這是 allocator massaging 的核心目標。

---

### 結論 4：page-level leak 可轉換成 object-level candidates

若 TLB side channel 洩漏 all-A slab 的 page base，則可根據 slot size 推導：

```text
object_addr = slab_base + slot_size × n
```

在本實驗中：

```text
object_addr = slab_base + 128 × n, n = 0..31
```

因為整頁都是 attacker-controlled objects，攻擊者不需要知道每個 slot 對應哪一個具體 object；知道整頁都是可控 object 已經足以支撐後續 exploit 設計。

---

## 11. 實驗限制

本實驗是簡化模型，不等同於真實 Linux SLUB allocator。

真實 kernel allocator 還包含：

```text
per-CPU freelist
partial slab list
NUMA node
freelist randomization
cache alignment
dedicated cache / generic kmalloc cache
GFP flags
kmalloc-cg accounting
background kernel activity
```

因此，本實驗不能用來預測真實 kernel 的精確 allocation 結果。它的用途是展示 allocator massaging 的核心概念：

```text
填滿舊洞 → 逼出新 slab → 用同類 object 填滿新 slab → page base leak 後推導 slot candidates
```

---

## 12. 可放入報告的一句話總結

```text
實驗 4 以 C 語言模擬 allocator massaging：先用 dummy allocations 填滿既有 partial slab，再 spray attacker-controlled objects，使新的 slab page 完整由同類 objects 佔滿。這說明在 TLB side channel 洩漏 page-aligned address 後，攻擊者可以利用 slab slot layout 推導頁內所有 object candidate addresses，從而提高 exploit 的穩定性與可預測性。
```
