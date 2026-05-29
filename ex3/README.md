# 實驗 3：Pipe Spray 與 Slab Allocator Lifecycle 觀察

## 1. 實驗目的

本實驗的目的不是重現 kernel exploit，也不是執行 privilege escalation，而是在觀察一個真實 user-space allocation primitive 對 Linux kernel slab allocator 的影響。

本實驗觀察三個階段：

```text
before  →  hold  →  after
建立 pipe 前  pipe 持有中  pipe 關閉後
```

要回答的問題是：

1. 大量建立 pipes 時，哪些 slab cache 的 `objects` / `slabs` 會增加？
2. pipes 關閉後，相關 cache 是否會回落？
3. allocator 狀態是否會完全恢復到 before，或留下可被後續 allocation 重用的痕跡？

這個實驗用來支撐 allocator massaging 的基本前提：攻擊者雖然不能直接控制 kernel heap layout，但可以透過大量 user-space syscall / object creation 對 kernel allocator 施加壓力，改變 slab cache 狀態。

---

## 2. 實驗環境

VM 環境：VirtualBox Ubuntu VM。

Kernel 與重要 config：

```text
Linux ubuntu 6.17.0-29-generic #29~24.04.1-Ubuntu SMP PREEMPT_DYNAMIC Mon May 11 10:30:58 UTC 2 x86_64 x86_64 x86_64 GNU/Linux
CONFIG_SLUB=y
# CONFIG_SLUB_TINY is not set
# CONFIG_SLUB_STATS is not set
CONFIG_SLUB_CPU_PARTIAL=y
CONFIG_SLUB_DEBUG=y
# CONFIG_SLUB_DEBUG_ON is not set
CONFIG_STRICT_MODULE_RWX=y
CONFIG_VMAP_STACK=y
CONFIG_RANDOMIZE_BASE=y
```

重點解讀：

```text
CONFIG_SLUB=y                  使用 SLUB allocator
CONFIG_SLUB_CPU_PARTIAL=y      啟用 per-CPU partial slab 行為
CONFIG_SLUB_DEBUG=y            kernel 支援 SLUB debug 功能
CONFIG_STRICT_MODULE_RWX=y     論文中的 D1 defense 存在
CONFIG_VMAP_STACK=y            論文中的 D3 defense 存在
CONFIG_RANDOMIZE_BASE=y        KASLR 啟用
```

這台 VM 適合做 allocator / side-channel 背景實驗。由於是 Ubuntu 6.17 generic kernel，結果不一定與論文測試的 v5.15、v6.5、v6.6、v6.8 完全一致，但足以觀察 allocator lifecycle 現象。

---

## 3. 實驗與論文攻擊鏈的關係

論文的完整攻擊鏈中，allocator massaging 是 location disclosure attack 的前置條件。論文指出，單純的 syscall 或 access primitive 會接觸大量 kernel objects，因此需要先透過 allocator massaging 讓目標 object 所在頁面具有較可預測的 layout，再用 TLB side channel 洩漏 page-level location。

本實驗只做其中一個安全子問題：

```text
user-space allocation primitive 是否能改變 kernel slab allocator 狀態？
```

本實驗沒有做：

```text
1. kernel address leak
2. TLB prefetch scan
3. UAF / OOB exploit
4. 任意讀寫
5. cred patching / privilege escalation
```

本實驗只是觀察 `pipe()` 造成的 kernel allocation lifecycle。

---

## 4. 實驗檔案

本實驗使用三個程式 / 腳本。

### 4.1 `pipe_spray_hold.c`

用途：建立大量 pipes，保持一段時間，讓觀察者可以在 pipes 還活著時讀取 `/sys/kernel/slab` 狀態。

編譯：

```bash
gcc -Wall -Wextra -O2 pipe_spray_hold.c -o pipe_spray_hold
```

---

### 4.2 `read_slab_snapshot.sh`

用途：讀取所有 `kmalloc-*` / `kmalloc-cg-*` slab cache 的主要欄位，輸出成 snapshot 檔案。

建議整個腳本用 `sudo` 執行，不要在迴圈內對每個欄位個別 `sudo`。

執行：

```bash
chmod +x read_slab_snapshot.sh
sudo ./read_slab_snapshot.sh before.txt
```

---

### 4.3 `diff_slab_snapshots.c`

用途：比較三個 snapshot：`before.txt`、`hold.txt`、`after.txt`。

它會列出：

```text
before → hold
hold   → after
before → after
```

每個 cache 的變化包含：

```text
objects delta
slabs delta
partial delta
```

---

## 5. 實驗流程

### 5.1 切換工作目錄

```bash
cd ~/ex5
```

### 5.2 編譯程式

