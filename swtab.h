#ifndef SWTAB_H
#define SWTAB_H

#define SWTAB_VERSION_MAJOR 0
#define SWTAB_VERSION_MINOR 1
#define SWTAB_VERSION_PATCH 0

#if !defined(__GNUC__) && !defined(__clang__)
#error "requires GCC or Clang"
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "requires little-endian byte order"
#endif
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
 * The swiss layout resizes when occupancy exceeds NUM/DEN of capacity.
 * Default is 7/8 (87.5%). Define both macros before including this
 * header to override. Dense mode uses an internal 3/4 load factor.
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

/* SWTAB_DENSE_THRESHOLD - Maximum entries kept in dense mode.
 *
 * Tables start as a simple open-addressed table. Once an insert would
 * exceed this threshold, the table promotes to the swiss layout. Define
 * as 0 to disable dense mode.
 */
#ifndef SWTAB_DENSE_THRESHOLD
#define SWTAB_DENSE_THRESHOLD 2048
#endif

/* SWTAB_DENSE_STORE_HASHES / SWTAB_SWISS_STORE_HASHES - Hash caching.
 *
 * Define as 1 to store one full hash per slot in that layout, or 0 to
 * recompute hashes from entries when needed. Dense mode stores hashes by
 * default; swiss mode does not.
 */
#ifndef SWTAB_DENSE_STORE_HASHES
#define SWTAB_DENSE_STORE_HASHES 1
#endif
#ifndef SWTAB_SWISS_STORE_HASHES
#define SWTAB_SWISS_STORE_HASHES 0
#endif

#if SWTAB_LOAD_FACTOR_NUM < 1
#error "SWTAB_LOAD_FACTOR_NUM must be at least 1"
#endif
#if SWTAB_LOAD_FACTOR_DEN < 1
#error "SWTAB_LOAD_FACTOR_DEN must be at least 1"
#endif
#if SWTAB_LOAD_FACTOR_NUM > SWTAB_LOAD_FACTOR_DEN
#error "SWTAB_LOAD_FACTOR_NUM must be <= SWTAB_LOAD_FACTOR_DEN"
#endif
#if SWTAB_DENSE_THRESHOLD < 0
#error "SWTAB_DENSE_THRESHOLD must be >= 0"
#endif
#if SWTAB_DENSE_STORE_HASHES != 0 && SWTAB_DENSE_STORE_HASHES != 1
#error "SWTAB_DENSE_STORE_HASHES must be 0 or 1"
#endif
#if SWTAB_SWISS_STORE_HASHES != 0 && SWTAB_SWISS_STORE_HASHES != 1
#error "SWTAB_SWISS_STORE_HASHES must be 0 or 1"
#endif
#if SWTAB_LOAD_FACTOR_DEN / 8 > SWTAB_LOAD_FACTOR_NUM || \
    (SWTAB_LOAD_FACTOR_DEN / 8 == SWTAB_LOAD_FACTOR_NUM && \
     SWTAB_LOAD_FACTOR_DEN % 8 != 0)
#error "SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN must be at least 1/8"
#endif

/* SWTAB_MALLOC / SWTAB_FREE - Allocation hooks.
 *
 * Defaults to malloc/free. Define both macros before including this
 * header to override.
 */
#ifndef SWTAB_MALLOC
#define SWTAB_MALLOC malloc
#endif
#ifndef SWTAB_FREE
#define SWTAB_FREE free
#endif

/* SWTAB_OOM - Out-of-memory hook.
 *
 * Called when allocation fails or size arithmetic overflows. Defaults
 * to abort(). If overridden, it should not return normally; swtab will
 * abort if it does.
 */
#ifndef SWTAB_OOM
#define SWTAB_OOM() abort()
#endif

/* swtab_hash_t - Hash value type (size_t).
 *
 * swtab_hash_fn - Hash function signature.
 *
 * Must return the same value for a given entry for the lifetime of the
 * table. The table may call this during find, find_next, and resize when
 * hash storage is disabled for the active layout.
 *
 *     swtab_hash_t my_hash(const void *entry) {
 *         const struct my_obj *obj = entry;
 *         return some_hash(obj->key, obj->key_len);
 *     }
 */
