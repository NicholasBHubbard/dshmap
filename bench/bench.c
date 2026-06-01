#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#include "bench_impl.h"
#include "impl_dshmap.h"
#include "impl_chained.h"

#define DO_NOT_OPTIMIZE(val) __asm__ volatile("" : "+r"(val) :: "memory")
#define COMPILER_BARRIER()   __asm__ volatile("" ::: "memory")

#define DEFAULT_MIN_TOTAL_OPS 200000

#ifndef PERF_IOC_FLAG_GROUP
#define PERF_IOC_FLAG_GROUP (1U << 0)
#endif

enum bench_op {
    BENCH_INSERT_SEQ = 0,
    BENCH_INSERT_RND,
    BENCH_FIND_HIT,
    BENCH_FIND_MISS,
    BENCH_REMOVE,
    BENCH_ITERATE,
    BENCH_MIXED,
    BENCH_OP_COUNT,
};

enum output_mode {
    OUTPUT_DETAIL = 0,
    OUTPUT_COMPARE,
    OUTPUT_CSV,
};

enum bench_key_mode {
    BENCH_KEYS_PTR = 0,
    BENCH_KEYS_STRING,
    BENCH_KEYS_EXPENSIVE,
    BENCH_KEYS_COUNT,
};

static const char *bench_op_names[BENCH_OP_COUNT] = {
    "insert_seq",
    "insert_rnd",
    "find_hit",
    "find_miss",
    "remove",
    "iterate",
    "mixed",
};

static const char *bench_key_mode_names[BENCH_KEYS_COUNT] = {
    "ptr",
    "string",
    "expensive",
};

struct bench_options {
    size_t *sizes;
    size_t n_sizes;
    size_t sizes_cap;
    size_t min_total_ops;
    uint32_t op_mask;
    enum bench_key_mode key_mode;
    enum output_mode mode;
    bool use_perf;
};

static size_t min_total_ops = DEFAULT_MIN_TOTAL_OPS;

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

/* --- Key workloads --- */

struct bench_entry {
    size_t len;
    char text[48];
};

struct bench_keys {
    enum bench_key_mode mode;
    bench_hash_fn hash_fn;
    struct bench_entry *entries;
    void **ptrs;
    bench_hash_t *hashes;
    void **ptrs_shuffled;
    bench_hash_t *hashes_shuffled;
    bench_hash_t *hashes_miss;
};

static void *
xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    return p;
}

static uint64_t
mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static void
bench_entry_init(struct bench_entry *entry, uint64_t value)
{
    int len;

    len = snprintf(entry->text, sizeof(entry->text), "key-%016llx-%016llx",
                   (unsigned long long)value,
                   (unsigned long long)mix64(value));
    if (len < 0 || (size_t)len >= sizeof(entry->text)) {
        fprintf(stderr, "bench: generated key overflow\n");
        exit(1);
    }
    entry->len = (size_t)len;
}

static bench_hash_t
hash_bytes(const char *data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;

    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 1099511628211ULL;
    }
    return (bench_hash_t)mix64(h ^ len);
}

static bench_hash_t
bench_hash_ptr(const void *entry)
{
    return (bench_hash_t)(uintptr_t)entry;
}

static bench_hash_t
bench_hash_string(const void *entry)
{
    const struct bench_entry *e = entry;
    return hash_bytes(e->text, e->len);
}

static bench_hash_t
bench_hash_expensive(const void *entry)
{
    const struct bench_entry *e = entry;
    uint64_t h = hash_bytes(e->text, e->len);

    for (uint64_t i = 0; i < 32; i++)
        h = mix64(h + 0x9e3779b97f4a7c15ULL + i);
    return (bench_hash_t)h;
}

