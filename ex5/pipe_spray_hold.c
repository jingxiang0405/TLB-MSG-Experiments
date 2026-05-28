#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    int r;
    int w;
} PipeFD;

static volatile sig_atomic_t stop = 0;

static void on_signal(int sig) {
    (void)sig;
    stop = 1;
}

static long parse_long(const char *s, const char *name) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0' || v <= 0) {
        fprintf(stderr, "invalid %s: %s\n", name, s);
        exit(1);
    }

    return v;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <num_pipes> <hold_seconds>\n", argv[0]);
        fprintf(stderr, "example: %s 20000 300\n", argv[0]);
        return 1;
    }

    long num_pipes = parse_long(argv[1], "num_pipes");
    long hold_seconds = parse_long(argv[2], "hold_seconds");

    PipeFD *fds = calloc((size_t)num_pipes, sizeof(PipeFD));
    if (!fds) {
        perror("calloc");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    long created = 0;

    for (long i = 0; i < num_pipes; i++) {
        int p[2];

        if (pipe(p) != 0) {
            perror("pipe");
            break;
        }

        fds[i].r = p[0];
        fds[i].w = p[1];
        created++;
    }

    printf("created pipes: %ld\n", created);
    printf("open file descriptors: approximately %ld\n", created * 2);
    printf("pid: %d\n", getpid());
    printf("holding for %ld seconds; observe /sys/kernel/slab in another terminal\n", hold_seconds);
    fflush(stdout);

    for (long t = 0; t < hold_seconds && !stop; t++) {
        sleep(1);
    }

    printf("closing pipes...\n");
    fflush(stdout);

    for (long i = 0; i < created; i++) {
        if (fds[i].r >= 0) close(fds[i].r);
        if (fds[i].w >= 0) close(fds[i].w);
    }

    free(fds);

    printf("done\n");
    return 0;
}
