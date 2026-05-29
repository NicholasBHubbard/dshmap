#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#include "bench_impl.h"
#include "impl_swtab.h"
#include "impl_chained.h"

#define DO_NOT_OPTIMIZE(val) __asm__ volatile("" : "+r"(val) :: "memory")
#define COMPILER_BARRIER()   __asm__ volatile("" ::: "memory")

#define MIN_TOTAL_OPS 200000

#ifndef PERF_IOC_FLAG_GROUP
#define PERF_IOC_FLAG_GROUP (1U << 0)
#endif

/* --- PRNG --- */

static uint64_t rng_state;

static void
rng_seed(uint64_t seed)
{
    rng_state = seed ? seed : 1;
}

static uint64_t
rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* --- Shuffle --- */

static void
shuffle_u64(uint64_t *arr, size_t n)
{
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rng_next() % (i + 1);
        uint64_t tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }
}

/* --- Hash function --- */

static bench_hash_t
bench_hash(const void *entry)
{
    return (bench_hash_t)(uintptr_t)entry;
}

/* --- Timing --- */

static inline uint64_t
time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* --- Perf counters --- */

#define NUM_HW_COUNTERS 4

enum {
    CTR_L1D_MISS = 0,
    CTR_LLC_MISS = 1,
    CTR_INSN     = 2,
    CTR_BR_MISS  = 3,
};

struct perf_group {
    int fd[NUM_HW_COUNTERS];
    bool available;
};

struct read_group {
    uint64_t nr;
    uint64_t values[NUM_HW_COUNTERS];
};

