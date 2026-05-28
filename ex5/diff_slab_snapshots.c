#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 1024
#define NAME_LEN 128

typedef struct {
    char name[NAME_LEN];
    long object_size;
    long objs_per_slab;
    long order;
    long objects;
    long slabs;
    long partial;
} Row;

static int read_rows(const char *path, Row *rows, int max_rows) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(1);
    }

    int n = 0;

    while (n < max_rows) {
        Row r;
        int ret = fscanf(
            f,
            "%127s %ld %ld %ld %ld %ld %ld",
            r.name,
            &r.object_size,
            &r.objs_per_slab,
            &r.order,
            &r.objects,
            &r.slabs,
            &r.partial
        );

        if (ret == EOF) break;

        if (ret != 7) {
            fprintf(stderr, "parse error in %s near row %d\n", path, n);
            exit(1);
        }

        rows[n++] = r;
    }

    fclose(f);
    return n;
}

static const Row *find_row(const Row *rows, int n, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(rows[i].name, name) == 0) {
            return &rows[i];
        }
    }
    return NULL;
}

static void diff(const char *a_path, const char *b_path) {
    Row a[MAX_ROWS];
    Row b[MAX_ROWS];

    int an = read_rows(a_path, a, MAX_ROWS);
    int bn = read_rows(b_path, b, MAX_ROWS);

    printf("\n%s -> %s\n", a_path, b_path);
    printf("%-24s %6s %6s %6s %12s %12s %12s %10s %10s %10s\n",
           "cache", "size", "objs", "order",
           "obj_before", "obj_after", "delta_obj",
           "slab_b", "slab_a", "delta_s");

    for (int i = 0; i < bn; i++) {
        const Row *ra = find_row(a, an, b[i].name);
        const Row *rb = &b[i];

        if (!ra) continue;

        long delta_obj = rb->objects - ra->objects;
        long delta_slab = rb->slabs - ra->slabs;
        long delta_partial = rb->partial - ra->partial;

        if (delta_obj == 0 && delta_slab == 0 && delta_partial == 0) {
            continue;
        }

        printf("%-24s %6ld %6ld %6ld %12ld %12ld %12ld %10ld %10ld %10ld",
               rb->name,
               rb->object_size,
               rb->objs_per_slab,
               rb->order,
               ra->objects,
               rb->objects,
               delta_obj,
               ra->slabs,
               rb->slabs,
               delta_slab);

        printf("  partial_delta=%ld\n", delta_partial);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s before.txt hold.txt after.txt\n", argv[0]);
        return 1;
    }

    diff(argv[1], argv[2]);
    diff(argv[2], argv[3]);
    diff(argv[1], argv[3]);

    return 0;
}