typedef size_t swtab_hash_t;
typedef swtab_hash_t (*swtab_hash_fn)(const void *entry);

/* swtab_key_eq_fn - Key equality function signature.
 *
 * Compares a table entry with a lookup key. Return true when the entry
 * matches the key. Key equality must be compatible with the lookup hash:
 * when eq_fn(entry, key) is true, hash_fn(entry) must equal the hash
 * passed to swtab_find_key(). Used by swtab_find_key() to resolve hash
 * collisions.
 *
 *     bool my_eq(const void *entry, const void *key) {
 *         const struct my_obj *obj = entry;
 *         const char *name = key;
 *         return strcmp(obj->name, name) == 0;
 *     }
 */
typedef bool (*swtab_key_eq_fn)(const void *entry, const void *key);

/* swtab - A dense-to-swiss hash map.
 *
 * Stores non-NULL void pointers to caller-owned entries. Entries are
 * located by hash; the caller provides a hash function that can
 * recompute the hash from an entry pointer. NULL entries are not
 * supported because NULL is used as the lookup miss result.
 * Tables use a simple dense open-addressed layout up to
 * SWTAB_DENSE_THRESHOLD entries, then promote to the swiss layout.
 * Full-hash caching can be configured per layout with
 * SWTAB_DENSE_STORE_HASHES and SWTAB_SWISS_STORE_HASHES.
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
    swtab_hash_t *hashes; /* cached full hashes, if enabled */
    swtab_hash_fn hash_fn; /* rehash entries on resize */
    size_t size;          /* number of occupied slots */
    size_t group_mask;    /* num_groups - 1, for H1 & group_mask */
    size_t growth_left;   /* inserts remaining before resize */
    bool dense;           /* using the dense layout */
} swtab;

/* swtab_init - Initialize a table.
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
 * The caller must provide a pre-computed hash. The entry pointer must
 * be non-NULL. Each entry pointer may have at most one membership in a
 * table; inserting the same entry pointer again while it is already
 * present is unsupported. Distinct entries with the same hash may
 * coexist and can be visited with swtab_find_next(). The entry pointer
 * must remain valid for the lifetime of its membership in the table.
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
 * Returns the first entry whose hash matches, or NULL if none. This is
 * a hash-only lookup: callers that need key equality must either check
 * candidate entries with swtab_find_next() or use swtab_find_key().
 * When multiple entries share a hash, use swtab_find_next() to iterate
 * through them.
 *
 *     void *obj = swtab_find(&st, hash);
 *     if (obj) {
 *         printf("found: %s\n", ((struct my_obj *)obj)->name);
 *     }
 */
static inline void *
swtab_find(const swtab *st, swtab_hash_t hash);

/* swtab_find_key - Look up an entry by hash and key equality.
 *
 * Returns the first entry whose hash matches and for which eq_fn(entry,
 * key) returns true, or NULL if none. Use swtab_find_key_next() to
 * continue through additional key-equal entries. Key equality is the
 * final candidate check; swtab_find_key() does not recompute every
 * candidate's full hash. This is the key-aware version of swtab_find();
 * use swtab_find() when hash equality alone is enough or when the caller
 * wants to iterate all same-hash candidates manually.
 *
 *     const char *name = "foo";
 *     swtab_hash_t hash = hash_name(name);
 *     struct my_obj *obj = swtab_find_key(&st, hash, name, my_eq);
 */
static inline void *
swtab_find_key(const swtab *st, swtab_hash_t hash, const void *key,
               swtab_key_eq_fn eq_fn);

/* swtab_find_key_next - Continue a key-aware lookup.
 *
 * Returns the next entry after 'prev' whose hash matches and for which
 * eq_fn(entry, key) returns true, or NULL if there are no more. 'prev'
 * must be a pointer previously returned by swtab_find_key() or
 * swtab_find_key_next() for the same hash and key. This is the
 * key-aware counterpart to swtab_find_next(). Uses the same
 * hash/equality compatibility contract as swtab_find_key().
 *
 *     swtab_hash_t h = hash_name(name);
 *     for (void *e = swtab_find_key(&st, h, name, my_eq);
 *          e;
 *          e = swtab_find_key_next(&st, h, name, my_eq, e)) {
 *         process(e);
 *     }
 */
