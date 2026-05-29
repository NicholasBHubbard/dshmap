#ifndef BENCH_IMPL_H
#define BENCH_IMPL_H

#include <stddef.h>

typedef size_t bench_hash_t;
typedef bench_hash_t (*bench_hash_fn)(const void *entry);
typedef void (*bench_iter_cb)(void *entry, void *arg);

typedef struct {
    const char *name;
    size_t ctx_size;
    void (*init)(void *ctx, bench_hash_fn hash_fn);
    void (*destroy)(void *ctx);
    void (*insert)(void *ctx, void *entry, bench_hash_t hash);
    void *(*find)(const void *ctx, bench_hash_t hash);
    void (*remove)(void *ctx, const void *entry, bench_hash_t hash);
    void (*reserve)(void *ctx, size_t count);
    size_t (*size)(const void *ctx);
    void (*clear)(void *ctx);
    void (*for_each)(const void *ctx, bench_iter_cb cb, void *arg);
    size_t (*memory_usage)(const void *ctx);
} bench_impl;

#endif
