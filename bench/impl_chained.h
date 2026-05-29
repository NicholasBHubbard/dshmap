#ifndef IMPL_CHAINED_H
#define IMPL_CHAINED_H

#include <stdlib.h>
#include <string.h>
#include "bench_impl.h"

struct chained_slot {
    size_t hash;
    struct chained_slot *next;
    void *entry;
};

struct chained_hmap {
    struct chained_slot **buckets;
    struct chained_slot *one;
    size_t mask;
    size_t n;
};

static void
chained_init(struct chained_hmap *hm)
{
    hm->one = NULL;
    hm->buckets = &hm->one;
    hm->mask = 0;
    hm->n = 0;
}

static void
chained_clear(struct chained_hmap *hm)
{
    for (size_t i = 0; i <= hm->mask; i++) {
        struct chained_slot *s = hm->buckets[i];
        while (s) {
            struct chained_slot *next = s->next;
            free(s);
            s = next;
        }
        hm->buckets[i] = NULL;
    }
    hm->n = 0;
}

static void
chained_destroy(struct chained_hmap *hm)
{
    chained_clear(hm);
    if (hm->buckets != &hm->one)
        free(hm->buckets);
    hm->buckets = &hm->one;
    hm->mask = 0;
}

static void
chained_resize(struct chained_hmap *hm, size_t new_cap)
{
    struct chained_slot **new_buckets = calloc(new_cap, sizeof(struct chained_slot *));
    size_t new_mask = new_cap - 1;

    for (size_t i = 0; i <= hm->mask; i++) {
        struct chained_slot *s = hm->buckets[i];
        while (s) {
            struct chained_slot *next = s->next;
            size_t idx = s->hash & new_mask;
            s->next = new_buckets[idx];
            new_buckets[idx] = s;
            s = next;
        }
    }

    if (hm->buckets != &hm->one)
        free(hm->buckets);
    hm->buckets = new_buckets;
    hm->mask = new_mask;
}

static void
chained_insert(struct chained_hmap *hm, void *entry, size_t hash)
{
    if (hm->n > hm->mask)
        chained_resize(hm, (hm->mask + 1) * 2);

    struct chained_slot *s = malloc(sizeof(*s));
    s->hash = hash;
    s->entry = entry;
    size_t idx = hash & hm->mask;
    s->next = hm->buckets[idx];
    hm->buckets[idx] = s;
    hm->n++;
}

static void *
chained_find(const struct chained_hmap *hm, size_t hash)
{
    size_t idx = hash & hm->mask;
    for (struct chained_slot *s = hm->buckets[idx]; s; s = s->next) {
        if (s->hash == hash)
            return s->entry;
    }
    return NULL;
}

static void
chained_remove(struct chained_hmap *hm, const void *entry, size_t hash)
{
    size_t idx = hash & hm->mask;
    struct chained_slot **pp = &hm->buckets[idx];
    while (*pp) {
        struct chained_slot *s = *pp;
        if (s->entry == entry) {
            *pp = s->next;
            free(s);
            hm->n--;
            return;
        }
        pp = &s->next;
    }
}

static void
chained_reserve(struct chained_hmap *hm, size_t count)
{
    if (count <= hm->mask + 1)
        return;
    size_t new_cap = hm->mask + 1;
    if (new_cap == 0)
        new_cap = 1;
    while (new_cap < count)
        new_cap *= 2;
    chained_resize(hm, new_cap);
}

/* --- vtable wrappers --- */

static void
impl_chained_init(void *ctx, bench_hash_fn hash_fn)
{
    (void)hash_fn;
    chained_init((struct chained_hmap *)ctx);
}

static void
impl_chained_destroy(void *ctx)
{
    chained_destroy((struct chained_hmap *)ctx);
}

static void
impl_chained_insert(void *ctx, void *entry, bench_hash_t hash)
{
    chained_insert((struct chained_hmap *)ctx, entry, hash);
}

static void *
impl_chained_find(const void *ctx, bench_hash_t hash)
{
    return chained_find((const struct chained_hmap *)ctx, hash);
}

static void
impl_chained_remove(void *ctx, const void *entry, bench_hash_t hash)
{
    chained_remove((struct chained_hmap *)ctx, entry, hash);
}

static void
impl_chained_reserve(void *ctx, size_t count)
{
    chained_reserve((struct chained_hmap *)ctx, count);
}

static size_t
impl_chained_size(const void *ctx)
{
    return ((const struct chained_hmap *)ctx)->n;
}

static void
impl_chained_clear(void *ctx)
{
    chained_clear((struct chained_hmap *)ctx);
}

static void
impl_chained_for_each(const void *ctx, bench_iter_cb cb, void *arg)
{
    const struct chained_hmap *hm = (const struct chained_hmap *)ctx;
    for (size_t i = 0; i <= hm->mask; i++) {
        for (struct chained_slot *s = hm->buckets[i]; s; s = s->next)
            cb(s->entry, arg);
    }
}

static size_t
impl_chained_memory_usage(const void *ctx)
{
    const struct chained_hmap *hm = (const struct chained_hmap *)ctx;
    return (hm->mask + 1) * sizeof(struct chained_slot *)
           + hm->n * sizeof(struct chained_slot);
}

static const bench_impl impl_chained = {
    .name         = "chained",
    .ctx_size     = sizeof(struct chained_hmap),
    .init         = impl_chained_init,
    .destroy      = impl_chained_destroy,
    .insert       = impl_chained_insert,
    .find         = impl_chained_find,
    .remove       = impl_chained_remove,
    .reserve      = impl_chained_reserve,
    .size         = impl_chained_size,
    .clear        = impl_chained_clear,
    .for_each     = impl_chained_for_each,
    .memory_usage = impl_chained_memory_usage,
};

#endif
