#ifndef IMPL_DSHMAP_H
#define IMPL_DSHMAP_H

#include "bench_impl.h"
#include "../dshmap.h"

static void
impl_dshmap_init(void *ctx, bench_hash_fn hash_fn)
{
    dshmap_init((dshmap *)ctx, hash_fn);
}

static void
impl_dshmap_destroy(void *ctx)
{
    dshmap_destroy((dshmap *)ctx);
}

static void
impl_dshmap_insert(void *ctx, void *entry, bench_hash_t hash)
{
    dshmap_insert((dshmap *)ctx, entry, hash);
}

static void *
impl_dshmap_find(const void *ctx, bench_hash_t hash)
{
    return dshmap_find((const dshmap *)ctx, hash);
}

static void
impl_dshmap_remove(void *ctx, const void *entry, bench_hash_t hash)
{
    dshmap_remove((dshmap *)ctx, entry, hash);
}

static void
impl_dshmap_reserve(void *ctx, size_t count)
{
    dshmap_reserve((dshmap *)ctx, count);
}

static size_t
impl_dshmap_size(const void *ctx)
{
    return dshmap_size((const dshmap *)ctx);
}

static void
impl_dshmap_clear(void *ctx)
{
    dshmap_clear((dshmap *)ctx);
}

static void
impl_dshmap_for_each(const void *ctx, bench_iter_cb cb, void *arg)
{
    const dshmap *map = (const dshmap *)ctx;
    DSHMAP_FOR_EACH(entry, map) {
        cb(entry, arg);
    }
}

static size_t
impl_dshmap_memory_usage(const void *ctx)
{
    const dshmap *map = (const dshmap *)ctx;
    if (map->slots == NULL)
        return 0;

    size_t capacity = (map->group_mask + 1) * 8;
    size_t bytes = capacity * (1 + sizeof(void *));
    if (map->hashes != NULL)
        bytes += capacity * sizeof(dshmap_hash_t);
    return bytes;
}

static const bench_impl impl_dshmap = {
    .name         = "dshmap",
    .ctx_size     = sizeof(dshmap),
    .init         = impl_dshmap_init,
    .destroy      = impl_dshmap_destroy,
    .insert       = impl_dshmap_insert,
    .find         = impl_dshmap_find,
    .remove       = impl_dshmap_remove,
    .reserve      = impl_dshmap_reserve,
    .size         = impl_dshmap_size,
    .clear        = impl_dshmap_clear,
    .for_each     = impl_dshmap_for_each,
    .memory_usage = impl_dshmap_memory_usage,
};

#endif
