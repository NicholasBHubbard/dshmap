/*
 * Copyright (c) 2026 Nicholas B. Hubbard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SWTAB_H
#define SWTAB_H

#define SWTAB_VERSION_MAJOR 0
#define SWTAB_VERSION_MINOR 1
#define SWTAB_VERSION_PATCH 0

#if !defined(__GNUC__) && !defined(__clang__)
#error "requires GCC or Clang"
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ===========================================================================
 *                               PUBLIC API
 * =========================================================================== */

/* SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN - Maximum load factor.
 *
 * The table resizes when occupancy exceeds NUM/DEN of capacity.
 * Default is 7/8 (87.5%). Define both macros before including this
 * header to override.
 *
 *     #define SWTAB_LOAD_FACTOR_NUM 3
 *     #define SWTAB_LOAD_FACTOR_DEN 4
 *     #include "swtab.h"
 */
#ifndef SWTAB_LOAD_FACTOR_NUM
#define SWTAB_LOAD_FACTOR_NUM 7
#endif
#ifndef SWTAB_LOAD_FACTOR_DEN
#define SWTAB_LOAD_FACTOR_DEN 8
#endif

/* swtab_hash_t - Hash value type (size_t).
 *
 * swtab_hash_fn - Hash function signature.
 *
 * Must return the same value for a given entry for the lifetime of the
 * table. The table calls this during find, find_next, and resize.
 *
 *     swtab_hash_t my_hash(const void *entry) {
 *         const struct my_obj *obj = entry;
 *         return some_hash(obj->key, obj->key_len);
 *     }
 */
typedef size_t swtab_hash_t;
typedef swtab_hash_t (*swtab_hash_fn)(const void *entry);

/* swtab - A swiss table hash map.
 *
 * Stores void pointers to caller-owned entries. Entries are located by
 * hash; the caller provides a hash function that can recompute the hash
 * from an entry pointer.
 *
 * Must be initialized with swtab_init() before use and cleaned up with
 * swtab_destroy(). Stack allocation is typical:
 *
 *     swtab st;
 *     swtab_init(&st, my_hash);
 */
typedef struct swtab {
    int8_t *ctrl;         /* H2 tag per slot; empty=0x80, deleted=0xFE */
    void **slots;         /* one entry pointer per slot */
    swtab_hash_fn hash_fn; /* rehash entries on resize */
    size_t size;          /* number of occupied slots */
    size_t group_mask;    /* num_groups - 1, for H1 & group_mask */
    size_t growth_left;   /* inserts remaining before resize */
} swtab;

/* swtab_init - Initialize a swiss table.
 *
 * The caller must call swtab_destroy() when done. No memory is
 * allocated until the first insert.
 *
 *     swtab st;
 *     swtab_init(&st, my_hash);
 *     // ... use st ...
 *     swtab_destroy(&st);
 */
static inline void
swtab_init(swtab *st, swtab_hash_fn hash_fn);

/* swtab_destroy - Free all memory owned by the table.
 *
 * Does not free the entries themselves; the caller owns those. The
 * table is reset to its initialized state and may be reused.
 *
 *     swtab_destroy(&st);
 *     // st is now empty and valid, as if swtab_init() was just called
 */
static inline void
swtab_destroy(swtab *st);

/* swtab_size - Return the number of entries in the table.
 *
 *     if (swtab_size(&st) > 1000) {
 *         // table is large
 *     }
 */
static inline size_t
swtab_size(const swtab *st);

/* swtab_is_empty - Return true if the table contains no entries.
 *
 *     if (swtab_is_empty(&st)) {
 *         printf("nothing to process\n");
 *     }
 */
static inline bool
swtab_is_empty(const swtab *st);

/* swtab_clear - Mark all slots as empty.
 *
 * Entry pointers are discarded but not freed; the caller owns those.
 * The table keeps its allocated capacity so subsequent inserts avoid
 * reallocation.
 *
 *     swtab_clear(&st);
 *     assert(swtab_is_empty(&st));
 *     // st retains its capacity, ready for new inserts
 */
static inline void
swtab_clear(swtab *st);