static bench_hash_fn
bench_hash_fn_for_mode(enum bench_key_mode mode)
{
    switch (mode) {
    case BENCH_KEYS_PTR:
        return bench_hash_ptr;
    case BENCH_KEYS_STRING:
        return bench_hash_string;
    case BENCH_KEYS_EXPENSIVE:
        return bench_hash_expensive;
    case BENCH_KEYS_COUNT:
        break;
    }
    abort();
}

static void
bench_make_key(enum bench_key_mode mode, bench_hash_fn hash_fn,
               uint64_t value, struct bench_entry *entry,
               void **ptr, bench_hash_t *hash)
{
    if (mode == BENCH_KEYS_PTR) {
        *ptr = (void *)(uintptr_t)value;
        *hash = hash_fn(*ptr);
        return;
    }

    bench_entry_init(entry, value);
    *ptr = entry;
    *hash = hash_fn(entry);
}

static bench_hash_t
bench_make_miss_hash(enum bench_key_mode mode, bench_hash_fn hash_fn,
                     uint64_t value)
{
    struct bench_entry entry;
    void *ptr;
    bench_hash_t hash;

    bench_make_key(mode, hash_fn, value, &entry, &ptr, &hash);
    return hash;
}

static void
shuffle_pairs(void **ptrs, bench_hash_t *hashes, size_t n)
{
    if (n < 2)
        return;

    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rng_next() % (i + 1);
        void *ptr_tmp = ptrs[i];
        bench_hash_t hash_tmp = hashes[i];
        ptrs[i] = ptrs[j];
        hashes[i] = hashes[j];
        ptrs[j] = ptr_tmp;
        hashes[j] = hash_tmp;
    }
}

static void
bench_keys_init(struct bench_keys *keys, enum bench_key_mode mode, size_t n)
{
    keys->mode = mode;
    keys->hash_fn = bench_hash_fn_for_mode(mode);
    keys->entries = mode == BENCH_KEYS_PTR ? NULL :
        xmalloc(n * sizeof(*keys->entries));
    keys->ptrs = xmalloc(n * sizeof(*keys->ptrs));
    keys->hashes = xmalloc(n * sizeof(*keys->hashes));
    keys->ptrs_shuffled = xmalloc(n * sizeof(*keys->ptrs_shuffled));
    keys->hashes_shuffled = xmalloc(n * sizeof(*keys->hashes_shuffled));
    keys->hashes_miss = xmalloc(n * sizeof(*keys->hashes_miss));

    for (size_t i = 0; i < n; i++) {
        struct bench_entry *entry = keys->entries ? &keys->entries[i] : NULL;
        bench_make_key(mode, keys->hash_fn, rng_next() | 1, entry,
                       &keys->ptrs[i], &keys->hashes[i]);
    }

    memcpy(keys->ptrs_shuffled, keys->ptrs, n * sizeof(*keys->ptrs));
    memcpy(keys->hashes_shuffled, keys->hashes, n * sizeof(*keys->hashes));
    shuffle_pairs(keys->ptrs_shuffled, keys->hashes_shuffled, n);

    for (size_t i = 0; i < n; i++)
        keys->hashes_miss[i] = bench_make_miss_hash(mode, keys->hash_fn,
                                                    rng_next() | 1);
}