static inline void *
swtab_find_key_next(const swtab *st, swtab_hash_t hash, const void *key,
                    swtab_key_eq_fn eq_fn, const void *prev);

/* swtab_find_next - Continue a lookup after swtab_find().
 *
 * Returns the next entry with the same hash after 'prev', or NULL if
 * there are no more. 'prev' must be a pointer previously returned by
 * swtab_find() or swtab_find_next() for the same hash. For key-aware
 * lookup, prefer swtab_find_key() and swtab_find_key_next().
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
 * 'var' is declared as void * in the loop scope. Entries must be
 * non-NULL; NULL is used internally as the loop sentinel. Iteration
 * order is arbitrary and not related to insertion order. Do not insert
 * or remove entries during iteration.
 *
 * Scans control groups using SWAR and visits occupied slots. Iteration
 * cost is affected by allocated table capacity and entry distribution.
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

#define SWTAB__LIKELY(x)   __builtin_expect(!!(x), 1)
#define SWTAB__UNLIKELY(x) __builtin_expect(!!(x), 0)

enum {
    SWTAB__EMPTY   = (int8_t)0x80,
    SWTAB__DELETED = (int8_t)0xFE,
    SWTAB__DENSE_LOAD_NUM = 3,
    SWTAB__DENSE_LOAD_DEN = 4,
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

static inline uint64_t
swtab__ctrl_deleted(uint64_t ctrl)
{
    uint64_t match = ctrl ^ (SWTAB__BROADCAST_BYTE * (uint8_t)SWTAB__DELETED);
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

static inline bool
swtab__is_allocated(const swtab *st)
{
    return st->slots != NULL;
}

static inline bool
swtab__is_dense(const swtab *st)
{
    return st->dense;
}

static inline void
swtab__oom(void)
{
    SWTAB_OOM();
    abort();
}

static inline size_t
swtab__checked_add(size_t a, size_t b)
{
    if (a > (size_t)-1 - b) {
        swtab__oom();
    }
    return a + b;
}

static inline size_t
swtab__checked_mul(size_t a, size_t b)
{
    if (a != 0 && b > (size_t)-1 / a) {
        swtab__oom();
    }
    return a * b;
}

static inline size_t
swtab__capacity_from_groups(size_t groups)
{
    return swtab__checked_mul(groups, 8);
}

static inline size_t
swtab__growth_left_for_cap(size_t cap)
{
    size_t q = cap / SWTAB_LOAD_FACTOR_DEN;
    size_t r = cap % SWTAB_LOAD_FACTOR_DEN;
    size_t whole = swtab__checked_mul(q, SWTAB_LOAD_FACTOR_NUM);
    size_t rem = swtab__checked_mul(r, SWTAB_LOAD_FACTOR_NUM) /
                 SWTAB_LOAD_FACTOR_DEN;
    return swtab__checked_add(whole, rem);
}

static inline size_t
swtab__dense_growth_left_for_cap(size_t cap)
{
    size_t q = cap / SWTAB__DENSE_LOAD_DEN;
    size_t r = cap % SWTAB__DENSE_LOAD_DEN;
    size_t whole = swtab__checked_mul(q, SWTAB__DENSE_LOAD_NUM);
    size_t rem = swtab__checked_mul(r, SWTAB__DENSE_LOAD_NUM) /
                 SWTAB__DENSE_LOAD_DEN;
    return swtab__checked_add(whole, rem);
}

static inline size_t
swtab__dense_capacity(const swtab *st)
{
    return (st->group_mask + 1) * 8;
}

static inline size_t
swtab__dense_mask(const swtab *st)
{
    return (st->group_mask << 3) | 7;
}

static inline swtab_hash_t
swtab__slot_hash(const swtab *st, size_t pos)
{
    if (st->hashes != NULL) {
        return st->hashes[pos];
    }
    return st->hash_fn(st->slots[pos]);
}

static inline void
swtab__set_slot_hash(swtab *st, size_t pos, swtab_hash_t hash)
{
    if (st->hashes != NULL) {
        st->hashes[pos] = hash;
    }
}

static inline size_t
swtab__dense_growth_left(const swtab *st)
{
    return swtab__dense_growth_left_for_cap(swtab__dense_capacity(st)) -
           st->size;
}

static inline size_t
swtab__dense_cap_for_count(size_t count)
{
    size_t cap = 8;
    while (swtab__dense_growth_left_for_cap(cap) < count) {
        cap = swtab__checked_mul(cap, 2);
    }
    return cap;
}

static inline size_t
swtab__groups_for_count(size_t count, size_t groups)
{
    while (swtab__growth_left_for_cap(
               swtab__capacity_from_groups(groups)) < count) {
        groups = swtab__checked_mul(groups, 2);
    }
    return groups;
}

#define SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) \
    for (uint64_t ctrl = 0, index##_init_ = 1; index##_init_; index##_init_ = 0) \
    for (size_t index = swtab__group_index(st, hash), index##_probe_ = 0; \
         index##_probe_ <= (st)->group_mask && (ctrl = swtab__load_ctrl(st, index), 1); \
         index = swtab__next_group_index(st, index, index##_probe_++))

static inline void
swtab__swiss_alloc(swtab *st, size_t cap)
{
    size_t slots_off = cap;
#if SWTAB_SWISS_STORE_HASHES
    size_t hashes_off = cap;
    size_t hashes_bytes = swtab__checked_mul(cap, sizeof(swtab_hash_t));
    slots_off = swtab__checked_add(hashes_off, hashes_bytes);
#endif
    size_t slots_bytes = swtab__checked_mul(cap, sizeof(void *));
    size_t alloc_size = swtab__checked_add(slots_off, slots_bytes);
    char *mem = SWTAB_MALLOC(alloc_size);
    if (!mem) {
        swtab__oom();
    }
    st->ctrl = (int8_t *)mem;
    memset(st->ctrl, SWTAB__EMPTY, cap);
    st->slots = (void **)(mem + slots_off);
#if SWTAB_SWISS_STORE_HASHES
    st->hashes = (swtab_hash_t *)(mem + hashes_off);
#else
    st->hashes = NULL;
#endif
    st->dense = false;
}

static inline void
swtab__dense_alloc(swtab *st, size_t cap)
{
    size_t slots_off = cap;
#if SWTAB_DENSE_STORE_HASHES
    size_t hashes_off = cap;
    size_t hashes_bytes = swtab__checked_mul(cap, sizeof(swtab_hash_t));
    slots_off = swtab__checked_add(hashes_off, hashes_bytes);
#endif
    size_t slots_bytes = swtab__checked_mul(cap, sizeof(void *));
    size_t alloc_size = swtab__checked_add(slots_off, slots_bytes);
    char *mem = SWTAB_MALLOC(alloc_size);
    if (!mem) {
        swtab__oom();
    }
    st->ctrl = (int8_t *)mem;
    memset(st->ctrl, SWTAB__EMPTY, cap);
#if SWTAB_DENSE_STORE_HASHES
    st->hashes = (swtab_hash_t *)(mem + hashes_off);
#else
    st->hashes = NULL;
#endif
    st->slots = (void **)(mem + slots_off);
    st->group_mask = cap / 8 - 1;
    st->size = 0;
    st->growth_left = swtab__dense_growth_left_for_cap(cap);
    st->dense = true;
}

static inline void
swtab__swiss_grow_to(swtab *st, size_t new_groups)
{
    size_t old_groups = swtab__checked_add(st->group_mask, 1);
    size_t old_cap = swtab__capacity_from_groups(old_groups);
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    swtab_hash_t *old_hashes = st->hashes;
    bool was_allocated = swtab__is_allocated(st);

    size_t new_cap = swtab__capacity_from_groups(new_groups);

    swtab__swiss_alloc(st, new_cap);
    st->group_mask = new_groups - 1;
    st->size = 0;
    st->growth_left = swtab__growth_left_for_cap(new_cap);

    if (was_allocated) {
        size_t old_groups_n = old_cap / 8;
        for (size_t g = 0; g < old_groups_n; g++) {
            uint64_t ctrl;
            memcpy(&ctrl, &old_ctrl[g * 8], 8);
            uint64_t occ = swtab__ctrl_occupied(ctrl);
            while (occ) {
                size_t pos = swtab__slot_pos(g, swtab__ctrl_next_match(&occ));
                swtab_hash_t hash = old_hashes != NULL ?
                    old_hashes[pos] : st->hash_fn(old_slots[pos]);
                swtab_insert(st, old_slots[pos], hash);
            }
        }
        SWTAB_FREE(old_ctrl);
    }
}

static inline void
swtab__swiss_grow(swtab *st)
{
    bool was_allocated = swtab__is_allocated(st);
    size_t new_groups;
    if (!was_allocated) {
        new_groups = 1;
    } else {
        size_t old_groups = swtab__checked_add(st->group_mask, 1);
        new_groups = swtab__checked_mul(old_groups, 2);
    }
    swtab__swiss_grow_to(st, new_groups);
}

static inline void
swtab__dense_insert_no_grow(swtab *st, void *entry, swtab_hash_t hash)
{
    size_t mask = swtab__dense_mask(st);
    size_t pos = hash & mask;
    while (st->ctrl[pos] != SWTAB__EMPTY) {
        pos = (pos + 1) & mask;
    }
    st->ctrl[pos] = (int8_t)swtab__h2(hash);
    swtab__set_slot_hash(st, pos, hash);
    st->slots[pos] = entry;
    st->size++;
    st->growth_left = swtab__dense_growth_left(st);
}

static inline void
swtab__dense_grow_to(swtab *st, size_t new_cap)
{
    size_t old_cap = swtab__dense_capacity(st);
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    swtab_hash_t *old_hashes = st->hashes;

    swtab__dense_alloc(st, new_cap);
    for (size_t i = 0; i < old_cap; i++) {
        if (old_ctrl[i] != SWTAB__EMPTY) {
            swtab_hash_t hash = old_hashes != NULL ?
                old_hashes[i] : st->hash_fn(old_slots[i]);
            swtab__dense_insert_no_grow(st, old_slots[i], hash);
        }
    }
    SWTAB_FREE(old_ctrl);
}

static inline void
swtab__dense_grow_for_count(swtab *st, size_t count)
{
    swtab__dense_grow_to(st, swtab__dense_cap_for_count(count));
}

static inline void
swtab__promote_to_swiss(swtab *st, size_t count)
{
    size_t old_cap = swtab__dense_capacity(st);
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    swtab_hash_t *old_hashes = st->hashes;

    size_t new_groups = swtab__groups_for_count(count, 1);
    swtab__swiss_alloc(st, swtab__capacity_from_groups(new_groups));
    st->group_mask = new_groups - 1;
    st->size = 0;
    st->growth_left = swtab__growth_left_for_cap(
        swtab__capacity_from_groups(new_groups));

    for (size_t i = 0; i < old_cap; i++) {
        if (old_ctrl[i] != SWTAB__EMPTY) {
            swtab_hash_t hash = old_hashes != NULL ?
                old_hashes[i] : st->hash_fn(old_slots[i]);
            swtab_insert(st, old_slots[i], hash);
        }
    }
    SWTAB_FREE(old_ctrl);
}

static inline void
swtab__dense_erase_at(swtab *st, size_t hole)
{
    size_t mask = swtab__dense_mask(st);
    size_t pos = (hole + 1) & mask;

    while (st->ctrl[pos] != SWTAB__EMPTY) {
        size_t ideal = swtab__slot_hash(st, pos) & mask;
        if (((hole - ideal) & mask) < ((pos - ideal) & mask)) {
            st->ctrl[hole] = st->ctrl[pos];
            if (st->hashes != NULL) {
                st->hashes[hole] = st->hashes[pos];
            }
            st->slots[hole] = st->slots[pos];
            hole = pos;
        }
        pos = (pos + 1) & mask;
    }

    st->ctrl[hole] = SWTAB__EMPTY;
    st->slots[hole] = NULL;
    if (st->hashes != NULL) {
        st->hashes[hole] = 0;
    }
    st->size--;
    st->growth_left = swtab__dense_growth_left(st);
}

/* ===========================================================================
 *                             IMPLEMENTATION
 * =========================================================================== */

