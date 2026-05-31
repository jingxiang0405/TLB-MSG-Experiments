#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

#define MSG_PAYLOAD  72
#define ALLOC_COUNT  64    /* 2 個 slab page 的量（32×2）*/

struct msgbuf { long mtype; char mtext[MSG_PAYLOAD]; };

long read_slabinfo(const char *cache_name) {
    FILE *f = fopen("/proc/slabinfo", "r");
    if (!f) return -1;
    char line[512];
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);
    while (fgets(line, sizeof(line), f)) {
        char name[128];
        long active_objs, num_objs, objsize, objperslab, pagesperslab;
        if (sscanf(line, "%127s %ld %ld %ld %ld %ld",
                   name, &active_objs, &num_objs,
                   &objsize, &objperslab, &pagesperslab) >= 5)
            if (strcmp(name, cache_name) == 0) {
                fclose(f);
                return active_objs;
            }
    }
    fclose(f);
    return -1;
}

long read_slab_attr(const char *cache_name, const char *field) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/kernel/slab/%s/%s", cache_name, field);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long val = -1;
    fscanf(f, "%ld", &val);
    fclose(f);
    return val;
}

int main() {
    struct msgbuf buf = {.mtype = 1};
    memset(buf.mtext, 'A', MSG_PAYLOAD);

    const char *cache_name = "kmalloc-cg-128";

    // 步驟 1：讀取靜態結構
    long objsize  = read_slab_attr(cache_name, "object_size");
    long objs_per = read_slab_attr(cache_name, "objs_per_slab");
    long order    = read_slab_attr(cache_name, "order");
    long align    = read_slab_attr(cache_name, "align");

    printf("object_size   = %ld bytes\n", objsize);
    printf("objs_per_slab = %ld 個\n",   objs_per);
    printf("order         = %ld → slab page 大小 = %ldkB\n",
           order, (1L << order) * 4);
    printf("align         = %ld bytes\n\n", align);

    // 步驟 2：分配前後對比
    long before = read_slabinfo(cache_name);
    printf("分配前 active_objs = %ld\n", before);

    int qids[ALLOC_COUNT];
    for (int i = 0; i < ALLOC_COUNT; i++) {
        qids[i] = msgget(IPC_PRIVATE, 0666 | IPC_CREAT);
        msgsnd(qids[i], &buf, MSG_PAYLOAD, 0);
    }

    long after = read_slabinfo(cache_name);
    printf("分配後 active_objs = %ld\n", after);
    printf("增加了 %ld 個物件\n\n", after - before);

    // 步驟 3：釋放後觀察
    for (int i = 0; i < ALLOC_COUNT; i++)
        msgctl(qids[i], IPC_RMID, NULL);

    long freed = read_slabinfo(cache_name);
    printf("釋放後 active_objs = %ld\n", freed);

    return 0;
}