/* swtab_reserve - Pre-allocate capacity for at least 'count' entries.
 *
 * No-op if the table can already hold 'count' entries without resizing.
 * Call this before a batch of inserts to allocate once upfront instead
 * of resizing repeatedly as the table grows.
 *
 *     swtab_reserve(&st, n);
 *     for (size_t i = 0; i < n; i++) {
 *         swtab_insert(&st, entries[i], hashes[i]);
 *     }
 */
static inline void
swtab_reserve(swtab *st, size_t count);

/* swtab_insert - Insert an entry into the table.
 *
 * The caller must provide a pre-computed hash. The table does not check
 * for duplicates; inserting the same entry twice is allowed and both
 * copies will be stored. The entry pointer must remain valid for the
 * lifetime of its membership in the table.
 *
 *     struct my_obj *obj = make_obj("foo");
 *     swtab_insert(&st, obj, my_hash(obj));
 */
static inline void
swtab_insert(swtab *st, void *entry, swtab_hash_t hash);

/* swtab_remove - Remove an entry from the table.
 *
 * Removes by pointer identity, not by hash equality. The caller must
 * pass the exact pointer that was inserted. No-op if the entry is not
 * found. Does not free the entry; the caller owns it.
 *
 *     swtab_remove(&st, obj, my_hash(obj));
 *     free(obj);
 */
static inline void
swtab_remove(swtab *st, const void *entry, swtab_hash_t hash);

/* swtab_find - Look up an entry by hash.
 *
 * Returns the first entry whose hash matches, or NULL if none. When
 * multiple entries share a hash, use swtab_find_next() to iterate
 * through them.
 *
 *     void *obj = swtab_find(&st, hash);
 *     if (obj) {
 *         printf("found: %s\n", ((struct my_obj *)obj)->name);
 *     }
 */
static inline void *
swtab_find(const swtab *st, swtab_hash_t hash);

/* swtab_find_next - Continue a lookup after swtab_find().
 *
 * Returns the next entry with the same hash after 'prev', or NULL if
 * there are no more. 'prev' must be a pointer previously returned by
 * swtab_find() or swtab_find_next() for the same hash.
 *
 *     swtab_hash_t h = my_hash(key);
 *     for (void *e = swtab_find(&st, h); e; e = swtab_find_next(&st, h, e)) {
 *         process(e);
 *     }
 */
static inline void *
swtab_find_next(const swtab *st, swtab_hash_t hash, const void *prev);

/* SWTAB_FOR_EACH - Iterate over all entries in the table.
 *
 * 'var' is declared as void * in the loop scope. Iteration order is
 * arbitrary and not related to insertion order. Do not insert or remove
 * entries during iteration.
 *
 * Skips empty groups in bulk using SWAR, so iteration cost scales with
 * the number of entries, not the table capacity.
 *
 *     SWTAB_FOR_EACH(entry, &st) {
 *         printf("%s\n", ((struct my_obj *)entry)->name);
 *     }
 */