static long
perf_event_open_(struct perf_event_attr *attr, pid_t pid,
                 int cpu, int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static void
perf_group_open(struct perf_group *pg)
{
    for (int i = 0; i < NUM_HW_COUNTERS; i++)
        pg->fd[i] = -1;
    pg->available = false;

    struct {
        uint32_t type;
        uint64_t config;
    } counters[NUM_HW_COUNTERS] = {
        { PERF_TYPE_HW_CACHE, 0x10000 },
        { PERF_TYPE_HW_CACHE, 0x10002 },
        { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
        { PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES },
    };

    for (int i = 0; i < NUM_HW_COUNTERS; i++) {
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.size = sizeof(pe);
        pe.type = counters[i].type;
        pe.config = counters[i].config;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;

        if (i == 0) {
            pe.disabled = 1;
            pe.read_format = PERF_FORMAT_GROUP;
            pg->fd[i] = (int)perf_event_open_(&pe, 0, -1, -1, 0);
        } else {
            pe.disabled = 0;
            pe.read_format = 0;
            pg->fd[i] = (int)perf_event_open_(&pe, 0, -1, pg->fd[0], 0);
        }

        if (pg->fd[i] < 0) {
            for (int j = 0; j < i; j++) {
                if (pg->fd[j] >= 0)
                    close(pg->fd[j]);
                pg->fd[j] = -1;
            }
            return;
        }
    }
    pg->available = true;
}

static void
perf_group_close(struct perf_group *pg)
{
    for (int i = 0; i < NUM_HW_COUNTERS; i++) {
        if (pg->fd[i] >= 0)
            close(pg->fd[i]);
        pg->fd[i] = -1;
    }
    pg->available = false;
}

static void
perf_group_reset(const struct perf_group *pg)
{
    if (pg->available)
        ioctl(pg->fd[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
}

static void
perf_group_enable(const struct perf_group *pg)
{
    if (pg->available)
        ioctl(pg->fd[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

static void
perf_group_disable(const struct perf_group *pg)
{
    if (pg->available)
        ioctl(pg->fd[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

static void
perf_group_read(const struct perf_group *pg, uint64_t out[NUM_HW_COUNTERS])
{
    memset(out, 0, NUM_HW_COUNTERS * sizeof(uint64_t));
    if (!pg->available)
        return;

    struct read_group rg;
    if (read(pg->fd[0], &rg, sizeof(rg)) == (ssize_t)sizeof(rg)) {
        for (int i = 0; i < NUM_HW_COUNTERS && i < (int)rg.nr; i++)
            out[i] = rg.values[i];
    }
}

/* --- Results --- */

struct bench_result {
    const char *name;
    size_t n_ops;
    uint64_t elapsed_ns;
    uint64_t counters[NUM_HW_COUNTERS];
};

static void
print_header(void)
{
    printf("  %-18s %8s %10s %10s %9s %10s\n",
           "Operation", "ns/op", "L1miss/op", "LLCmiss/op", "insn/op", "brmiss/op");
    printf("  %-18s %8s %10s %10s %9s %10s\n",
           "------------------", "--------", "----------", "----------",
           "---------", "----------");
}

static void
print_result(const struct bench_result *r, bool have_perf)
{
    double ns = (double)r->elapsed_ns / (double)r->n_ops;
    if (have_perf) {
        double ops = (double)r->n_ops;
        printf("  %-18s %8.1f %10.2f %10.2f %9.1f %10.2f\n",
               r->name, ns,
               (double)r->counters[CTR_L1D_MISS] / ops,
               (double)r->counters[CTR_LLC_MISS] / ops,
               (double)r->counters[CTR_INSN] / ops,
               (double)r->counters[CTR_BR_MISS] / ops);
    } else {
        printf("  %-18s %8.1f %10s %10s %9s %10s\n",
               r->name, ns, "n/a", "n/a", "n/a", "n/a");
    }
}

/* --- Helpers --- */

static size_t
calc_iters(size_t n)
{
    size_t iters = MIN_TOTAL_OPS / n;
    return iters < 1 ? 1 : iters;
}

static void *
alloc_ctx(const bench_impl *impl)
{
    void *ctx = calloc(1, impl->ctx_size);
    impl->init(ctx, bench_hash);
    return ctx;
}

static void
free_ctx(const bench_impl *impl, void *ctx)
{
    impl->destroy(ctx);
    free(ctx);
}

static void
populate(const bench_impl *impl, void *ctx,
         const uint64_t *keys, size_t n)
{
    if (impl->reserve)
        impl->reserve(ctx, n);
    for (size_t i = 0; i < n; i++)
        impl->insert(ctx, (void *)(uintptr_t)keys[i], keys[i]);
}

/* --- Iteration callback --- */

static void
iter_nop(void *entry, void *arg)
{
    (void)arg;
    DO_NOT_OPTIMIZE(entry);
}

/* --- Benchmarks --- */

static struct bench_result
bench_insert_seq(const bench_impl *impl, size_t n,
                 const uint64_t *keys, struct perf_group *pg)
{
    struct bench_result res = { .name = "insert_seq" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl);
    for (size_t i = 0; i < n; i++)
        impl->insert(ctx, (void *)(uintptr_t)keys[i], keys[i]);
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++)
            impl->insert(ctx, (void *)(uintptr_t)keys[i], keys[i]);

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];

        free_ctx(impl, ctx);
    }

    res.n_ops = n * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));
    return res;
}

static struct bench_result
bench_insert_rnd(const bench_impl *impl, size_t n,
                 const uint64_t *keys_shuffled, struct perf_group *pg)
{
    struct bench_result res = { .name = "insert_rnd" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl);
    for (size_t i = 0; i < n; i++)
        impl->insert(ctx, (void *)(uintptr_t)keys_shuffled[i], keys_shuffled[i]);
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++)
            impl->insert(ctx, (void *)(uintptr_t)keys_shuffled[i],
                         keys_shuffled[i]);

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];

        free_ctx(impl, ctx);
    }

    res.n_ops = n * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));
    return res;
}

static struct bench_result
bench_find_hit(const bench_impl *impl, size_t n, const uint64_t *keys,
               const uint64_t *keys_shuffled, struct perf_group *pg)
{
    struct bench_result res = { .name = "find_hit" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl);
    populate(impl, ctx, keys, n);

    for (size_t i = 0; i < n; i++) {
        void *p = impl->find(ctx, keys_shuffled[i]);
        DO_NOT_OPTIMIZE(p);
    }

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++) {
            void *p = impl->find(ctx, keys_shuffled[i]);
            DO_NOT_OPTIMIZE(p);
        }

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];
    }

    res.n_ops = n * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));

    free_ctx(impl, ctx);
    return res;
}

static struct bench_result
bench_find_miss(const bench_impl *impl, size_t n, const uint64_t *keys,
                const uint64_t *keys_miss, struct perf_group *pg)
{
    struct bench_result res = { .name = "find_miss" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl);
    populate(impl, ctx, keys, n);

    for (size_t i = 0; i < n; i++) {
        void *p = impl->find(ctx, keys_miss[i]);
        DO_NOT_OPTIMIZE(p);
    }

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++) {
            void *p = impl->find(ctx, keys_miss[i]);
            DO_NOT_OPTIMIZE(p);
        }

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];
    }

    res.n_ops = n * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));

    free_ctx(impl, ctx);
    return res;
}

