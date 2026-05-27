#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 4096
#define REPEAT    500

// rdtscp 會等到前面所有指令都完成才讀時間
static inline uint64_t rdtscp() {
    uint32_t lo, hi;
    __asm__ __volatile__(
        "lfence\n\t"      // 等前面所有load完成
        "rdtsc\n\t"
        "lfence"          // 等rdtsc本身完成
        : "=a"(lo), "=d"(hi) :: "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

static inline void clflush(void *addr) {
    __asm__ __volatile__("clflush (%0)" :: "r"(addr) : "memory");
}

static char *evict_pages[NUM_PAGES];

void shuffle(char **arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char *tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

int main() {
    srand(42);

    char *target = (char *)aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    char *evict_pool = (char *)aligned_alloc(PAGE_SIZE,
                                             NUM_PAGES * PAGE_SIZE);
    memset(target, 1, PAGE_SIZE);
    memset(evict_pool, 1, NUM_PAGES * PAGE_SIZE);

    // 建立隨機順序的驅逐頁面指標
    for (int i = 0; i < NUM_PAGES; i++)
        evict_pages[i] = evict_pool + i * PAGE_SIZE;

    uint64_t hit_total = 0, miss_total = 0;

    for (int i = 0; i < REPEAT; i++) {

        // === 測量 TLB Hit ===
        volatile char x = target[0];   // 先存取，讓翻譯進 TLB

        uint64_t t1 = rdtscp();
        x = target[0];
        uint64_t t2 = rdtscp();
        hit_total += (t2 - t1);

        // === 驅逐 TLB ===
        shuffle(evict_pages, NUM_PAGES);  // 每次隨機順序
        for (int j = 0; j < NUM_PAGES; j++) {
            volatile char y = *evict_pages[j];
            (void)y;
        }

        // 把 target 的資料和 page table 都趕出 cache
        clflush(target);

        // === 測量 TLB Miss ===
        uint64_t t3 = rdtscp();
        x = target[0];                   // 現在要重新走 Page Table
        uint64_t t4 = rdtscp();
        miss_total += (t4 - t3);
    }

    uint64_t hit_avg  = hit_total  / REPEAT;
    uint64_t miss_avg = miss_total / REPEAT;

    printf("=== TLB Hit vs Miss ===\n");
    printf("平均 TLB Hit  時間：%4lu cycles\n", hit_avg);
    printf("平均 TLB Miss 時間：%4lu cycles\n", miss_avg);
    printf("倍數差距：%.1fx\n\n", (double)miss_avg / (double)hit_avg);

    free(target);
    free(evict_pool);
    return 0;
}