static inline void
swtab_init(swtab *st, swtab_hash_fn hash_fn)
{
    st->ctrl = (int8_t *)swtab__empty_ctrl;
    st->slots = NULL;
    st->hashes = NULL;
    st->hash_fn = hash_fn;
    st->size = 0;
    st->group_mask = 0;
    st->growth_left = 0;
    st->dense = false;
}

static inline void
swtab_destroy(swtab *st)
{
    if (swtab__is_allocated(st)) {
        SWTAB_FREE(st->ctrl);
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
    if (swtab__is_dense(st)) {
        size_t cap = swtab__dense_capacity(st);
        memset(st->ctrl, SWTAB__EMPTY, cap);
        st->size = 0;
        st->growth_left = swtab__dense_growth_left_for_cap(cap);
    } else if (swtab__is_allocated(st)) {
        size_t cap = swtab__capacity_from_groups(
            swtab__checked_add(st->group_mask, 1));
        memset(st->ctrl, SWTAB__EMPTY, cap);
        st->size = 0;
        st->growth_left = swtab__growth_left_for_cap(cap);
    }
}

static inline void
swtab_reserve(swtab *st, size_t count)
{
    if (SWTAB_DENSE_THRESHOLD != 0 && count <= SWTAB_DENSE_THRESHOLD &&
        !swtab__is_allocated(st)) {
        if (count == 0) {
            return;
        }
        swtab__dense_alloc(st, swtab__dense_cap_for_count(count));
        return;
    }

    if (swtab__is_dense(st)) {
        if (count <= SWTAB_DENSE_THRESHOLD) {
            if (count <= st->size + st->growth_left) {
                return;
            }
            swtab__dense_grow_for_count(st, count);
            return;
        }
        swtab__promote_to_swiss(st, count);
        return;
    }

    if (count <= st->size + st->growth_left) {
        return;
    }

    bool was_allocated = swtab__is_allocated(st);
    size_t new_groups;
    if (!was_allocated) {
        new_groups = 1;
    } else {
        new_groups = swtab__checked_mul(
            swtab__checked_add(st->group_mask, 1), 2);
    }
    new_groups = swtab__groups_for_count(count, new_groups);
    swtab__swiss_grow_to(st, new_groups);
}

static inline void
swtab_insert(swtab *st, void *entry, swtab_hash_t hash)
{
    if (swtab__is_dense(st)) {
        size_t count = swtab__checked_add(st->size, 1);
        if (SWTAB__UNLIKELY(count > SWTAB_DENSE_THRESHOLD)) {
            swtab__promote_to_swiss(st, count);
        } else {
            if (SWTAB__UNLIKELY(st->growth_left == 0)) {
                swtab__dense_grow_for_count(st, count);
            }
            swtab__dense_insert_no_grow(st, entry, hash);
            return;
        }
    } else if (SWTAB__UNLIKELY(!swtab__is_allocated(st)) &&
               SWTAB_DENSE_THRESHOLD != 0) {
        swtab__dense_alloc(st, swtab__dense_cap_for_count(1));
        swtab__dense_insert_no_grow(st, entry, hash);
        return;
    }

    uint8_t h2 = swtab__h2(hash);

    if (SWTAB__UNLIKELY(st->growth_left == 0)) {
        SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t deleted = swtab__ctrl_deleted(ctrl);
            if (deleted) {
                size_t pos = swtab__slot_pos(index, swtab__match_slot(deleted));
                st->ctrl[pos] = (int8_t)h2;
                swtab__set_slot_hash(st, pos, hash);
                st->slots[pos] = entry;
                st->size++;
                return;
            }

            if (swtab__group_has_empty(ctrl)) {
                break;
            }
        }
        swtab__swiss_grow(st);
    }

    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t available = swtab__ctrl_available(ctrl);
        if (SWTAB__LIKELY(available)) {
            size_t pos = swtab__slot_pos(index, swtab__match_slot(available));
            bool was_empty = (st->ctrl[pos] == SWTAB__EMPTY);
            st->ctrl[pos] = (int8_t)h2;
            swtab__set_slot_hash(st, pos, hash);
            st->slots[pos] = entry;
            st->size++;
            if (SWTAB__LIKELY(was_empty)) {
                st->growth_left--;
            }
            return;
        }
    }
}