static struct bench_result
bench_remove_all(const bench_impl *impl, size_t n, const uint64_t *keys,
                 const uint64_t *keys_shuffled, struct perf_group *pg)
{
    struct bench_result res = { .name = "remove" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl);
    populate(impl, ctx, keys, n);
    for (size_t i = 0; i < n; i++)
        impl->remove(ctx, (void *)(uintptr_t)keys_shuffled[i],
                     keys_shuffled[i]);
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl);
        populate(impl, ctx, keys, n);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++)
            impl->remove(ctx, (void *)(uintptr_t)keys_shuffled[i],
                         keys_shuffled[i]);

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];

        free_ctx(impl, ctx);
    }

    res.n_ops = n * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));
    return res;
}

static struct bench_result
bench_iterate(const bench_impl *impl, size_t n,
              const uint64_t *keys, struct perf_group *pg)
{
    struct bench_result res = { .name = "iterate" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl);
    populate(impl, ctx, keys, n);

    impl->for_each(ctx, iter_nop, NULL);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        impl->for_each(ctx, iter_nop, NULL);

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];
    }

    res.n_ops = n * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));

    free_ctx(impl, ctx);
    return res;
}

static struct bench_result
bench_mixed(const bench_impl *impl, size_t n,
            const uint64_t *keys, struct perf_group *pg)
{
    struct bench_result res = { .name = "mixed" };
    size_t iters = calc_iters(n);

    size_t n_ops = n;
    size_t initial = n / 2;
    size_t extra = n - initial;

    enum { OP_FIND = 0, OP_INSERT = 1, OP_REMOVE = 2 };

    uint8_t *ops = malloc(n_ops);
    size_t *find_idx = malloc(n_ops * sizeof(size_t));
    uint64_t *extra_keys = malloc(extra * sizeof(uint64_t));

    uint64_t saved_state = rng_state;

    for (size_t i = 0; i < extra; i++)
        extra_keys[i] = rng_next() | 1;

    size_t insert_count = 0;
    size_t remove_count = 0;
    for (size_t i = 0; i < n_ops; i++) {
        uint64_t r = rng_next() % 100;
        if (r < 70) {
            ops[i] = OP_FIND;
            find_idx[i] = rng_next() % initial;
        } else if (r < 90 && insert_count < extra) {
            ops[i] = OP_INSERT;
            find_idx[i] = insert_count++;
        } else if (remove_count < initial) {
            ops[i] = OP_REMOVE;
            find_idx[i] = remove_count++;
        } else {
            ops[i] = OP_FIND;
            find_idx[i] = rng_next() % initial;
        }
    }

    void *ctx = alloc_ctx(impl);
    if (impl->reserve)
        impl->reserve(ctx, initial + extra);
    for (size_t i = 0; i < initial; i++)
        impl->insert(ctx, (void *)(uintptr_t)keys[i], keys[i]);

    for (size_t i = 0; i < n_ops; i++) {
        switch (ops[i]) {
        case OP_FIND: {
            void *p = impl->find(ctx, keys[find_idx[i]]);
            DO_NOT_OPTIMIZE(p);
            break;
        }
        case OP_INSERT:
            impl->insert(ctx, (void *)(uintptr_t)extra_keys[find_idx[i]],
                         extra_keys[find_idx[i]]);
            break;
        case OP_REMOVE:
            impl->remove(ctx, (void *)(uintptr_t)keys[find_idx[i]],
                         keys[find_idx[i]]);
            break;
        }
    }
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl);
        if (impl->reserve)
            impl->reserve(ctx, initial + extra);
        for (size_t i = 0; i < initial; i++)
            impl->insert(ctx, (void *)(uintptr_t)keys[i], keys[i]);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n_ops; i++) {
            switch (ops[i]) {
            case OP_FIND: {
                void *p = impl->find(ctx, keys[find_idx[i]]);
                DO_NOT_OPTIMIZE(p);
                break;
            }
            case OP_INSERT:
                impl->insert(ctx,
                             (void *)(uintptr_t)extra_keys[find_idx[i]],
                             extra_keys[find_idx[i]]);
                break;
            case OP_REMOVE:
                impl->remove(ctx,
                             (void *)(uintptr_t)keys[find_idx[i]],
                             keys[find_idx[i]]);
                break;
            }
        }

        COMPILER_BARRIER();
        uint64_t t1 = time_ns();
        perf_group_disable(pg);

        uint64_t ctr[NUM_HW_COUNTERS];
        perf_group_read(pg, ctr);
        total_ns += t1 - t0;
        for (int c = 0; c < NUM_HW_COUNTERS; c++)
            total_ctr[c] += ctr[c];

        free_ctx(impl, ctx);
    }

    res.n_ops = n_ops * iters;
    res.elapsed_ns = total_ns;
    memcpy(res.counters, total_ctr, sizeof(total_ctr));

    rng_state = saved_state;
    free(ops);
    free(find_idx);
    free(extra_keys);
    return res;
}