#define SWTAB_FOR_EACH(var, st) \
    for (size_t var##_g_ = 0; var##_g_ <= (st)->group_mask; var##_g_++) \
    for (uint64_t var##_o_ = swtab__ctrl_occupied(swtab__load_ctrl(st, var##_g_)); \
         var##_o_; \
         var##_o_ &= var##_o_ - 1) \
    for (void *var = (st)->slots[swtab__slot_pos(var##_g_, swtab__match_slot(var##_o_))]; \
         var; var = NULL)

/* ===========================================================================
 *                                INTERNAL
 * =========================================================================== */

enum {
    SWTAB__EMPTY   = (int8_t)0x80,
    SWTAB__DELETED = (int8_t)0xFE,
};

static const uint64_t SWTAB__BROADCAST_BYTE = 0x0101010101010101ULL;
static const uint64_t SWTAB__HIGH_BITS      = 0x8080808080808080ULL;

static const int8_t swtab__empty_ctrl[8] = {
    SWTAB__EMPTY, SWTAB__EMPTY, SWTAB__EMPTY, SWTAB__EMPTY,
    SWTAB__EMPTY, SWTAB__EMPTY, SWTAB__EMPTY, SWTAB__EMPTY,
};

static inline swtab_hash_t swtab__h1(swtab_hash_t hash) { return hash >> 7; }
static inline uint8_t swtab__h2(swtab_hash_t hash) { return hash & 0x7F; }

static inline size_t
swtab__slot_pos(size_t group_index, size_t slot)
{
    return group_index * 8 + slot;
}

static inline size_t
swtab__group_index(const swtab *st, swtab_hash_t hash)
{
    return swtab__h1(hash) & st->group_mask;
}

static inline size_t
swtab__next_group_index(const swtab *st, size_t index, size_t probe)
{
    return (index + probe + 1) & st->group_mask;
}

static inline uint64_t
swtab__load_ctrl(const swtab *st, size_t group_index)
{
    uint64_t ctrl;
    memcpy(&ctrl, &st->ctrl[group_index * 8], 8);
    return ctrl;
}

static inline size_t
swtab__match_slot(uint64_t mask)
{
    return __builtin_ctzll(mask) / 8;
}

static inline uint64_t
swtab__ctrl_available(uint64_t ctrl)
{
    return ctrl & SWTAB__HIGH_BITS;
}

static inline uint64_t
swtab__ctrl_occupied(uint64_t ctrl)
{
    return ~ctrl & SWTAB__HIGH_BITS;
}

static inline uint64_t
swtab__ctrl_match(uint64_t ctrl, uint8_t h2)
{
    uint64_t match = ctrl ^ (SWTAB__BROADCAST_BYTE * h2);
    match = (match - SWTAB__BROADCAST_BYTE) & ~match & SWTAB__HIGH_BITS;
    return match;
}

static inline size_t
swtab__ctrl_next_match(uint64_t *match)
{
    size_t slot = swtab__match_slot(*match);
    *match &= *match - 1;
    return slot;
}

static inline uint64_t
swtab__group_has_empty(uint64_t ctrl)
{
    return (ctrl & ~(ctrl << 1)) & SWTAB__HIGH_BITS;
}

#define SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) \
    for (uint64_t ctrl = 0, index##_init_ = 1; index##_init_; index##_init_ = 0) \
    for (size_t index = swtab__group_index(st, hash), index##_probe_ = 0; \
         index##_probe_ <= (st)->group_mask && (ctrl = swtab__load_ctrl(st, index), 1); \
         index = swtab__next_group_index(st, index, index##_probe_++))

static inline void
swtab__alloc(swtab *st, size_t cap)
{
    size_t slots_off = cap;
    char *mem = malloc(slots_off + cap * sizeof(void *));
    st->ctrl = (int8_t *)mem;
    memset(st->ctrl, SWTAB__EMPTY, cap);
    st->slots = (void **)(mem + slots_off);
}

static inline void
swtab__grow_to(swtab *st, size_t new_groups)
{
    size_t old_cap = (st->group_mask + 1) * 8;
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    bool was_empty = (st->ctrl == swtab__empty_ctrl);

    size_t new_cap = new_groups * 8;

    swtab__alloc(st, new_cap);
    st->group_mask = new_groups - 1;
    st->size = 0;
    st->growth_left = new_cap * SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN;

    if (!was_empty) {
        size_t old_groups_n = old_cap / 8;
        for (size_t g = 0; g < old_groups_n; g++) {
            uint64_t ctrl;
            memcpy(&ctrl, &old_ctrl[g * 8], 8);
            uint64_t occ = swtab__ctrl_occupied(ctrl);
            while (occ) {
                size_t pos = swtab__slot_pos(g, swtab__ctrl_next_match(&occ));
                swtab_insert(st, old_slots[pos], st->hash_fn(old_slots[pos]));
            }
        }
        free(old_ctrl);
    }
}

static inline void
swtab__grow(swtab *st)
{
    bool was_empty = (st->ctrl == swtab__empty_ctrl);
    size_t new_groups = was_empty ? 1 : (st->group_mask + 1) * 2;
    swtab__grow_to(st, new_groups);
}

/* ===========================================================================
 *                             IMPLEMENTATION
 * =========================================================================== */

static inline void
swtab_init(swtab *st, swtab_hash_fn hash_fn)
{
    st->ctrl = (int8_t *)swtab__empty_ctrl;
    st->slots = NULL;
    st->hash_fn = hash_fn;
    st->size = 0;
    st->group_mask = 0;
    st->growth_left = 0;
}

static inline void
swtab_destroy(swtab *st)
{
    if (st->ctrl != swtab__empty_ctrl) {
        free(st->ctrl);
    }
    swtab_hash_fn fn = st->hash_fn;
    swtab_init(st, fn);
}

static inline size_t
swtab_size(const swtab *st)
{
    return st->size;
}

static inline bool
swtab_is_empty(const swtab *st)
{
    return st->size == 0;
}

static inline void
swtab_clear(swtab *st)
{
    if (st->ctrl != swtab__empty_ctrl) {
        size_t cap = (st->group_mask + 1) * 8;
        memset(st->ctrl, SWTAB__EMPTY, cap);
        st->size = 0;
        st->growth_left = cap * SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN;
    }
}

static inline void
swtab_reserve(swtab *st, size_t count)
{
    if (count <= st->size + st->growth_left) {
        return;
    }

    bool was_empty = (st->ctrl == swtab__empty_ctrl);
    size_t new_groups = was_empty ? 1 : (st->group_mask + 1) * 2;
    while (new_groups * 8 * SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN < count) {
        new_groups *= 2;
    }
    swtab__grow_to(st, new_groups);
}

static inline void
swtab_insert(swtab *st, void *entry, swtab_hash_t hash)
{
    if (st->growth_left == 0) {
        swtab__grow(st);
    }

    uint8_t h2 = swtab__h2(hash);
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t available = swtab__ctrl_available(ctrl);
        if (available) {
            size_t pos = swtab__slot_pos(index, swtab__match_slot(available));
            bool was_empty = (st->ctrl[pos] == SWTAB__EMPTY);
            st->ctrl[pos] = (int8_t)h2;
            st->slots[pos] = entry;
            st->size++;
            if (was_empty) {
                st->growth_left--;
            }
            return;
        }
    }
}

static inline void
swtab_remove(swtab *st, const void *entry, swtab_hash_t hash)
{
    uint8_t h2 = swtab__h2(hash);
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t match = swtab__ctrl_match(ctrl, h2);
        while (match) {
            size_t pos = swtab__slot_pos(index, swtab__ctrl_next_match(&match));
            if (st->slots[pos] == entry) {
                uint64_t empty = swtab__group_has_empty(ctrl);
                st->ctrl[pos] = empty ? SWTAB__EMPTY : SWTAB__DELETED;
                st->slots[pos] = NULL;
                st->size--;
                if (!empty && st->growth_left > 0) {
                    st->growth_left--;
                }
                return;
            }
        }
        if (swtab__group_has_empty(ctrl)) {
            return;
        }
    }
}

static inline void *
swtab_find(const swtab *st, swtab_hash_t hash)
{
    uint8_t h2 = swtab__h2(hash);
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t match = swtab__ctrl_match(ctrl, h2);
        while (match) {
            size_t pos = swtab__slot_pos(index, swtab__ctrl_next_match(&match));
            if (st->hash_fn(st->slots[pos]) == hash) {
                return st->slots[pos];
            }
        }

        if (swtab__group_has_empty(ctrl)) {
            return NULL;
        }
    }
    return NULL;
}

static inline void *
swtab_find_next(const swtab *st, swtab_hash_t hash, const void *prev)
{
    uint8_t h2 = swtab__h2(hash);
    bool found_prev = false;
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t match = swtab__ctrl_match(ctrl, h2);
        while (match) {
            size_t pos = swtab__slot_pos(index, swtab__ctrl_next_match(&match));
            if (!found_prev) {
                if (st->slots[pos] == prev) {
                    found_prev = true;
                }
                continue;
            }
            if (st->hash_fn(st->slots[pos]) == hash) {
                return st->slots[pos];
            }
        }

        if (swtab__group_has_empty(ctrl)) {
            return NULL;
        }
    }
    return NULL;
}

#endif