```bash
gcc -Wall -Wextra -O2 pipe_spray_hold.c -o pipe_spray_hold
gcc -Wall -Wextra -O2 diff_slab_snapshots.c -o diff_slab_snapshots
```

### 5.3 提高 file descriptor limit

建立 20000 pipes 會開約 40000 個 file descriptors，因此需要提高 limit：

```bash
ulimit -n 100000
ulimit -n
```

### 5.4 記錄 before

```bash
sudo ./read_slab_snapshot.sh before.txt
```

### 5.5 建立並 hold 20000 pipes

開另一個 terminal：

```bash
cd ~/kernel-lab/exp5-pipe-spray
ulimit -n 100000
./pipe_spray_hold 20000 300
```

程式會輸出類似：

```text
created pipes: 20000
open file descriptors: approximately 40000
pid: <pid>
holding for 300 seconds; observe /sys/kernel/slab in another terminal
```

### 5.6 在 hold 期間記錄 hold

回到第一個 terminal：

```bash
sudo ./read_slab_snapshot.sh hold.txt
```

### 5.7 pipes 關閉後記錄 after

等 `pipe_spray_hold` 結束後：

```bash
sudo ./read_slab_snapshot.sh after.txt
```

### 5.8 比較三個 snapshot

```bash
./diff_slab_snapshots before.txt hold.txt after.txt
```

---

## 6. 實驗結果

### 6.1 before → hold

`before → hold` 代表 pipes 還活著時 allocator 的變化。

變化最大的幾個 cache：

| cache | size | objs_per_slab | order | objects before | objects hold | Δ objects | slabs before | slabs hold | Δ slabs | Δ partial |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| kmalloc-rnd-05-512 | 512 | 16 | 1 | 1244 | 2110 | +866 | 80 | 132 | +52 | -11 |
| kmalloc-rnd-05-192 | 192 | 21 | 0 | 4668 | 5523 | +855 | 237 | 263 | +26 | -60 |
| kmalloc-rnd-05-128 | 128 | 32 | 0 | 1408 | 2144 | +736 | 44 | 67 | +23 | 0 |
| kmalloc-cg-1k | 1024 | 16 | 2 | 356 | 848 | +492 | 28 | 53 | +25 | -15 |
| kmalloc-cg-192 | 192 | 21 | 0 | 507 | 756 | +249 | 31 | 36 | +5 | -8 |
| kmalloc-rnd-03-32 | 32 | 128 | 0 | 896 | 1122 | +226 | 7 | 9 | +2 | +2 |
| kmalloc-rcl-96 | 96 | 42 | 0 | 547 | 714 | +167 | 14 | 17 | +3 | -6 |
| kmalloc-rcl-128 | 128 | 32 | 0 | 320 | 480 | +160 | 10 | 15 | +5 | 0 |

解讀：

```text
建立 20000 pipes 期間，多個 kmalloc cache 的 objects/slabs 明顯增加。
這表示 pipe() 不是只配置單一 kernel object，而是造成多種 kernel allocations。
```

最明顯的 cache 是 `kmalloc-rnd-05-512`、`kmalloc-rnd-05-192`、`kmalloc-rnd-05-128`、`kmalloc-cg-1k`。

---

### 6.2 hold → after

`hold → after` 代表 pipes 關閉後 allocator 的變化。

主要回落的 cache：

| cache | size | objs_per_slab | order | objects hold | objects after | Δ objects | slabs hold | slabs after | Δ slabs | Δ partial |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| kmalloc-cg-192 | 192 | 21 | 0 | 756 | 323 | -433 | 36 | 24 | -12 | +12 |
| kmalloc-cg-1k | 1024 | 16 | 2 | 848 | 364 | -484 | 53 | 35 | -18 | +23 |
| kmalloc-rnd-05-192 | 192 | 21 | 0 | 5523 | 5459 | -64 | 263 | 263 | 0 | +12 |
| kmalloc-rnd-11-96 | 96 | 42 | 0 | 507 | 447 | -60 | 20 | 18 | -2 | -1 |

解讀：

```text
pipes 關閉後，部分 cache 的 objects/slabs 下降，表示部分 kernel objects 被釋放。
但不是所有 cache 都回落，也不是所有 slabs 都被歸還。
```

特別是 `kmalloc-rnd-05-192`：

```text
objects 下降 64
slabs 不變
```

這表示 object 被釋放後，allocator 可能仍保留 slab capacity，等待後續 allocation 重用。

---

### 6.3 before → after

`before → after` 最能觀察 allocator 是否完全恢復。

部分仍然維持高水位的 cache：

