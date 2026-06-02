#ifndef IMPL_FLAT_H
#define IMPL_FLAT_H

#include <stdlib.h>
#include <string.h>
#include "bench_impl.h"

struct flat_slot {
    bench_hash_t hash;
    void *entry;
};

struct flat_hmap {
    struct flat_slot *slots;
    size_t cap;
    size_t n;
};

static void
flat_init(struct flat_hmap *hm)
{
    hm->slots = NULL;
    hm->cap = 0;
    hm->n = 0;
}

static void
flat_destroy(struct flat_hmap *hm)
{
    free(hm->slots);
    flat_init(hm);
}

static size_t
flat_cap_for_count(size_t count)
{
    size_t cap = 8;
    while (cap / 4 * 3 < count)
        cap *= 2;
    return cap;
}

static void
flat_insert_no_grow(struct flat_hmap *hm, void *entry, bench_hash_t hash)
{
    size_t mask = hm->cap - 1;
    size_t pos = hash & mask;

    while (hm->slots[pos].entry != NULL)
        pos = (pos + 1) & mask;

    hm->slots[pos].hash = hash;
    hm->slots[pos].entry = entry;
    hm->n++;
}

static void
flat_resize(struct flat_hmap *hm, size_t new_cap)
{
    struct flat_slot *old_slots = hm->slots;
    size_t old_cap = hm->cap;
    size_t old_n = hm->n;

    hm->slots = calloc(new_cap, sizeof(*hm->slots));
    if (!hm->slots)
        abort();
    hm->cap = new_cap;
    hm->n = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_slots[i].entry != NULL)
            flat_insert_no_grow(hm, old_slots[i].entry, old_slots[i].hash);
    }
    if (hm->n != old_n)
        abort();
    free(old_slots);
}

static void
flat_reserve(struct flat_hmap *hm, size_t count)
{
    size_t cap = flat_cap_for_count(count);
    if (cap > hm->cap)
        flat_resize(hm, cap);
}

static void
flat_insert(struct flat_hmap *hm, void *entry, bench_hash_t hash)
{
    if (hm->cap == 0 || (hm->n + 1) > hm->cap / 4 * 3)
        flat_resize(hm, flat_cap_for_count(hm->n + 1));
    flat_insert_no_grow(hm, entry, hash);
}

static void *
flat_find(const struct flat_hmap *hm, bench_hash_t hash)
{
    if (hm->cap == 0)
        return NULL;

    size_t mask = hm->cap - 1;
    size_t pos = hash & mask;

    while (hm->slots[pos].entry != NULL) {
        if (hm->slots[pos].hash == hash)
            return hm->slots[pos].entry;
        pos = (pos + 1) & mask;
    }
    return NULL;
}

static void *
flat_find_key(const struct flat_hmap *hm, bench_hash_t hash, const void *key,
              bench_key_eq_fn eq_fn)
{
    if (hm->cap == 0)
        return NULL;

    size_t mask = hm->cap - 1;
    size_t pos = hash & mask;

    while (hm->slots[pos].entry != NULL) {
        if (hm->slots[pos].hash == hash &&
            eq_fn(hm->slots[pos].entry, key)) {
            return hm->slots[pos].entry;
        }
        pos = (pos + 1) & mask;
    }
    return NULL;
}

static void
flat_erase_at(struct flat_hmap *hm, size_t hole)
{
    size_t mask = hm->cap - 1;
    size_t pos = (hole + 1) & mask;

    while (hm->slots[pos].entry != NULL) {
        size_t ideal = hm->slots[pos].hash & mask;
        if (((hole - ideal) & mask) < ((pos - ideal) & mask)) {
            hm->slots[hole] = hm->slots[pos];
            hole = pos;
        }
        pos = (pos + 1) & mask;
    }

    hm->slots[hole].hash = 0;
    hm->slots[hole].entry = NULL;
    hm->n--;
}

static void
flat_remove(struct flat_hmap *hm, const void *entry, bench_hash_t hash)
{
    if (hm->cap == 0)
        return;

    size_t mask = hm->cap - 1;
    size_t pos = hash & mask;

    while (hm->slots[pos].entry != NULL) {
        if (hm->slots[pos].hash == hash && hm->slots[pos].entry == entry) {
            flat_erase_at(hm, pos);
            return;
        }
        pos = (pos + 1) & mask;
    }
}

static void
flat_clear(struct flat_hmap *hm)
{
    if (hm->slots != NULL)
        memset(hm->slots, 0, hm->cap * sizeof(*hm->slots));
    hm->n = 0;
}

static void
flat_for_each(const struct flat_hmap *hm, bench_iter_cb cb, void *arg)
{
    for (size_t i = 0; i < hm->cap; i++) {
        if (hm->slots[i].entry != NULL)
            cb(hm->slots[i].entry, arg);
    }
}

/* --- vtable wrappers --- */

static void
impl_flat_init(void *ctx, bench_hash_fn hash_fn)
{
    (void)hash_fn;
    flat_init((struct flat_hmap *)ctx);
}

static void
impl_flat_destroy(void *ctx)
{
    flat_destroy((struct flat_hmap *)ctx);
}

static void
impl_flat_insert(void *ctx, void *entry, bench_hash_t hash)
{
    flat_insert((struct flat_hmap *)ctx, entry, hash);
}

static void *
impl_flat_find(const void *ctx, bench_hash_t hash)
{
    return flat_find((const struct flat_hmap *)ctx, hash);
}

static void *
impl_flat_find_key(const void *ctx, bench_hash_t hash, const void *key,
                   bench_key_eq_fn eq_fn)
{
    return flat_find_key((const struct flat_hmap *)ctx, hash, key, eq_fn);
}

static void
impl_flat_remove(void *ctx, const void *entry, bench_hash_t hash)
{
    flat_remove((struct flat_hmap *)ctx, entry, hash);
}

static void
impl_flat_reserve(void *ctx, size_t count)
{
    flat_reserve((struct flat_hmap *)ctx, count);
}

static size_t
impl_flat_size(const void *ctx)
{
    return ((const struct flat_hmap *)ctx)->n;
}

static void
impl_flat_clear(void *ctx)
{
    flat_clear((struct flat_hmap *)ctx);
}

static void
impl_flat_for_each(const void *ctx, bench_iter_cb cb, void *arg)
{
    flat_for_each((const struct flat_hmap *)ctx, cb, arg);
}

static size_t
impl_flat_memory_usage(const void *ctx)
{
    const struct flat_hmap *hm = ctx;
    return hm->cap * sizeof(*hm->slots);
}

static const bench_impl impl_flat = {
    .name         = "flat",
    .ctx_size     = sizeof(struct flat_hmap),
    .init         = impl_flat_init,
    .destroy      = impl_flat_destroy,
    .insert       = impl_flat_insert,
    .find         = impl_flat_find,
    .find_key     = impl_flat_find_key,
    .remove       = impl_flat_remove,
    .reserve      = impl_flat_reserve,
    .size         = impl_flat_size,
    .clear        = impl_flat_clear,
    .for_each     = impl_flat_for_each,
    .memory_usage = impl_flat_memory_usage,
};

#endif