static inline void
swtab_remove(swtab *st, const void *entry, swtab_hash_t hash)
{
    if (swtab__is_dense(st)) {
        size_t mask = swtab__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != SWTAB__EMPTY) {
            if (swtab__slot_hash(st, pos) == hash &&
                st->slots[pos] == entry) {
                swtab__dense_erase_at(st, pos);
                return;
            }
            pos = (pos + 1) & mask;
        }
        return;
    }

    uint8_t h2 = swtab__h2(hash);
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t match = swtab__ctrl_match(ctrl, h2);
        while (match) {
            size_t pos = swtab__slot_pos(index, swtab__ctrl_next_match(&match));
            if ((st->hashes == NULL || st->hashes[pos] == hash) &&
                st->slots[pos] == entry) {
                uint64_t empty = swtab__group_has_empty(ctrl);
                st->ctrl[pos] = empty ? SWTAB__EMPTY : SWTAB__DELETED;
                if (st->hashes != NULL) {
                    st->hashes[pos] = 0;
                }
                st->slots[pos] = NULL;
                st->size--;
                if (empty) {
                    st->growth_left++;
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
    if (swtab__is_dense(st)) {
        size_t mask = swtab__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != SWTAB__EMPTY) {
            if (swtab__slot_hash(st, pos) == hash) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    }

    uint8_t h2 = swtab__h2(hash);
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t match = swtab__ctrl_match(ctrl, h2);
        while (match) {
            size_t pos = swtab__slot_pos(index, swtab__ctrl_next_match(&match));
            if (swtab__slot_hash(st, pos) == hash) {
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
swtab_find_key(const swtab *st, swtab_hash_t hash, const void *key,
               swtab_key_eq_fn eq_fn)
{
    if (swtab__is_dense(st)) {
        int8_t h2 = (int8_t)swtab__h2(hash);
        size_t mask = swtab__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != SWTAB__EMPTY) {
            if ((st->hashes != NULL ? st->hashes[pos] == hash :
                 st->ctrl[pos] == h2) &&
                eq_fn(st->slots[pos], key)) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    }

    uint8_t h2 = swtab__h2(hash);
    SWTAB__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t match = swtab__ctrl_match(ctrl, h2);
        while (match) {
            size_t pos = swtab__slot_pos(index, swtab__ctrl_next_match(&match));
            if ((st->hashes == NULL || st->hashes[pos] == hash) &&
                eq_fn(st->slots[pos], key)) {
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
swtab_find_key_next(const swtab *st, swtab_hash_t hash, const void *key,
                    swtab_key_eq_fn eq_fn, const void *prev)
{
    if (swtab__is_dense(st)) {
        bool found_prev = false;
        int8_t h2 = (int8_t)swtab__h2(hash);
        size_t mask = swtab__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != SWTAB__EMPTY) {
            if (!found_prev) {
                if (st->slots[pos] == prev) {
                    found_prev = true;
                }
            } else if ((st->hashes != NULL ? st->hashes[pos] == hash :
                        st->ctrl[pos] == h2) &&
                       eq_fn(st->slots[pos], key)) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    }

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
            if ((st->hashes == NULL || st->hashes[pos] == hash) &&
                eq_fn(st->slots[pos], key)) {
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
    if (swtab__is_dense(st)) {
        bool found_prev = false;
        size_t mask = swtab__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != SWTAB__EMPTY) {
            if (!found_prev) {
                if (st->slots[pos] == prev) {
                    found_prev = true;
                }
            } else if (swtab__slot_hash(st, pos) == hash) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    }

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
            if (swtab__slot_hash(st, pos) == hash) {
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
