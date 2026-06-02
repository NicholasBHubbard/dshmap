#ifndef IMPL_PACKED_H
#define IMPL_PACKED_H

#include <stdlib.h>
#include <string.h>
#include "bench_impl.h"

struct packed_slot {
    bench_hash_t hash;
    void *entry;
};

struct packed_hmap {
    struct packed_slot *slots;
    size_t cap;
    size_t n;
};

static void
packed_init(struct packed_hmap *hm)
{
    hm->slots = NULL;
    hm->cap = 0;
    hm->n = 0;
}

static void
packed_destroy(struct packed_hmap *hm)
{
    free(hm->slots);
    packed_init(hm);
}

static void
packed_reserve(struct packed_hmap *hm, size_t count)
{
    if (count <= hm->cap)
        return;

    struct packed_slot *slots = realloc(hm->slots, count * sizeof(*slots));
    if (!slots)
        abort();
    hm->slots = slots;
    hm->cap = count;
}

static void
packed_insert(struct packed_hmap *hm, void *entry, bench_hash_t hash)
{
    if (hm->n == hm->cap) {
        size_t new_cap = hm->cap ? hm->cap * 2 : 8;
        packed_reserve(hm, new_cap);
    }
    hm->slots[hm->n].hash = hash;
    hm->slots[hm->n].entry = entry;
    hm->n++;
}

static void *
packed_find(const struct packed_hmap *hm, bench_hash_t hash)
{
    for (size_t i = 0; i < hm->n; i++) {
        if (hm->slots[i].hash == hash)
            return hm->slots[i].entry;
    }
    return NULL;
}

static void *
packed_find_key(const struct packed_hmap *hm, bench_hash_t hash,
                const void *key, bench_key_eq_fn eq_fn)
{
    for (size_t i = 0; i < hm->n; i++) {
        if (hm->slots[i].hash == hash && eq_fn(hm->slots[i].entry, key))
            return hm->slots[i].entry;
    }
    return NULL;
}

static void
packed_remove(struct packed_hmap *hm, const void *entry, bench_hash_t hash)
{
    for (size_t i = 0; i < hm->n; i++) {
        if (hm->slots[i].hash == hash && hm->slots[i].entry == entry) {
            hm->n--;
            hm->slots[i] = hm->slots[hm->n];
            return;
        }
    }
}

static void
packed_clear(struct packed_hmap *hm)
{
    hm->n = 0;
}

static void
packed_for_each(const struct packed_hmap *hm, bench_iter_cb cb, void *arg)
{
    for (size_t i = 0; i < hm->n; i++)
        cb(hm->slots[i].entry, arg);
}

/* --- vtable wrappers --- */

static void
impl_packed_init(void *ctx, bench_hash_fn hash_fn)
{
    (void)hash_fn;
    packed_init((struct packed_hmap *)ctx);
}

static void
impl_packed_destroy(void *ctx)
{
    packed_destroy((struct packed_hmap *)ctx);
}

static void
impl_packed_insert(void *ctx, void *entry, bench_hash_t hash)
{
    packed_insert((struct packed_hmap *)ctx, entry, hash);
}

static void *
impl_packed_find(const void *ctx, bench_hash_t hash)
{
    return packed_find((const struct packed_hmap *)ctx, hash);
}

static void *
impl_packed_find_key(const void *ctx, bench_hash_t hash, const void *key,
                     bench_key_eq_fn eq_fn)
{
    return packed_find_key((const struct packed_hmap *)ctx, hash, key, eq_fn);
}

static void
impl_packed_remove(void *ctx, const void *entry, bench_hash_t hash)
{
    packed_remove((struct packed_hmap *)ctx, entry, hash);
}

static void
impl_packed_reserve(void *ctx, size_t count)
{
    packed_reserve((struct packed_hmap *)ctx, count);
}

static size_t
impl_packed_size(const void *ctx)
{
    return ((const struct packed_hmap *)ctx)->n;
}

static void
impl_packed_clear(void *ctx)
{
    packed_clear((struct packed_hmap *)ctx);
}

static void
impl_packed_for_each(const void *ctx, bench_iter_cb cb, void *arg)
{
    packed_for_each((const struct packed_hmap *)ctx, cb, arg);
}

static size_t
impl_packed_memory_usage(const void *ctx)
{
    const struct packed_hmap *hm = ctx;
    return hm->cap * sizeof(*hm->slots);
}

static const bench_impl impl_packed = {
    .name         = "packed",
    .ctx_size     = sizeof(struct packed_hmap),
    .init         = impl_packed_init,
    .destroy      = impl_packed_destroy,
    .insert       = impl_packed_insert,
    .find         = impl_packed_find,
    .find_key     = impl_packed_find_key,
    .remove       = impl_packed_remove,
    .reserve      = impl_packed_reserve,
    .size         = impl_packed_size,
    .clear        = impl_packed_clear,
    .for_each     = impl_packed_for_each,
    .memory_usage = impl_packed_memory_usage,
};

#endif