| cache | size | objs_per_slab | order | objects before | objects after | Δ objects | slabs before | slabs after | Δ slabs | Δ partial |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| kmalloc-rnd-05-512 | 512 | 16 | 1 | 1244 | 2110 | +866 | 80 | 132 | +52 | -11 |
| kmalloc-rnd-05-192 | 192 | 21 | 0 | 4668 | 5459 | +791 | 237 | 263 | +26 | -48 |
| kmalloc-rnd-05-128 | 128 | 32 | 0 | 1408 | 2144 | +736 | 44 | 67 | +23 | 0 |
| kmalloc-cg-16 | 16 | 256 | 0 | 1517 | 1855 | +338 | 6 | 8 | +2 | 0 |
| kmalloc-rnd-03-32 | 32 | 128 | 0 | 896 | 1122 | +226 | 7 | 9 | +2 | +2 |
| kmalloc-rcl-96 | 96 | 42 | 0 | 547 | 714 | +167 | 14 | 17 | +3 | -6 |
| kmalloc-rcl-128 | 128 | 32 | 0 | 320 | 480 | +160 | 10 | 15 | +5 | 0 |

解讀：

```text
pipe_spray_hold 結束後，部分 cache 仍然比 before 有更高的 objects/slabs。
這表示 allocator 狀態沒有完全恢復到初始狀態。
```

---

## 7. 對 `kmalloc-rnd-*` 的觀察

本次 VM 結果中出現大量 `kmalloc-rnd-*` cache，例如：

```text
kmalloc-rnd-05-128
kmalloc-rnd-05-192
kmalloc-rnd-05-512
```

這表示此 kernel 對 kmalloc cache 做了 randomized cache 分流或類似機制。其效果是：allocation site 不一定落在傳統單一 `kmalloc-128` / `kmalloc-256` / `kmalloc-512`，而可能被分散到多個 randomized kmalloc cache。

對 exploit 場景的含義：

```text
1. allocator layout 的不確定性增加
2. 傳統只看 kmalloc-128/256/512 的觀察方式不夠
3. 需要觀察所有 kmalloc-* / kmalloc-cg-* / kmalloc-rnd-* cache
4. 但大量 pipe spray 仍然會造成明顯且可觀察的 slab 狀態變化
```

因此，本實驗不能說「某個 kmalloc-rnd-* cache 一定是 pipe_buffer」，只能說：

```text
pipe_spray_hold 造成多個 kmalloc cache 顯著變化，說明 pipe 建立涉及多種 kernel allocation。
```

若要精準定位 allocation site，需要更進一步使用 kernel debug symbols、ftrace、kprobe、BPF tracing，或閱讀 kernel source 對照 allocation path。

---

## 8. 實驗結論

### 結論 1：pipe allocation 會明顯改變 kernel slab 狀態

建立 20000 pipes 後，多個 slab cache 的 `objects` / `slabs` 明顯增加。其中變化最明顯的是：

```text
kmalloc-rnd-05-512: objects +866, slabs +52
kmalloc-rnd-05-192: objects +855, slabs +26
kmalloc-rnd-05-128: objects +736, slabs +23
kmalloc-cg-1k:      objects +492, slabs +25
```

這證明 user-space 的 `pipe()` 行為會觸發大量 kernel allocations。

### 結論 2：pipe 關閉後 allocator 不一定完全回到 before

`hold → after` 顯示部分 cache 會回落，例如 `kmalloc-cg-1k` 和 `kmalloc-cg-192`；但 `before → after` 顯示多個 cache 仍維持比初始狀態更高的 objects/slabs。

這說明 kernel heap allocator 不是「close fd 後立刻完全歸零」的模型，而是存在 cache 保留、延遲回收、partial slab 與重用行為。

### 結論 3：這支撐 allocator massaging 的基本前提

Allocator massaging 的核心不是直接指定某個 kernel address，而是利用大量 allocation/free 操作改變 allocator 狀態，讓後續目標 object 更可能落入攻擊者預期的 slab layout。

本實驗觀察到：

```text
before → hold:  大量 allocation 造成 cache 增長
hold → after:   部分 object/slab 回落
before → after: allocator 狀態不完全恢復
```

這正是 memory reuse 與 allocator massaging 能成立的現象基礎。

### 結論 4：本實驗仍不是 exploit

本實驗沒有洩漏 kernel address，也沒有利用 kernel vulnerability。它只是觀察 allocator lifecycle。

本實驗可以作為後續安全實驗的基礎，例如：

```text
1. 重複性測試：連續跑多次 before/hold/after，看變化是否穩定
2. ftrace / kprobe / BPF tracing：追蹤 pipe 相關 allocation site
3. user-space common/differential pattern 模擬：不掃 kernel address，只驗證 pattern extraction
```

---
