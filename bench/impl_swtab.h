#ifndef IMPL_SWTAB_H
#define IMPL_SWTAB_H

#include "bench_impl.h"
#include "../swtab.h"

static void
impl_swtab_init(void *ctx, bench_hash_fn hash_fn)
{
    swtab_init((swtab *)ctx, hash_fn);
}

static void
impl_swtab_destroy(void *ctx)
{
    swtab_destroy((swtab *)ctx);
}

static void
impl_swtab_insert(void *ctx, void *entry, bench_hash_t hash)
{
    swtab_insert((swtab *)ctx, entry, hash);
}

static void *
impl_swtab_find(const void *ctx, bench_hash_t hash)
{
    return swtab_find((const swtab *)ctx, hash);
}

static void
impl_swtab_remove(void *ctx, const void *entry, bench_hash_t hash)
{
    swtab_remove((swtab *)ctx, entry, hash);
}

static void
impl_swtab_reserve(void *ctx, size_t count)
{
    swtab_reserve((swtab *)ctx, count);
}

static size_t
impl_swtab_size(const void *ctx)
{
    return swtab_size((const swtab *)ctx);
}

static void
impl_swtab_clear(void *ctx)
{
    swtab_clear((swtab *)ctx);
}

static void
impl_swtab_for_each(const void *ctx, bench_iter_cb cb, void *arg)
{
    const swtab *st = (const swtab *)ctx;
    SWTAB_FOR_EACH(entry, st) {
        cb(entry, arg);
    }
}

static size_t
impl_swtab_memory_usage(const void *ctx)
{
    const swtab *st = (const swtab *)ctx;
    size_t capacity = (st->group_mask + 1) * 8;
    return capacity * (1 + sizeof(void *));
}

static const bench_impl impl_swtab = {
    .name         = "swtab",
    .ctx_size     = sizeof(swtab),
    .init         = impl_swtab_init,
    .destroy      = impl_swtab_destroy,
    .insert       = impl_swtab_insert,
    .find         = impl_swtab_find,
    .remove       = impl_swtab_remove,
    .reserve      = impl_swtab_reserve,
    .size         = impl_swtab_size,
    .clear        = impl_swtab_clear,
    .for_each     = impl_swtab_for_each,
    .memory_usage = impl_swtab_memory_usage,
};

#endif
