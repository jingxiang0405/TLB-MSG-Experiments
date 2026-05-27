#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>

#define PAGE_SIZE  4096
#define MAX_PAGES  4096
#define REPEAT     20

static long perf_event_open(struct perf_event_attr *attr,
                             pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int open_dtlb_miss_counter() {
    struct perf_event_attr attr = {0};
    attr.type           = PERF_TYPE_HW_CACHE;
    attr.size           = sizeof(attr);
    attr.config         =
        PERF_COUNT_HW_CACHE_DTLB              |
        (PERF_COUNT_HW_CACHE_OP_READ   << 8)  |
        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    attr.disabled       = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv     = 1;

    int fd = perf_event_open(&attr, 0, -1, -1, 0);
    if (fd < 0) {
        perror("perf_event_open 失敗");
        fprintf(stderr, "請先執行：sudo sysctl kernel.perf_event_paranoid=0\n");
        exit(1);
    }
    return fd;
}

static char *pages[MAX_PAGES];

void shuffle(char **arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        char *tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

static inline void clflush(void *addr) {
    __asm__ __volatile__("clflush (%0)" :: "r"(addr) : "memory");
}

int main() {
    srand(42);

    char *pool = (char *)aligned_alloc(PAGE_SIZE, MAX_PAGES * PAGE_SIZE);
    memset(pool, 1, MAX_PAGES * PAGE_SIZE);
    madvise(pool, MAX_PAGES * PAGE_SIZE, MADV_NOHUGEPAGE);

    int fd = open_dtlb_miss_counter();

    printf("=== TLB 容量實驗（硬體計數器版）===\n\n");
    printf("%-10s %-12s %-12s %-10s %s\n",
           "頁面數N", "TLB Miss數", "總存取數", "Miss率", "狀態");
    printf("%-10s %-12s %-12s %-10s %s\n",
           "-------", "----------", "--------", "------", "----");

    int test_sizes[] = {
        8, 16, 32, 48, 64, 80, 96, 128,
        192, 256, 384, 512, 768,
        1024, 1280, 1536, 2048, 3072, 4096
    };
    int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int t = 0; t < num_tests; t++) {
        int N = test_sizes[t];
        long long total_miss   = 0;
        long long total_access = (long long)N * REPEAT;

        for (int r = 0; r < REPEAT; r++) {
            for (int i = 0; i < N; i++)
                pages[i] = pool + i * PAGE_SIZE;
            shuffle(pages, N);

            for (int i = 0; i < N; i++)
                clflush(pages[i]);
            __asm__ __volatile__("mfence" ::: "memory");

            // 第一次存取：把 N 個頁面的翻譯載入 TLB
            for (int i = 0; i < N; i++) {
                volatile char x = *pages[i]; (void)x;
            }

            shuffle(pages, N);  // 再打亂，防止 prefetcher

            // 開始計數
            ioctl(fd, PERF_EVENT_IOC_RESET,  0);
            ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

            // 第二次存取：量這次的 TLB Miss 數
            for (int i = 0; i < N; i++) {
                volatile char x = *pages[i]; (void)x;
            }

            ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

            long long count = 0;
            read(fd, &count, sizeof(count));
            total_miss += count;
        }

        double miss_rate = (double)total_miss / (double)total_access * 100.0;

        const char *status;
        if      (miss_rate <  5.0) status = "✓ 幾乎全是 Hit（TLB 裝得下）";
        else if (miss_rate < 30.0) status = "△ 開始 Miss（接近容量）";
        else if (miss_rate < 70.0) status = "✗ 大量 Miss（TLB 已滿）";
        else                       status = "✗✗ 幾乎全是 Miss";

        printf("%-10d %-12lld %-12lld %-9.1f%% %s\n",
               N, total_miss, total_access, miss_rate, status);
    }

    close(fd);
    free(pool);

    printf("\n解讀：\n");
    printf("  Miss 率從低突然變高的轉折點 = TLB 的容量\n");
    printf("  第一個轉折 ≈ L1 DTLB 容量（通常 64 條目）\n");
    printf("  第二個轉折 ≈ L2 STLB 容量（通常 1024-2048 條目）\n");
    return 0;
}