static void
bench_keys_destroy(struct bench_keys *keys)
{
    free(keys->entries);
    free(keys->ptrs);
    free(keys->hashes);
    free(keys->ptrs_shuffled);
    free(keys->hashes_shuffled);
    free(keys->hashes_miss);
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
perf_group_init_disabled(struct perf_group *pg)
{
    for (int i = 0; i < NUM_HW_COUNTERS; i++)
        pg->fd[i] = -1;
    pg->available = false;
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

static double
result_ns_per_op(const struct bench_result *r)
{
    return (double)r->elapsed_ns / (double)r->n_ops;
}

static double
result_counter_per_op(const struct bench_result *r, int counter)
{
    return (double)r->counters[counter] / (double)r->n_ops;
}

static bool
op_enabled(uint32_t op_mask, enum bench_op op)
{
    return (op_mask & (1u << op)) != 0;
}

/* --- Helpers --- */

static size_t
calc_iters(size_t n)
{
    size_t iters = min_total_ops / n;
    return iters < 1 ? 1 : iters;
}

static void *
alloc_ctx(const bench_impl *impl, bench_hash_fn hash_fn)
{
    void *ctx = calloc(1, impl->ctx_size);
    impl->init(ctx, hash_fn);
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
         void *const *entries, const bench_hash_t *hashes, size_t n)
{
    if (impl->reserve)
        impl->reserve(ctx, n);
    for (size_t i = 0; i < n; i++)
        impl->insert(ctx, entries[i], hashes[i]);
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
                 bench_hash_fn hash_fn, void *const *entries,
                 const bench_hash_t *hashes, struct perf_group *pg)
{
    struct bench_result res = { .name = "insert_seq" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl, hash_fn);
    for (size_t i = 0; i < n; i++)
        impl->insert(ctx, entries[i], hashes[i]);
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl, hash_fn);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++)
            impl->insert(ctx, entries[i], hashes[i]);

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
                 bench_hash_fn hash_fn, void *const *entries_shuffled,
                 const bench_hash_t *hashes_shuffled, struct perf_group *pg)
{
    struct bench_result res = { .name = "insert_rnd" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl, hash_fn);
    for (size_t i = 0; i < n; i++)
        impl->insert(ctx, entries_shuffled[i], hashes_shuffled[i]);
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl, hash_fn);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++)
            impl->insert(ctx, entries_shuffled[i], hashes_shuffled[i]);

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
bench_find_hit(const bench_impl *impl, size_t n, bench_hash_fn hash_fn,
               void *const *entries, const bench_hash_t *hashes,
               const bench_hash_t *hashes_shuffled, struct perf_group *pg)
{
    struct bench_result res = { .name = "find_hit" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl, hash_fn);
    populate(impl, ctx, entries, hashes, n);

    for (size_t i = 0; i < n; i++) {
        void *p = impl->find(ctx, hashes_shuffled[i]);
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
            void *p = impl->find(ctx, hashes_shuffled[i]);
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
bench_find_miss(const bench_impl *impl, size_t n, bench_hash_fn hash_fn,
                void *const *entries, const bench_hash_t *hashes,
                const bench_hash_t *hashes_miss, struct perf_group *pg)
{
    struct bench_result res = { .name = "find_miss" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl, hash_fn);
    populate(impl, ctx, entries, hashes, n);

    for (size_t i = 0; i < n; i++) {
        void *p = impl->find(ctx, hashes_miss[i]);
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
            void *p = impl->find(ctx, hashes_miss[i]);
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
bench_remove_all(const bench_impl *impl, size_t n, bench_hash_fn hash_fn,
                 void *const *entries, const bench_hash_t *hashes,
                 void *const *entries_shuffled,
                 const bench_hash_t *hashes_shuffled, struct perf_group *pg)
{
    struct bench_result res = { .name = "remove" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl, hash_fn);
    populate(impl, ctx, entries, hashes, n);
    for (size_t i = 0; i < n; i++)
        impl->remove(ctx, entries_shuffled[i], hashes_shuffled[i]);
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl, hash_fn);
        populate(impl, ctx, entries, hashes, n);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n; i++)
            impl->remove(ctx, entries_shuffled[i], hashes_shuffled[i]);

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
bench_iterate(const bench_impl *impl, size_t n, bench_hash_fn hash_fn,
              void *const *entries, const bench_hash_t *hashes,
              struct perf_group *pg)
{
    struct bench_result res = { .name = "iterate" };
    size_t iters = calc_iters(n);

    void *ctx = alloc_ctx(impl, hash_fn);
    populate(impl, ctx, entries, hashes, n);

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
bench_mixed(const bench_impl *impl, size_t n, const struct bench_keys *keys,
            struct perf_group *pg)
{
    struct bench_result res = { .name = "mixed" };
    size_t iters = calc_iters(n);

    size_t n_ops = n;
    size_t initial = (n + 1) / 2;
    size_t extra = n - initial;

    enum { OP_FIND = 0, OP_INSERT = 1, OP_REMOVE = 2 };

    uint8_t *ops = xmalloc(n_ops);
    size_t *find_idx = xmalloc(n_ops * sizeof(size_t));
    struct bench_entry *extra_entries = extra && keys->mode != BENCH_KEYS_PTR ?
        xmalloc(extra * sizeof(*extra_entries)) : NULL;
    void **extra_ptrs = extra ? xmalloc(extra * sizeof(*extra_ptrs)) : NULL;
    bench_hash_t *extra_hashes = extra ?
        xmalloc(extra * sizeof(*extra_hashes)) : NULL;

    uint64_t saved_state = rng_state;

    for (size_t i = 0; i < extra; i++) {
        struct bench_entry *entry = extra_entries ? &extra_entries[i] : NULL;
        bench_make_key(keys->mode, keys->hash_fn, rng_next() | 1, entry,
                       &extra_ptrs[i], &extra_hashes[i]);
    }

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

    void *ctx = alloc_ctx(impl, keys->hash_fn);
    if (impl->reserve)
        impl->reserve(ctx, initial + extra);
    for (size_t i = 0; i < initial; i++)
        impl->insert(ctx, keys->ptrs[i], keys->hashes[i]);

    for (size_t i = 0; i < n_ops; i++) {
        switch (ops[i]) {
        case OP_FIND: {
            void *p = impl->find(ctx, keys->hashes[find_idx[i]]);
            DO_NOT_OPTIMIZE(p);
            break;
        }
        case OP_INSERT:
            impl->insert(ctx, extra_ptrs[find_idx[i]],
                         extra_hashes[find_idx[i]]);
            break;
        case OP_REMOVE:
            impl->remove(ctx, keys->ptrs[find_idx[i]],
                         keys->hashes[find_idx[i]]);
            break;
        }
    }
    free_ctx(impl, ctx);

    uint64_t total_ns = 0;
    uint64_t total_ctr[NUM_HW_COUNTERS] = {0};

    for (size_t iter = 0; iter < iters; iter++) {
        ctx = alloc_ctx(impl, keys->hash_fn);
        if (impl->reserve)
            impl->reserve(ctx, initial + extra);
        for (size_t i = 0; i < initial; i++)
            impl->insert(ctx, keys->ptrs[i], keys->hashes[i]);

        perf_group_reset(pg);
        perf_group_enable(pg);
        uint64_t t0 = time_ns();
        COMPILER_BARRIER();

        for (size_t i = 0; i < n_ops; i++) {
            switch (ops[i]) {
            case OP_FIND: {
                void *p = impl->find(ctx, keys->hashes[find_idx[i]]);
                DO_NOT_OPTIMIZE(p);
                break;
            }
            case OP_INSERT:
                impl->insert(ctx, extra_ptrs[find_idx[i]],
                             extra_hashes[find_idx[i]]);
                break;
            case OP_REMOVE:
                impl->remove(ctx, keys->ptrs[find_idx[i]],
                             keys->hashes[find_idx[i]]);
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
    free(extra_entries);
    free(extra_ptrs);
    free(extra_hashes);
    return res;
}

static struct bench_result
run_operation(enum bench_op op, const bench_impl *impl, size_t n,
              const struct bench_keys *keys, struct perf_group *pg)
{
    switch (op) {
    case BENCH_INSERT_SEQ:
        return bench_insert_seq(impl, n, keys->hash_fn, keys->ptrs,
                                keys->hashes, pg);
    case BENCH_INSERT_RND:
        return bench_insert_rnd(impl, n, keys->hash_fn,
                                keys->ptrs_shuffled,
                                keys->hashes_shuffled, pg);
    case BENCH_FIND_HIT:
        return bench_find_hit(impl, n, keys->hash_fn, keys->ptrs,
                              keys->hashes, keys->hashes_shuffled, pg);
    case BENCH_FIND_MISS:
        return bench_find_miss(impl, n, keys->hash_fn, keys->ptrs,
                               keys->hashes, keys->hashes_miss, pg);
    case BENCH_REMOVE:
        return bench_remove_all(impl, n, keys->hash_fn, keys->ptrs,
                                keys->hashes, keys->ptrs_shuffled,
                                keys->hashes_shuffled, pg);
    case BENCH_ITERATE:
        return bench_iterate(impl, n, keys->hash_fn, keys->ptrs,
                             keys->hashes, pg);
    case BENCH_MIXED:
        return bench_mixed(impl, n, keys, pg);
    case BENCH_OP_COUNT:
        break;
    }
    abort();
}

static size_t
measure_memory(const bench_impl *impl, const struct bench_keys *keys, size_t n)
{
    void *ctx = alloc_ctx(impl, keys->hash_fn);
    populate(impl, ctx, keys->ptrs, keys->hashes, n);
    size_t mem = impl->memory_usage(ctx);
    free_ctx(impl, ctx);
    return mem;
}

/* --- Driver --- */

static void
print_csv_header(void)
{
    printf("keys,size,impl,operation,ns_per_op,l1miss_per_op,llcmiss_per_op,"
           "insn_per_op,brmiss_per_op,memory_bytes,bytes_per_entry\n");
}

static void
print_csv_result(enum bench_key_mode key_mode, size_t n, const bench_impl *impl,
                 const struct bench_result *r, bool have_perf, size_t memory)
{
    printf("%s,%zu,%s,%s,%.3f,", bench_key_mode_names[key_mode], n,
           impl->name, r->name, result_ns_per_op(r));
    if (have_perf) {
        printf("%.3f,%.3f,%.3f,%.3f,",
               result_counter_per_op(r, CTR_L1D_MISS),
               result_counter_per_op(r, CTR_LLC_MISS),
               result_counter_per_op(r, CTR_INSN),
               result_counter_per_op(r, CTR_BR_MISS));
    } else {
        printf(",,,,");
    }
    printf("%zu,%.3f\n", memory, (double)memory / (double)n);
}

static void
run_benchmarks(size_t n, const bench_impl **impls, size_t n_impls,
               const struct bench_options *opts, struct perf_group *pg)
{
    printf("\nTable size: %zu (%s keys)\n", n,
           bench_key_mode_names[opts->key_mode]);

    rng_seed(0xdeadbeefcafe1234ULL ^ n);
    struct bench_keys keys;
    bench_keys_init(&keys, opts->key_mode, n);

    for (size_t impl_i = 0; impl_i < n_impls; impl_i++) {
        const bench_impl *impl = impls[impl_i];
        printf("\n  [%s]\n", impl->name);
        print_header();

        struct bench_result r;

        for (enum bench_op op = 0; op < BENCH_OP_COUNT; op++) {
            if (!op_enabled(opts->op_mask, op))
                continue;
            r = run_operation(op, impl, n, &keys, pg);
            print_result(&r, pg->available);
        }
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

    size_t *mem = xmalloc(n_impls * sizeof(size_t));
    for (size_t i = 0; i < n_impls; i++)
        mem[i] = measure_memory(impls[i], &keys, n);

    printf("    %-14s", "total");
    for (size_t i = 0; i < n_impls; i++)
        printf(" %10zu", mem[i]);
    printf("\n");

    printf("    %-14s", "bytes/entry");
    for (size_t i = 0; i < n_impls; i++)
        printf(" %10.1f", (double)mem[i] / (double)n);
    printf("\n");

    free(mem);
    bench_keys_destroy(&keys);
}

static void
run_compare(size_t n, const bench_impl **impls, size_t n_impls,
            const struct bench_options *opts, struct perf_group *pg)
{
    if (n_impls < 2)
        abort();

    rng_seed(0xdeadbeefcafe1234ULL ^ n);
    struct bench_keys keys;
    bench_keys_init(&keys, opts->key_mode, n);

    for (enum bench_op op = 0; op < BENCH_OP_COUNT; op++) {
        if (!op_enabled(opts->op_mask, op))
            continue;

        struct bench_result dshmap = run_operation(op, impls[0], n, &keys, pg);
        struct bench_result chained = run_operation(op, impls[1], n, &keys, pg);
        double dshmap_ns = result_ns_per_op(&dshmap);
        double chained_ns = result_ns_per_op(&chained);
        const char *winner = dshmap_ns < chained_ns ? impls[0]->name : impls[1]->name;
        printf("%8zu %-12s %10.3f %10.3f %8.3f %8s\n",
               n, bench_op_names[op], dshmap_ns, chained_ns,
               dshmap_ns / chained_ns, winner);
    }

    bench_keys_destroy(&keys);
}

static void
run_csv(size_t n, const bench_impl **impls, size_t n_impls,
        const struct bench_options *opts, struct perf_group *pg)
{
    rng_seed(0xdeadbeefcafe1234ULL ^ n);
    struct bench_keys keys;
    bench_keys_init(&keys, opts->key_mode, n);

    for (size_t impl_i = 0; impl_i < n_impls; impl_i++) {
        const bench_impl *impl = impls[impl_i];
        size_t memory = measure_memory(impl, &keys, n);
        for (enum bench_op op = 0; op < BENCH_OP_COUNT; op++) {
            if (!op_enabled(opts->op_mask, op))
                continue;
            struct bench_result r = run_operation(op, impl, n, &keys, pg);
            print_csv_result(opts->key_mode, n, impl, &r, pg->available,
                             memory);
        }
    }

    bench_keys_destroy(&keys);
}

static void
usage(const char *prog, FILE *out)
{
    fprintf(out,
            "usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  --sizes LIST          comma-separated sizes, e.g. 1,2,4,8,16\n"
            "  --linear A:B[:STEP]   add every STEP sizes from A through B\n"
            "  --geometric A:B[:MUL] add sizes A, A*MUL, ... through B\n"
            "  --keys MODE           key workload: ptr, string, expensive\n"
            "  --ops LIST            comma-separated operations or all\n"
            "  --compare             print compact dshmap/chained comparison\n"
            "  --csv                 print machine-readable CSV rows\n"
            "  --min-ops N           target at least N operations per benchmark\n"
            "  --no-perf             skip hardware performance counters\n"
            "  --help                show this help\n"
            "\n"
            "Operations: insert_seq, insert_rnd, find_hit, find_miss, "
            "remove, iterate, mixed\n",
            prog);
}

static void
die_usage(const char *prog, const char *msg)
{
    fprintf(stderr, "bench: %s\n\n", msg);
    usage(prog, stderr);
    exit(2);
}

static bool
parse_size_value(const char *s, size_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || end == s || *end != '\0' || v == 0 ||
        v > (unsigned long long)SIZE_MAX) {
        return false;
    }
    *out = (size_t)v;
    return true;
}

static void
add_size(struct bench_options *opts, size_t n)
{
    if (opts->n_sizes == opts->sizes_cap) {
        size_t new_cap = opts->sizes_cap ? opts->sizes_cap * 2 : 16;
        size_t *new_sizes = realloc(opts->sizes, new_cap * sizeof(size_t));
        if (!new_sizes) {
            fprintf(stderr, "bench: out of memory\n");
            exit(1);
        }
        opts->sizes = new_sizes;
        opts->sizes_cap = new_cap;
    }
    opts->sizes[opts->n_sizes++] = n;
}

static void
add_default_sizes(struct bench_options *opts)
{
    static const size_t defaults[] = { 64, 1024, 65536, 1048576 };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        add_size(opts, defaults[i]);
}

static void
parse_sizes(const char *prog, struct bench_options *opts, const char *arg)
{
    char *copy = malloc(strlen(arg) + 1);
    if (!copy) {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    strcpy(copy, arg);

    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        size_t n;
        if (!parse_size_value(tok, &n))
            die_usage(prog, "invalid --sizes value");
        add_size(opts, n);
    }

    free(copy);
}

static void
parse_range(const char *prog, const char *arg,
            size_t *start, size_t *end, size_t *step)
{
    char *copy = malloc(strlen(arg) + 1);
    if (!copy) {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    strcpy(copy, arg);

    char *second = strchr(copy, ':');
    if (!second)
        die_usage(prog, "range must be A:B or A:B:STEP");
    *second++ = '\0';

    char *third = strchr(second, ':');
    if (third)
        *third++ = '\0';

    if (!parse_size_value(copy, start) ||
        !parse_size_value(second, end) ||
        (third && !parse_size_value(third, step))) {
        die_usage(prog, "invalid range value");
    }
    if (!third)
        *step = 0;
    if (*end < *start)
        die_usage(prog, "range end must be >= start");

    free(copy);
}

static void
parse_linear(const char *prog, struct bench_options *opts, const char *arg)
{
    size_t start, end, step;
    parse_range(prog, arg, &start, &end, &step);
    if (step == 0)
        step = 1;

    for (size_t n = start; n <= end; ) {
        add_size(opts, n);
        if (end - n < step)
            break;
        n += step;
    }
}

static void
parse_geometric(const char *prog, struct bench_options *opts, const char *arg)
{
    size_t start, end, mul;
    parse_range(prog, arg, &start, &end, &mul);
    if (mul == 0)
        mul = 2;
    if (mul < 2)
        die_usage(prog, "geometric multiplier must be >= 2");

    for (size_t n = start; n <= end; ) {
        add_size(opts, n);
        if (n > end / mul)
            break;
        n *= mul;
    }
}

static bool
parse_op_name(const char *name, enum bench_op *op)
{
    for (enum bench_op i = 0; i < BENCH_OP_COUNT; i++) {
        if (strcmp(name, bench_op_names[i]) == 0) {
            *op = i;
            return true;
        }
    }
    return false;
}

static void
parse_ops(const char *prog, struct bench_options *opts, const char *arg)
{
    if (strcmp(arg, "all") == 0) {
        opts->op_mask = (1u << BENCH_OP_COUNT) - 1;
        return;
    }

    char *copy = malloc(strlen(arg) + 1);
    if (!copy) {
        fprintf(stderr, "bench: out of memory\n");
        exit(1);
    }
    strcpy(copy, arg);

    opts->op_mask = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        enum bench_op op;
        if (!parse_op_name(tok, &op))
            die_usage(prog, "invalid operation name");
        opts->op_mask |= 1u << op;
    }
    if (opts->op_mask == 0)
        die_usage(prog, "--ops must name at least one operation");

    free(copy);
}

static bool
parse_key_mode_name(const char *name, enum bench_key_mode *mode)
{
    for (enum bench_key_mode i = 0; i < BENCH_KEYS_COUNT; i++) {
        if (strcmp(name, bench_key_mode_names[i]) == 0) {
            *mode = i;
            return true;
        }
    }
    return false;
}

static void
parse_key_mode(const char *prog, struct bench_options *opts, const char *arg)
{
    if (!parse_key_mode_name(arg, &opts->key_mode))
        die_usage(prog, "invalid --keys value");
}

static void
parse_args(int argc, char **argv, struct bench_options *opts)
{
    opts->sizes = NULL;
    opts->n_sizes = 0;
    opts->sizes_cap = 0;
    opts->min_total_ops = DEFAULT_MIN_TOTAL_OPS;
    opts->op_mask = (1u << BENCH_OP_COUNT) - 1;
    opts->key_mode = BENCH_KEYS_PTR;
    opts->mode = OUTPUT_DETAIL;
    opts->use_perf = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0], stdout);
            exit(0);
        } else if (strcmp(argv[i], "--sizes") == 0) {
            if (++i == argc)
                die_usage(argv[0], "--sizes requires an argument");
            parse_sizes(argv[0], opts, argv[i]);
        } else if (strcmp(argv[i], "--linear") == 0) {
            if (++i == argc)
                die_usage(argv[0], "--linear requires an argument");
            parse_linear(argv[0], opts, argv[i]);
        } else if (strcmp(argv[i], "--geometric") == 0) {
            if (++i == argc)
                die_usage(argv[0], "--geometric requires an argument");
            parse_geometric(argv[0], opts, argv[i]);
        } else if (strcmp(argv[i], "--keys") == 0) {
            if (++i == argc)
                die_usage(argv[0], "--keys requires an argument");
            parse_key_mode(argv[0], opts, argv[i]);
        } else if (strcmp(argv[i], "--ops") == 0) {
            if (++i == argc)
                die_usage(argv[0], "--ops requires an argument");
            parse_ops(argv[0], opts, argv[i]);
        } else if (strcmp(argv[i], "--compare") == 0) {
            opts->mode = OUTPUT_COMPARE;
        } else if (strcmp(argv[i], "--csv") == 0) {
            opts->mode = OUTPUT_CSV;
        } else if (strcmp(argv[i], "--min-ops") == 0) {
            if (++i == argc)
                die_usage(argv[0], "--min-ops requires an argument");
            if (!parse_size_value(argv[i], &opts->min_total_ops))
                die_usage(argv[0], "invalid --min-ops value");
        } else if (strcmp(argv[i], "--no-perf") == 0) {
            opts->use_perf = false;
        } else {
            die_usage(argv[0], "unknown option");
        }
    }

    if (opts->n_sizes == 0)
        add_default_sizes(opts);
}

int
main(int argc, char **argv)
{
    struct bench_options opts;
    parse_args(argc, argv, &opts);
    min_total_ops = opts.min_total_ops;

    if (opts.mode != OUTPUT_CSV) {
        printf("Hash Table Benchmark\n");
        printf("====================\n");
        printf("Key workload: %s\n", bench_key_mode_names[opts.key_mode]);
    }

    struct perf_group pg;
    if (opts.use_perf)
        perf_group_open(&pg);
    else
        perf_group_init_disabled(&pg);

    if (opts.mode != OUTPUT_CSV && opts.use_perf && !pg.available)
        printf("\nNOTE: Hardware perf counters unavailable; "
               "reporting wall time only.\n");

    const bench_impl *impls[] = { &impl_dshmap, &impl_chained };
    size_t n_impls = sizeof(impls) / sizeof(impls[0]);

    if (opts.mode == OUTPUT_COMPARE) {
        printf("\n%8s %-12s %10s %10s %8s %8s\n",
               "size", "operation", "dshmap", "chained", "ratio", "winner");
        printf("%8s %-12s %10s %10s %8s %8s\n",
               "--------", "------------", "----------", "----------",
               "--------", "--------");
    } else if (opts.mode == OUTPUT_CSV) {
        print_csv_header();
    }

    for (size_t i = 0; i < opts.n_sizes; i++) {
        if (opts.mode == OUTPUT_DETAIL) {
            run_benchmarks(opts.sizes[i], impls, n_impls, &opts, &pg);
        } else if (opts.mode == OUTPUT_COMPARE) {
            run_compare(opts.sizes[i], impls, n_impls, &opts, &pg);
        } else {
            run_csv(opts.sizes[i], impls, n_impls, &opts, &pg);
        }
    }

    if (opts.mode != OUTPUT_CSV)
        printf("\n");
    perf_group_close(&pg);
    free(opts.sizes);
    return 0;
}
