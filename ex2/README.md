# 實驗 2：TLB 容量上限

## 目的
找出 TLB 的容量上限，理解「TLB 被填滿時會發生什麼事」，
以及為什麼 Evict 是可行的攻擊手段。

## 原理

TLB 的條目數量有限。當存取的頁面數量超過 TLB 容量，舊的條目會被驅逐：

```
N 個頁面 ≤ TLB 容量 → 第二次存取全是 Hit → Miss 率低
N 個頁面 > TLB 容量 → 有些條目被擠掉   → Miss 率升高
```

找到 Miss 率突然升高的轉折點，就是 TLB 的容量。

Intel 常見數字：
- L1 DTLB（4kB）：64 條目
- L2 STLB（4kB）：1024 或 2048 條目

## 為什麼不用計時，改用硬體計數器

最初嘗試用時間差來偵測 TLB 容量，但結果幾乎看不出差異：

```
N=8    → 35 cycles/頁
N=256  → 17 cycles/頁  （反而更快？）
N=4096 → 31 cycles/頁  （幾乎沒變化）
```

原因：**Page Table 被 L3 Cache 住了**。TLB Miss 後走 Page Table，
但 Page Table 本身在 L3 Cache 裡，懲罰極小，timing 看不出來。

解法：改用 `perf_event_open` syscall，讓 CPU 硬體**直接數 TLB Miss 次數**，
與 Cache 裡有什麼完全無關。

## 其他踩過的坑

### 坑：Transparent Huge Pages（THP）

Ubuntu 預設開啟 THP。第一次執行後，核心把連續 4kB 頁面合併成 2MB 大頁，
導致第二次執行看起來全是 Hit。

**修正：** 用 `madvise` 禁止合併。
```c
madvise(pool, MAX_PAGES * PAGE_SIZE, MADV_NOHUGEPAGE);
```

### 坑：Hardware Prefetcher 掩蓋 L1 轉折點

循序存取讓 CPU 預測並預取，L1 的容量邊界消失。

**修正：** 每次用隨機順序存取。
```c
shuffle(pages, N);
```

## 編譯與執行

```bash
# 允許讀取硬體計數器（重開機後恢復）
sudo sysctl kernel.perf_event_paranoid=0

# 編譯執行
gcc -O0 -o tlb_capacity tlb_capacity.c
./tlb_capacity3
```

## 實際觀測結果

```
頁面數N   TLB Miss數   總存取數    Miss率     狀態
8         1            160         0.6%      ✓ 幾乎全是 Hit
16        0            320         0.0%      ✓ 幾乎全是 Hit
...（中間都是 Hit）
1536      389          30720       1.3%      ✓ 幾乎全是 Hit
2048      2155         40960       5.3%      △ 開始 Miss
3072      36261        61440       59.0%     ✗ 大量 Miss
4096      63863        81920       78.0%     ✗✗ 幾乎全是 Miss
```

**結論：L2 STLB 容量約為 1536～2048 條目**（符合現代 Intel CPU 規格）。

## 為什麼看不到 L1 的轉折點

`dTLB-load-misses` 只在「L1 **且** L2 都 Miss」時才計數：

```
N 超過 L1 容量（64）→ L1 Miss，但 L2 Hit → 不被計數
N 超過 L2 容量（2048）→ L1+L2 都 Miss → 才被計數
```

所以實驗只能量到 L2 的邊界，看不到 L1 的轉折點。

## 與論文的關係

| 本實驗的發現 | 對論文攻擊的意義 |
|-------------|----------------|
| L2 STLB 有約 2048 個條目 | Evict 時需要讓目標被驅逐出 L2 |
| TLB 是有限資源，會驅逐舊條目 | Evict+Reload 攻擊的物理基礎 |
| 超過容量後 Miss 率快速上升 | 少量頁面就能針對特定 Set 完成驅逐 |

論文用「精準 Set-targeted Eviction」，只需存取 8 個（= Way 數）
落在同一個 Set 的頁面，就能驅逐目標，比暴力清空整個 TLB 有效率得多。