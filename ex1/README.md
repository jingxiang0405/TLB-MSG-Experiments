# 實驗 1：TLB Hit vs Miss 時間差

## 目的
直接量測 TLB Hit 與 TLB Miss 的時間差。

## 原理

CPU 每次存取記憶體都需要把虛擬位址轉換成實體位址。TLB 快取了最近用過的翻譯結果：

```
TLB Hit：翻譯結果在 TLB 裡 → 直接拿 → 快（~30-80 cycles）
TLB Miss：翻譯結果不在 TLB → 走 Page Table → 慢（~150-500 cycles）
```

這個時間差是論文所有 TLB 側通道攻擊的物理基礎。攻擊者透過量測時間，推測某個位址是否被 kernel 存取過。

## 踩過的坑與修正

### 坑 1：CPU 亂序執行（Out-of-Order Execution）

**問題：** CPU 會重新排列指令順序，導致 `rdtsc` 計時不準。

```c
// 我們寫的             // CPU 可能實際做的
t1 = rdtsc()           x = target[0]  ← 提前執行
x  = target[0]         t1 = rdtsc()
t2 = rdtsc()           t2 = rdtsc()
                       // 結果 t2 - t1 ≈ 0
```

**修正：** 在 `rdtsc` 前後加 `lfence`，強制 CPU 等前面所有指令完成。

```c
lfence
rdtsc       // 現在才讀時間
lfence
```

---

### 坑 2：Hardware Prefetcher 干擾

**問題：** CPU 偵測到規律的循序存取，自動預取下一頁回 TLB，導致我們「製造的 Miss」被悄悄修復。

```
循序驅逐：page[0], page[1], page[2]...
CPU：「我看出規律了！」→ 偷偷預取 target 回 TLB
結果：target 根本沒被驅逐出去
```

**修正：** 每次用隨機順序驅逐，讓 prefetcher 預測不到。

```c
shuffle(evict_pages, N);  // 打亂順序
for (int j = 0; j < N; j++)
    volatile char y = *evict_pages[j];
```

---

### 坑 3：Data Cache 掩蓋了 TLB Miss 的懲罰

**問題：** 即使 TLB Miss，Page Table 可能還在 L2/L3 Cache 裡，導致 Miss 的懲罰被大幅縮小，時間差不明顯。

**修正：** 用 `clflush` 把目標資料趕出 Cache，讓 TLB Miss 時必須真正存取記憶體。

```c
clflush(target);  // 清掉 cache，確保 Miss 懲罰完整呈現
```

## 編譯與執行

```bash
gcc -O0 -o tlb_basic tlb_basic.c
./tlb_basic
```

## 預期結果

```
平均 TLB Miss 時間 / 平均 TLB Hit  時間  在 3 ~ 10 之間
```

## 與論文的關係

| 本實驗 | 論文攻擊 |
|--------|---------|
| 手動 `clflush` + 大量頁面驅逐 TLB | 精準的 Set-targeted TLB Eviction |
| `rdtsc` 直接量時間 | `prefetch` 指令量時間（訊號更乾淨）|
| 量測 user space 記憶體 | 量測 kernel space 記憶體 |

論文用 `prefetch` 而非直接存取，因為 `prefetch` 不會觸發存取違規，即使對 kernel 位址也能計時，這是從 user space 觀察 kernel TLB 狀態的關鍵。
