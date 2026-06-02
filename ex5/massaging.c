#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>

#define MSG_PAYLOAD     72
#define DRAIN_COUNT   2016    // 加大排空數量
#define TARGET_COUNT   300
#define OBJS_PER_SLAB   32    // kmalloc-cg-128: 4096÷128=32

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

typedef struct { int qid; uint64_t time; int group; } Alloc;

// 用中位數的 N 倍當閾值，比最大間隔法更穩定
uint64_t median_threshold(uint64_t *times, int n, double factor) {
    uint64_t sorted[TARGET_COUNT];
    memcpy(sorted, times, n * sizeof(uint64_t));
    for (int i = 0; i < n-1; i++)
        for (int j = 0; j < n-i-1; j++)
            if (sorted[j] > sorted[j+1]) {
                uint64_t t = sorted[j];
                sorted[j] = sorted[j+1];
                sorted[j+1] = t;
            }
    uint64_t median = sorted[n/2];
    return (uint64_t)(median * factor);
}

int main() {
    struct msgbuf buf = {.mtype = 1};
    memset(buf.mtext, 'A', MSG_PAYLOAD);

    int   drain_qids[DRAIN_COUNT];
    Alloc target[TARGET_COUNT];
    uint64_t raw_times[TARGET_COUNT];

    /* 步驟 1：Drain */
    printf("=== 實驗 5：Allocator Massaging ===\n\n");
    printf("步驟 1：排空 cache（分配 %d 個假 msg_msg）...\n", DRAIN_COUNT);
    for (int i = 0; i < DRAIN_COUNT; i++) {
        drain_qids[i] = msgget(IPC_PRIVATE, 0666|IPC_CREAT);
        msgsnd(drain_qids[i], &buf, MSG_PAYLOAD, 0);
    }
    printf("  完成。\n\n");

    /* 步驟 2：分配目標物件 + 計時 */
    printf("步驟 2：分配 %d 個目標 msg_msg 並計時...\n", TARGET_COUNT);
    for (int i = 0; i < TARGET_COUNT; i++) {
        target[i].qid = msgget(IPC_PRIVATE, 0666|IPC_CREAT);
        uint64_t t1 = rdtsc();
        msgsnd(target[i].qid, &buf, MSG_PAYLOAD, 0);
        uint64_t t2 = rdtsc();
        target[i].time = t2 - t1;
        raw_times[i]   = t2 - t1;
        target[i].group = 0;
    }
    printf("  完成。\n\n");

    /* 步驟 3：印出時間分布，讓使用者確認閾值 */
    printf("步驟 3：時間分布（前 60 次）\n\n");
    printf("  編號  時間(cycles)  視覺化\n");
    printf("  ----  ------------  ------\n");

    uint64_t threshold = median_threshold(raw_times, TARGET_COUNT, 2.0);

    for (int i = 0; i < 60; i++) {
        const char *bar = (target[i].time >= threshold) ? "▓▓▓ << 慢（新 slab page）"
                                                        : "░";
        printf("  %-4d  %-12lu  %s\n", i, target[i].time, bar);
    }

    printf("\n  自動閾值（中位數 × 2）：%lu cycles\n\n", threshold);

    /* 步驟 4：分組 */
    printf("步驟 4：依慢分配事件分組\n\n");
    int current_group = 1;
    int slow_count = 0;
    for (int i = 0; i < TARGET_COUNT; i++) {
        if (target[i].time >= threshold) {
            if (i > 0) current_group++;
            slow_count++;
        }
        target[i].group = current_group;
    }
    printf("  偵測到 %d 個「慢分配」事件（新 slab page）\n", slow_count);
    printf("  分成 %d 個 group\n\n", current_group);

    /* 步驟 5：統計每個 group 的物件數 */
    printf("步驟 5：各 slab page 物件數量\n");
    printf("  （理論值：每頁 %d 個物件）\n\n", OBJS_PER_SLAB);
    printf("  %-10s %-8s %-30s %s\n", "Slab page", "物件數", "視覺化", "狀態");
    printf("  %-10s %-8s %-30s %s\n", "---------", "------", "------", "----");

    for (int g = 1; g <= current_group && g <= 12; g++) {
        int cnt = 0;
        for (int i = 0; i < TARGET_COUNT; i++)
            if (target[i].group == g) cnt++;

        char bar[64] = {0};
        int barlen = cnt > 40 ? 40 : cnt;
        for (int k = 0; k < barlen; k++) bar[k] = 0xE2, bar[k]=(char)0xe2, bar[k+1]=(char)0x96, bar[k+2]=(char)0x88, k+=2;

        const char *status = (cnt == OBJS_PER_SLAB) ? "✓ 滿" :
                             (cnt  < OBJS_PER_SLAB) ? "（部分）" : "（跨頁？）";

        // 簡單 bar
        char simple[50]={0};
        int b = cnt < 48 ? cnt : 48;
        memset(simple, '#', b);
        printf("  %-10d %-8d %-30s %s\n", g, cnt, simple, status);
    }
    if (current_group > 12) printf("  ...（只顯示前 12 個）\n");

    /* 清理 */
    for (int i = 0; i < DRAIN_COUNT; i++) msgctl(drain_qids[i], IPC_RMID, NULL);
    for (int i = 0; i < TARGET_COUNT; i++) msgctl(target[i].qid, IPC_RMID, NULL);

    return 0;
}
