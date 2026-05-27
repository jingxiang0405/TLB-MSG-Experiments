#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 4096
#define SLOT_SIZE 128
#define SLOTS_PER_SLAB (PAGE_SIZE / SLOT_SIZE)
#define MAX_SLABS 16

typedef struct {
    uint64_t base;
    char slots[SLOTS_PER_SLAB];   // '.', 'V', 'D', 'A'
} Slab;

typedef struct {
    Slab slabs[MAX_SLABS];
    int slab_count;
    uint64_t next_base;
} Allocator;

static void init_slab(Slab *s, uint64_t base) {
    s->base = base;
    for (int i = 0; i < SLOTS_PER_SLAB; i++) {
        s->slots[i] = '.';
    }
}

static void init_allocator(Allocator *a, uint64_t start_base) {
    a->slab_count = 0;
    a->next_base = start_base;
}

static Slab *new_slab(Allocator *a) {
    if (a->slab_count >= MAX_SLABS) {
        fprintf(stderr, "error: MAX_SLABS reached\n");
        exit(1);
    }

    Slab *s = &a->slabs[a->slab_count++];
    init_slab(s, a->next_base);
    a->next_base += PAGE_SIZE;
    return s;
}

static int has_free_slot(const Slab *s) {
    for (int i = 0; i < SLOTS_PER_SLAB; i++) {
        if (s->slots[i] == '.') {
            return 1;
        }
    }
    return 0;
}

static int alloc_object(Allocator *a, char obj, int *out_slab_id, int *out_slot, uint64_t *out_addr) {
    for (int sid = 0; sid < a->slab_count; sid++) {
        Slab *s = &a->slabs[sid];

        for (int slot = 0; slot < SLOTS_PER_SLAB; slot++) {
            if (s->slots[slot] == '.') {
                s->slots[slot] = obj;

                if (out_slab_id) *out_slab_id = sid;
                if (out_slot) *out_slot = slot;
                if (out_addr) *out_addr = s->base + (uint64_t)slot * SLOT_SIZE;

                return 0;
            }
        }
    }

    Slab *s = new_slab(a);
    s->slots[0] = obj;

    if (out_slab_id) *out_slab_id = a->slab_count - 1;
    if (out_slot) *out_slot = 0;
    if (out_addr) *out_addr = s->base;

    return 0;
}

static int is_full_of(const Slab *s, char obj) {
    for (int i = 0; i < SLOTS_PER_SLAB; i++) {
        if (s->slots[i] != obj) {
            return 0;
        }
    }
    return 1;
}

static void print_counts(const Slab *s) {
    int free_count = 0;
    int a_count = 0;
    int d_count = 0;
    int v_count = 0;

    for (int i = 0; i < SLOTS_PER_SLAB; i++) {
        switch (s->slots[i]) {
            case '.': free_count++; break;
            case 'A': a_count++; break;
            case 'D': d_count++; break;
            case 'V': v_count++; break;
            default: break;
        }
    }

    printf(".:%d A:%d D:%d V:%d", free_count, a_count, d_count, v_count);
}

static void dump_allocator(const Allocator *a, const char *title) {
    printf("\n");
    printf("================================================================================\n");
    printf("%s\n", title);
    printf("================================================================================\n");

    for (int sid = 0; sid < a->slab_count; sid++) {
        const Slab *s = &a->slabs[sid];

        printf("slab %02d base=0x%016llx  ",
               sid,
               (unsigned long long)s->base);

        for (int i = 0; i < SLOTS_PER_SLAB; i++) {
            putchar(s->slots[i]);
        }

        printf("  ");
        print_counts(s);
        printf("\n");
    }
}

static void print_candidate_addresses(const Slab *s) {
    printf("\n");
    printf("Assume TLB side channel leaks slab_base = 0x%016llx\n",
           (unsigned long long)s->base);
    printf("slot_size = %d bytes\n", SLOT_SIZE);
    printf("formula: object_addr = slab_base + slot_size * n\n");
    printf("\n");

    for (int n = 0; n < SLOTS_PER_SLAB; n++) {
        uint64_t addr = s->base + (uint64_t)n * SLOT_SIZE;
        printf("slot %02d: 0x%016llx\n",
               n,
               (unsigned long long)addr);
    }
}

int main(void) {
    Allocator allocator;
    init_allocator(&allocator, 0xffff8ae0f1200000ULL);

    /*
     * Phase 1:
     * 模擬初始狀態：slab 裡已有不同種類的 kernel objects。
     *
     * V = victim / unrelated object
     * D = dummy object
     * A = attacker-controlled object
     * . = free slot
     */
    for (int i = 0; i < 10; i++) {
        alloc_object(&allocator, 'V', NULL, NULL, NULL);
    }

    for (int i = 0; i < 8; i++) {
        alloc_object(&allocator, 'D', NULL, NULL, NULL);
    }

    for (int i = 0; i < 6; i++) {
        alloc_object(&allocator, 'A', NULL, NULL, NULL);
    }

    dump_allocator(&allocator, "Phase 1: initial mixed slab state");

    /*
     * Phase 2:
     * drain / padding。
     *
     * 用 dummy object 把目前 partial slab 的剩餘 free slots 填滿。
     * 這一頁不是攻擊目標；它的用途是消耗 allocator 既有空位，
     * 讓下一波 attacker spray 從新的 slab page 開始。
     */
    if (allocator.slab_count == 0) {
        fprintf(stderr, "unexpected: no slab allocated\n");
        return 1;
    }

    while (has_free_slot(&allocator.slabs[allocator.slab_count - 1])) {
        alloc_object(&allocator, 'D', NULL, NULL, NULL);
    }

    dump_allocator(&allocator, "Phase 2: drain current partial slab with dummy objects");

    /*
     * Phase 3:
     * target spray。
     *
     * 配置剛好一整個 slab page 的 attacker-controlled objects。
     * 因為上一階段已填滿舊 partial slab，這些 A 會落到新的 slab。
     */
    for (int i = 0; i < SLOTS_PER_SLAB; i++) {
        alloc_object(&allocator, 'A', NULL, NULL, NULL);
    }

    dump_allocator(&allocator, "Phase 3: target spray creates an all-A slab");

    printf("\n");
    printf("================================================================================\n");
    printf("Result\n");
    printf("================================================================================\n");

    int found = 0;
    const Slab *chosen = NULL;

    for (int sid = 0; sid < allocator.slab_count; sid++) {
        const Slab *s = &allocator.slabs[sid];

        if (is_full_of(s, 'A')) {
            printf("Found full attacker-controlled slab: slab %02d, base=0x%016llx\n",
                   sid,
                   (unsigned long long)s->base);

            if (!chosen) {
                chosen = s;
            }

            found = 1;
        }
    }

    if (!found) {
        printf("No full attacker-controlled slab found.\n");
        return 0;
    }

    print_candidate_addresses(chosen);

    return 0;
}