/* --- Driver --- */

struct bench_config {
    size_t n;
    const char *label;
};

static void
run_benchmarks(const struct bench_config *cfg,
               const bench_impl **impls, size_t n_impls,
               struct perf_group *pg)
{
    size_t n = cfg->n;
    printf("\nTable size: %s\n", cfg->label);

    rng_seed(0xdeadbeefcafe1234ULL ^ n);

    uint64_t *keys = malloc(n * sizeof(uint64_t));
    uint64_t *keys_shuffled = malloc(n * sizeof(uint64_t));
    uint64_t *keys_miss = malloc(n * sizeof(uint64_t));

    for (size_t i = 0; i < n; i++)
        keys[i] = rng_next() | 1;
    memcpy(keys_shuffled, keys, n * sizeof(uint64_t));
    shuffle_u64(keys_shuffled, n);
    for (size_t i = 0; i < n; i++)
        keys_miss[i] = rng_next() | 1;

    for (size_t impl_i = 0; impl_i < n_impls; impl_i++) {
        const bench_impl *impl = impls[impl_i];
        printf("\n  [%s]\n", impl->name);
        print_header();

        struct bench_result r;

        r = bench_insert_seq(impl, n, keys, pg);
        print_result(&r, pg->available);

        r = bench_insert_rnd(impl, n, keys_shuffled, pg);
        print_result(&r, pg->available);

        r = bench_find_hit(impl, n, keys, keys_shuffled, pg);
        print_result(&r, pg->available);

        r = bench_find_miss(impl, n, keys, keys_miss, pg);
        print_result(&r, pg->available);

        r = bench_remove_all(impl, n, keys, keys_shuffled, pg);
        print_result(&r, pg->available);

        r = bench_iterate(impl, n, keys, pg);
        print_result(&r, pg->available);

        r = bench_mixed(impl, n, keys, pg);
        print_result(&r, pg->available);
    }

    /* comparative memory */
    printf("\n  Memory (%zu entries):\n", n);
    printf("    %-14s", "");
    for (size_t i = 0; i < n_impls; i++)
        printf(" %10s", impls[i]->name);
    printf("\n");
    printf("    %-14s", "--------------");
    for (size_t i = 0; i < n_impls; i++)
        printf(" %10s", "----------");
    printf("\n");

    size_t *mem = malloc(n_impls * sizeof(size_t));
    for (size_t i = 0; i < n_impls; i++) {
        void *ctx = alloc_ctx(impls[i]);
        populate(impls[i], ctx, keys, n);
        mem[i] = impls[i]->memory_usage(ctx);
        free_ctx(impls[i], ctx);
    }

    printf("    %-14s", "total");
    for (size_t i = 0; i < n_impls; i++)
        printf(" %10zu", mem[i]);
    printf("\n");

    printf("    %-14s", "bytes/entry");
    for (size_t i = 0; i < n_impls; i++)
        printf(" %10.1f", (double)mem[i] / (double)n);
    printf("\n");

    free(mem);
    free(keys);
    free(keys_shuffled);
    free(keys_miss);
}

int
main(void)
{
    printf("Hash Table Benchmark\n");
    printf("====================\n");

    struct perf_group pg;
    perf_group_open(&pg);

    if (!pg.available)
        printf("\nNOTE: Hardware perf counters unavailable; "
               "reporting wall time only.\n");

    const bench_impl *impls[] = { &impl_swtab, &impl_chained };
    size_t n_impls = sizeof(impls) / sizeof(impls[0]);

    static const struct bench_config configs[] = {
        {      64, "64 (L1-resident)"    },
        {    1024, "1024 (L2-resident)"   },
        {   65536, "65536 (L3-resident)"  },
        { 1048576, "1048576 (exceeds L3)" },
    };

    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++)
        run_benchmarks(&configs[i], impls, n_impls, &pg);

    printf("\n");
    perf_group_close(&pg);
    return 0;
}
