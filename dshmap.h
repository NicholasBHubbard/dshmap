#ifndef DSHMAP_H
#define DSHMAP_H

#define DSHMAP_VERSION_MAJOR 0
#define DSHMAP_VERSION_MINOR 1
#define DSHMAP_VERSION_PATCH 0

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

/* DSHMAP_LOAD_FACTOR_NUM / DSHMAP_LOAD_FACTOR_DEN - Maximum load factor.
 *
 * The Swiss layout resizes when occupancy exceeds NUM/DEN of capacity.
 * Default is 7/8 (87.5%). Define both macros before including this
 * header to override. Dense mode uses an internal 3/4 load factor.
 *
 *     #define DSHMAP_LOAD_FACTOR_NUM 3
 *     #define DSHMAP_LOAD_FACTOR_DEN 4
 *     #include "dshmap.h"
 */
#ifndef DSHMAP_LOAD_FACTOR_NUM
#define DSHMAP_LOAD_FACTOR_NUM 7
#endif
#ifndef DSHMAP_LOAD_FACTOR_DEN
#define DSHMAP_LOAD_FACTOR_DEN 8
#endif

/* DSHMAP_DENSE_THRESHOLD - Maximum entries kept in dense mode.
 *
 * Tables start as a simple open-addressed table. Once an insert would
 * exceed this threshold, the table promotes to the Swiss layout. Reserve
 * can also promote the table. Promotion is one-way; clear does not move
 * a table back to dense mode. Define as 0 to disable dense mode.
 */
#ifndef DSHMAP_DENSE_THRESHOLD
#define DSHMAP_DENSE_THRESHOLD 2048
#endif

/* DSHMAP_DENSE_STORE_HASHES / DSHMAP_SWISS_STORE_HASHES - Hash storage.
 *
 * Define as 1 to store one full hash per slot in that layout, or 0 to
 * recompute hashes from entries when needed. Dense mode stores hashes by
 * default; Swiss mode does not.
 */
#ifndef DSHMAP_DENSE_STORE_HASHES
#define DSHMAP_DENSE_STORE_HASHES 1
#endif
#ifndef DSHMAP_SWISS_STORE_HASHES
#define DSHMAP_SWISS_STORE_HASHES 0
#endif

#if DSHMAP_LOAD_FACTOR_NUM < 1
#error "DSHMAP_LOAD_FACTOR_NUM must be at least 1"
#endif
#if DSHMAP_LOAD_FACTOR_DEN < 1
#error "DSHMAP_LOAD_FACTOR_DEN must be at least 1"
#endif
#if DSHMAP_LOAD_FACTOR_NUM > DSHMAP_LOAD_FACTOR_DEN
#error "DSHMAP_LOAD_FACTOR_NUM must be <= DSHMAP_LOAD_FACTOR_DEN"
#endif
#if DSHMAP_DENSE_THRESHOLD < 0
#error "DSHMAP_DENSE_THRESHOLD must be >= 0"
#endif
#if DSHMAP_DENSE_STORE_HASHES != 0 && DSHMAP_DENSE_STORE_HASHES != 1
#error "DSHMAP_DENSE_STORE_HASHES must be 0 or 1"
#endif
#if DSHMAP_SWISS_STORE_HASHES != 0 && DSHMAP_SWISS_STORE_HASHES != 1
#error "DSHMAP_SWISS_STORE_HASHES must be 0 or 1"
#endif
#if DSHMAP_LOAD_FACTOR_DEN / 8 > DSHMAP_LOAD_FACTOR_NUM || \
    (DSHMAP_LOAD_FACTOR_DEN / 8 == DSHMAP_LOAD_FACTOR_NUM && \
     DSHMAP_LOAD_FACTOR_DEN % 8 != 0)
#error "DSHMAP_LOAD_FACTOR_NUM / DSHMAP_LOAD_FACTOR_DEN must be at least 1/8"
#endif

/* DSHMAP_MALLOC / DSHMAP_FREE - Allocation hooks.
 *
 * Defaults to malloc/free. Define both macros before including this
 * header to override. DSHMAP_MALLOC must return memory with the same
 * alignment guarantees as malloc. DSHMAP_FREE must be able to free memory
 * returned by DSHMAP_MALLOC. Do not mix unrelated allocators.
 */
#ifndef DSHMAP_MALLOC
#define DSHMAP_MALLOC malloc
#endif
#ifndef DSHMAP_FREE
#define DSHMAP_FREE free
#endif

/* DSHMAP_OOM - Out-of-memory hook.
 *
 * Called when allocation fails or size arithmetic overflows. Defaults
 * to abort(). If overridden, it should not return normally; dshmap will
 * abort if it does.
 */
#ifndef DSHMAP_OOM
#define DSHMAP_OOM() abort()
#endif

/* dshmap_hash_t - Hash value type (size_t).
 *
 * dshmap_hash_fn - Hash function signature.
 *
 * Must return the same value for a given entry for the lifetime of the
 * table. The table may call this to recompute an entry's hash when full
 * hashes are not stored for the active layout, or were not stored for a
 * layout being resized or promoted.
 *
 *     dshmap_hash_t my_hash(const void *entry) {
 *         const struct my_obj *obj = entry;
 *         return some_hash(obj->key, obj->key_len);
 *     }
 */
typedef size_t dshmap_hash_t;
typedef dshmap_hash_t (*dshmap_hash_fn)(const void *entry);

/* dshmap_key_eq_fn - Key equality function signature.
 *
 * Compares a table entry with a lookup key. Return true when the entry
 * matches the key. Key equality must be compatible with the lookup hash:
 * when eq_fn(entry, key) is true, the entry's full hash must equal the hash
 * passed to dshmap_find_key(). Used by dshmap_find_key() to resolve hash
 * collisions.
 *
 *     bool my_eq(const void *entry, const void *key) {
 *         const struct my_obj *obj = entry;
 *         const char *name = key;
 *         return strcmp(obj->name, name) == 0;
 *     }
 */
typedef bool (*dshmap_key_eq_fn)(const void *entry, const void *key);

/* dshmap - A dense-to-Swiss hash map.
 *
 * Stores non-NULL void pointers to caller-owned entries. Entries are
 * located by hash; the caller provides a hash function that can
 * recompute the hash from an entry pointer. NULL entries are not
 * supported because NULL is used as the lookup miss result.
 * Tables use a simple dense open-addressed layout up to
 * DSHMAP_DENSE_THRESHOLD entries, then promote to the Swiss layout.
 * Full-hash storage can be configured per layout with
 * DSHMAP_DENSE_STORE_HASHES and DSHMAP_SWISS_STORE_HASHES.
 * Promotion is one-way. A promoted table stays Swiss until destroy.
 *
 * The struct fields are visible so the type can be stack allocated, but
 * they are implementation details. Application code should not read or
 * write them directly.
 *
 * dshmap has no internal locking. Use external locking if any thread may
 * mutate the table while another thread can access it.
 *
 * Must be initialized with dshmap_init() before use and cleaned up with
 * dshmap_destroy(). Stack allocation is typical:
 *
 *     dshmap st;
 *     dshmap_init(&st, my_hash);
 */
typedef struct dshmap {
    int8_t *ctrl;         /* H2 tag per slot; empty=0x80, deleted=0xFE */
    void **slots;         /* one entry pointer per slot */
    dshmap_hash_t *hashes; /* stored full hashes, if enabled */
    dshmap_hash_fn hash_fn; /* recompute hashes when needed */
    size_t size;          /* number of occupied slots */
    size_t group_mask;    /* num_groups - 1, for H1 & group_mask */
    size_t growth_left;   /* inserts remaining before resize */
    bool dense;           /* using the dense layout */
} dshmap;

/* dshmap_init - Initialize a table.
 *
 * The caller must call dshmap_destroy() when done. No memory is
 * allocated until the first insert. hash_fn is normally required. It may
 * be NULL only when the compile-time settings guarantee that every active
 * layout stores full hashes. In the default config, pass a real hash
 * function.
 *
 *     dshmap st;
 *     dshmap_init(&st, my_hash);
 *     // ... use st ...
 *     dshmap_destroy(&st);
 */
static inline void
dshmap_init(dshmap *st, dshmap_hash_fn hash_fn);

/* dshmap_destroy - Free all memory owned by the table.
 *
 * Does not free the entries themselves; the caller owns those. The
 * table is reset to its initialized state and may be reused. The table
 * keeps the same hash_fn it was initialized with.
 *
 *     dshmap_destroy(&st);
 *     // st is now empty and valid, as if dshmap_init() was just called
 */
static inline void
dshmap_destroy(dshmap *st);

/* dshmap_size - Return the number of entries in the table.
 *
 *     if (dshmap_size(&st) > 1000) {
 *         // table is large
 *     }
 */
static inline size_t
dshmap_size(const dshmap *st);

/* dshmap_is_empty - Return true if the table contains no entries.
 *
 *     if (dshmap_is_empty(&st)) {
 *         printf("nothing to process\n");
 *     }
 */
static inline bool
dshmap_is_empty(const dshmap *st);

/* dshmap_clear - Mark all slots as empty.
 *
 * Entry pointers are discarded but not freed; the caller owns those.
 * The table keeps its allocated capacity so subsequent inserts avoid
 * reallocation. clear does not demote a Swiss table back to dense mode.
 *
 *     dshmap_clear(&st);
 *     assert(dshmap_is_empty(&st));
 *     // st retains its capacity, ready for new inserts
 */
static inline void
dshmap_clear(dshmap *st);

/* dshmap_reserve - Pre-allocate capacity for at least 'count' entries.
 *
 * No-op if the table can already hold 'count' entries without resizing.
 * Call this before a batch of inserts to allocate once upfront instead
 * of resizing repeatedly as the table grows. Reserving more than
 * DSHMAP_DENSE_THRESHOLD entries promotes the table to Swiss mode.
 * Promotion is one-way.
 *
 *     dshmap_reserve(&st, n);
 *     for (size_t i = 0; i < n; i++) {
 *         dshmap_insert(&st, entries[i], hashes[i]);
 *     }
 */
static inline void
dshmap_reserve(dshmap *st, size_t count);

/* dshmap_insert - Insert an entry into the table.
 *
 * The caller must provide a precomputed full hash for entry. If dshmap
 * ever calls hash_fn(entry), it must return the same hash. The entry
 * pointer must be non-NULL. Each entry pointer may have at most one
 * membership in a table; inserting the same entry pointer again while it
 * is already present is unsupported. Distinct entries with the same hash
 * may coexist and can be visited with dshmap_find_next(). The entry
 * pointer must remain valid for the lifetime of its membership in the
 * table.
 *
 *     struct my_obj *obj = make_obj("foo");
 *     dshmap_insert(&st, obj, my_hash(obj));
 */
static inline void
dshmap_insert(dshmap *st, void *entry, dshmap_hash_t hash);

/* dshmap_remove - Remove an entry from the table.
 *
 * Removes by pointer identity, not by hash equality. The caller must
 * pass the exact pointer that was inserted and the same full hash used
 * for insertion. No-op if the entry is not found. Does not free the
 * entry; the caller owns it.
 *
 *     dshmap_remove(&st, obj, my_hash(obj));
 *     free(obj);
 */
static inline void
dshmap_remove(dshmap *st, const void *entry, dshmap_hash_t hash);

/* dshmap_find - Look up an entry by hash.
 *
 * Returns the first entry whose hash matches, or NULL if none. This is
 * a hash-only lookup: callers that need key equality must either check
 * candidate entries with dshmap_find_next() or use dshmap_find_key().
 * When multiple entries share a hash, use dshmap_find_next() to iterate
 * through them.
 *
 *     void *obj = dshmap_find(&st, hash);
 *     if (obj) {
 *         printf("found: %s\n", ((struct my_obj *)obj)->name);
 *     }
 */
static inline void *
dshmap_find(const dshmap *st, dshmap_hash_t hash);

/* dshmap_find_key - Look up an entry by hash and key equality.
 *
 * Returns the first entry whose hash matches and for which eq_fn(entry,
 * key) returns true, or NULL if none. Use dshmap_find_key_next() to
 * continue through additional key-equal entries. Key equality is the
 * final candidate check; dshmap_find_key() does not recompute every
 * candidate's full hash. This is the key-aware version of dshmap_find();
 * use dshmap_find() when hash equality alone is enough or when the caller
 * wants to iterate all same-hash candidates manually.
 *
 * When full hashes are not stored, eq_fn may be called for entries with
 * the same H2 tag (low 7 hash bits) but a different full hash. eq_fn must
 * compare the real key and return false for non-matching entries.
 *
 *     const char *name = "foo";
 *     dshmap_hash_t hash = hash_name(name);
 *     struct my_obj *obj = dshmap_find_key(&st, hash, name, my_eq);
 */
static inline void *
dshmap_find_key(const dshmap *st, dshmap_hash_t hash, const void *key,
               dshmap_key_eq_fn eq_fn);

/* dshmap_find_key_next - Continue a key-aware lookup.
 *
 * Returns the next entry after 'prev' whose hash matches and for which
 * eq_fn(entry, key) returns true, or NULL if there are no more. 'prev'
 * must be a pointer previously returned by dshmap_find_key() or
 * dshmap_find_key_next() for the same hash and key. This is the
 * key-aware counterpart to dshmap_find_next(). Uses the same
 * hash/equality compatibility contract as dshmap_find_key(). eq_fn may
 * see entries with the same H2 tag when full hashes are not stored.
 *
 *     dshmap_hash_t h = hash_name(name);
 *     for (void *e = dshmap_find_key(&st, h, name, my_eq);
 *          e;
 *          e = dshmap_find_key_next(&st, h, name, my_eq, e)) {
 *         process(e);
 *     }
 */
static inline void *
dshmap_find_key_next(const dshmap *st, dshmap_hash_t hash, const void *key,
                    dshmap_key_eq_fn eq_fn, const void *prev);

/* dshmap_find_next - Continue a lookup after dshmap_find().
 *
 * Returns the next entry with the same hash after 'prev', or NULL if
 * there are no more. 'prev' must be a pointer previously returned by
 * dshmap_find() or dshmap_find_next() for the same hash. For key-aware
 * lookup, prefer dshmap_find_key() and dshmap_find_key_next().
 *
 *     dshmap_hash_t h = my_hash(key);
 *     for (void *e = dshmap_find(&st, h); e; e = dshmap_find_next(&st, h, e)) {
 *         process(e);
 *     }
 */
static inline void *
dshmap_find_next(const dshmap *st, dshmap_hash_t hash, const void *prev);

/* DSHMAP_FOR_EACH - Iterate over all entries in the table.
 *
 * 'var' is declared as void * in the loop scope. Entries must be
 * non-NULL; NULL is used internally as the loop sentinel. Iteration
 * order is arbitrary and not related to insertion order. Do not insert
 * or remove entries during iteration. The 'st' argument is evaluated
 * more than once, so pass a simple table pointer with no side effects.
 * A break statement inside the loop body only breaks one generated inner
 * loop; it does not stop the whole iteration.
 *
 * Scans control groups using SWAR and visits occupied slots. Iteration
 * cost is affected by allocated table capacity and entry distribution.
 *
 *     DSHMAP_FOR_EACH(entry, &st) {
 *         printf("%s\n", ((struct my_obj *)entry)->name);
 *     }
 */
#define DSHMAP_FOR_EACH(var, st) \
    for (size_t var##_g_ = 0; var##_g_ <= (st)->group_mask; var##_g_++) \
    for (uint64_t var##_o_ = dshmap__ctrl_occupied(dshmap__load_ctrl(st, var##_g_)); \
         var##_o_; \
         var##_o_ &= var##_o_ - 1) \
    for (void *var = (st)->slots[dshmap__slot_pos(var##_g_, dshmap__match_slot(var##_o_))]; \
         var; var = NULL)

/* ===========================================================================
 *                                INTERNAL
 * =========================================================================== */

#define DSHMAP__LIKELY(x)   __builtin_expect(!!(x), 1)
#define DSHMAP__UNLIKELY(x) __builtin_expect(!!(x), 0)

enum {
    DSHMAP__EMPTY   = (int8_t)0x80,
    DSHMAP__DELETED = (int8_t)0xFE,
    DSHMAP__DENSE_LOAD_NUM = 3,
    DSHMAP__DENSE_LOAD_DEN = 4,
};

static const uint64_t DSHMAP__BROADCAST_BYTE = 0x0101010101010101ULL;
static const uint64_t DSHMAP__HIGH_BITS      = 0x8080808080808080ULL;

static const int8_t dshmap__empty_ctrl[8] = {
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
};

static inline dshmap_hash_t dshmap__h1(dshmap_hash_t hash) { return hash >> 7; }
static inline uint8_t dshmap__h2(dshmap_hash_t hash) { return hash & 0x7F; }

static inline size_t
dshmap__slot_pos(size_t group_index, size_t slot)
{
    return group_index * 8 + slot;
}

static inline size_t
dshmap__group_index(const dshmap *st, dshmap_hash_t hash)
{
    return dshmap__h1(hash) & st->group_mask;
}

static inline size_t
dshmap__next_group_index(const dshmap *st, size_t index, size_t probe)
{
    return (index + probe + 1) & st->group_mask;
}

static inline uint64_t
dshmap__load_ctrl(const dshmap *st, size_t group_index)
{
    uint64_t ctrl;
    memcpy(&ctrl, &st->ctrl[group_index * 8], 8);
    return ctrl;
}

static inline size_t
dshmap__match_slot(uint64_t mask)
{
    return __builtin_ctzll(mask) / 8;
}

static inline uint64_t
dshmap__ctrl_available(uint64_t ctrl)
{
    return ctrl & DSHMAP__HIGH_BITS;
}

static inline uint64_t
dshmap__ctrl_occupied(uint64_t ctrl)
{
    return ~ctrl & DSHMAP__HIGH_BITS;
}

static inline uint64_t
dshmap__ctrl_match(uint64_t ctrl, uint8_t h2)
{
    uint64_t match = ctrl ^ (DSHMAP__BROADCAST_BYTE * h2);
    match = (match - DSHMAP__BROADCAST_BYTE) & ~match & DSHMAP__HIGH_BITS;
    return match;
}

static inline uint64_t
dshmap__ctrl_deleted(uint64_t ctrl)
{
    uint64_t match = ctrl ^ (DSHMAP__BROADCAST_BYTE * (uint8_t)DSHMAP__DELETED);
    match = (match - DSHMAP__BROADCAST_BYTE) & ~match & DSHMAP__HIGH_BITS;
    return match;
}

static inline size_t
dshmap__ctrl_next_match(uint64_t *match)
{
    size_t slot = dshmap__match_slot(*match);
    *match &= *match - 1;
    return slot;
}

static inline uint64_t
dshmap__group_has_empty(uint64_t ctrl)
{
    return (ctrl & ~(ctrl << 1)) & DSHMAP__HIGH_BITS;
}

static inline bool
dshmap__is_allocated(const dshmap *st)
{
    return st->slots != NULL;
}

static inline bool
dshmap__is_dense(const dshmap *st)
{
    return st->dense;
}

static inline void
dshmap__oom(void)
{
    DSHMAP_OOM();
    abort();
}

static inline size_t
dshmap__checked_add(size_t a, size_t b)
{
    if (a > (size_t)-1 - b) {
        dshmap__oom();
    }
    return a + b;
}

static inline size_t
dshmap__checked_mul(size_t a, size_t b)
{
    if (a != 0 && b > (size_t)-1 / a) {
        dshmap__oom();
    }
    return a * b;
}

static inline size_t
dshmap__capacity_from_groups(size_t groups)
{
    return dshmap__checked_mul(groups, 8);
}

static inline size_t
dshmap__growth_left_for_cap(size_t cap)
{
    size_t q = cap / DSHMAP_LOAD_FACTOR_DEN;
    size_t r = cap % DSHMAP_LOAD_FACTOR_DEN;
    size_t whole = dshmap__checked_mul(q, DSHMAP_LOAD_FACTOR_NUM);
    size_t rem = dshmap__checked_mul(r, DSHMAP_LOAD_FACTOR_NUM) /
                 DSHMAP_LOAD_FACTOR_DEN;
    return dshmap__checked_add(whole, rem);
}

static inline size_t
dshmap__dense_growth_left_for_cap(size_t cap)
{
    size_t q = cap / DSHMAP__DENSE_LOAD_DEN;
    size_t r = cap % DSHMAP__DENSE_LOAD_DEN;
    size_t whole = dshmap__checked_mul(q, DSHMAP__DENSE_LOAD_NUM);
    size_t rem = dshmap__checked_mul(r, DSHMAP__DENSE_LOAD_NUM) /
                 DSHMAP__DENSE_LOAD_DEN;
    return dshmap__checked_add(whole, rem);
}

static inline size_t
dshmap__dense_capacity(const dshmap *st)
{
    return (st->group_mask + 1) * 8;
}

static inline size_t
dshmap__dense_mask(const dshmap *st)
{
    return (st->group_mask << 3) | 7;
}

static inline dshmap_hash_t
dshmap__slot_hash(const dshmap *st, size_t pos)
{
    if (st->hashes != NULL) {
        return st->hashes[pos];
    }
    return st->hash_fn(st->slots[pos]);
}

static inline void
dshmap__set_slot_hash(dshmap *st, size_t pos, dshmap_hash_t hash)
{
    if (st->hashes != NULL) {
        st->hashes[pos] = hash;
    }
}

static inline size_t
dshmap__dense_growth_left(const dshmap *st)
{
    return dshmap__dense_growth_left_for_cap(dshmap__dense_capacity(st)) -
           st->size;
}

static inline size_t
dshmap__dense_cap_for_count(size_t count)
{
    size_t cap = 8;
    while (dshmap__dense_growth_left_for_cap(cap) < count) {
        cap = dshmap__checked_mul(cap, 2);
    }
    return cap;
}

static inline size_t
dshmap__groups_for_count(size_t count, size_t groups)
{
    while (dshmap__growth_left_for_cap(
               dshmap__capacity_from_groups(groups)) < count) {
        groups = dshmap__checked_mul(groups, 2);
    }
    return groups;
}

#define DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) \
    for (uint64_t ctrl = 0, index##_init_ = 1; index##_init_; index##_init_ = 0) \
    for (size_t index = dshmap__group_index(st, hash), index##_probe_ = 0; \
         index##_probe_ <= (st)->group_mask && (ctrl = dshmap__load_ctrl(st, index), 1); \
         index = dshmap__next_group_index(st, index, index##_probe_++))

static inline void
dshmap__swiss_alloc(dshmap *st, size_t cap)
{
    size_t slots_off = cap;
#if DSHMAP_SWISS_STORE_HASHES
    size_t hashes_off = cap;
    size_t hashes_bytes = dshmap__checked_mul(cap, sizeof(dshmap_hash_t));
    slots_off = dshmap__checked_add(hashes_off, hashes_bytes);
#endif
    size_t slots_bytes = dshmap__checked_mul(cap, sizeof(void *));
    size_t alloc_size = dshmap__checked_add(slots_off, slots_bytes);
    char *mem = DSHMAP_MALLOC(alloc_size);
    if (!mem) {
        dshmap__oom();
    }
    st->ctrl = (int8_t *)mem;
    memset(st->ctrl, DSHMAP__EMPTY, cap);
    st->slots = (void **)(mem + slots_off);
#if DSHMAP_SWISS_STORE_HASHES
    st->hashes = (dshmap_hash_t *)(mem + hashes_off);
#else
    st->hashes = NULL;
#endif
    st->dense = false;
}

static inline void
dshmap__dense_alloc(dshmap *st, size_t cap)
{
    size_t slots_off = cap;
#if DSHMAP_DENSE_STORE_HASHES
    size_t hashes_off = cap;
    size_t hashes_bytes = dshmap__checked_mul(cap, sizeof(dshmap_hash_t));
    slots_off = dshmap__checked_add(hashes_off, hashes_bytes);
#endif
    size_t slots_bytes = dshmap__checked_mul(cap, sizeof(void *));
    size_t alloc_size = dshmap__checked_add(slots_off, slots_bytes);
    char *mem = DSHMAP_MALLOC(alloc_size);
    if (!mem) {
        dshmap__oom();
    }
    st->ctrl = (int8_t *)mem;
    memset(st->ctrl, DSHMAP__EMPTY, cap);
#if DSHMAP_DENSE_STORE_HASHES
    st->hashes = (dshmap_hash_t *)(mem + hashes_off);
#else
    st->hashes = NULL;
#endif
    st->slots = (void **)(mem + slots_off);
    st->group_mask = cap / 8 - 1;
    st->size = 0;
    st->growth_left = dshmap__dense_growth_left_for_cap(cap);
    st->dense = true;
}

static inline void
dshmap__swiss_grow_to(dshmap *st, size_t new_groups)
{
    size_t old_groups = dshmap__checked_add(st->group_mask, 1);
    size_t old_cap = dshmap__capacity_from_groups(old_groups);
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    dshmap_hash_t *old_hashes = st->hashes;
    bool was_allocated = dshmap__is_allocated(st);

    size_t new_cap = dshmap__capacity_from_groups(new_groups);

    dshmap__swiss_alloc(st, new_cap);
    st->group_mask = new_groups - 1;
    st->size = 0;
    st->growth_left = dshmap__growth_left_for_cap(new_cap);

    if (was_allocated) {
        size_t old_groups_n = old_cap / 8;
        for (size_t g = 0; g < old_groups_n; g++) {
            uint64_t ctrl;
            memcpy(&ctrl, &old_ctrl[g * 8], 8);
            uint64_t occ = dshmap__ctrl_occupied(ctrl);
            while (occ) {
                size_t pos = dshmap__slot_pos(g, dshmap__ctrl_next_match(&occ));
                dshmap_hash_t hash = old_hashes != NULL ?
                    old_hashes[pos] : st->hash_fn(old_slots[pos]);
                dshmap_insert(st, old_slots[pos], hash);
            }
        }
        DSHMAP_FREE(old_ctrl);
    }
}

static inline void
dshmap__swiss_grow(dshmap *st)
{
    bool was_allocated = dshmap__is_allocated(st);
    size_t new_groups;
    if (!was_allocated) {
        new_groups = 1;
    } else {
        size_t old_groups = dshmap__checked_add(st->group_mask, 1);
        new_groups = dshmap__checked_mul(old_groups, 2);
    }
    dshmap__swiss_grow_to(st, new_groups);
}

static inline void
dshmap__dense_insert_no_grow(dshmap *st, void *entry, dshmap_hash_t hash)
{
    size_t mask = dshmap__dense_mask(st);
    size_t pos = hash & mask;
    while (st->ctrl[pos] != DSHMAP__EMPTY) {
        pos = (pos + 1) & mask;
    }
    st->ctrl[pos] = (int8_t)dshmap__h2(hash);
    dshmap__set_slot_hash(st, pos, hash);
    st->slots[pos] = entry;
    st->size++;
    st->growth_left = dshmap__dense_growth_left(st);
}

static inline void
dshmap__dense_grow_to(dshmap *st, size_t new_cap)
{
    size_t old_cap = dshmap__dense_capacity(st);
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    dshmap_hash_t *old_hashes = st->hashes;

    dshmap__dense_alloc(st, new_cap);
    for (size_t i = 0; i < old_cap; i++) {
        if (old_ctrl[i] != DSHMAP__EMPTY) {
            dshmap_hash_t hash = old_hashes != NULL ?
                old_hashes[i] : st->hash_fn(old_slots[i]);
            dshmap__dense_insert_no_grow(st, old_slots[i], hash);
        }
    }
    DSHMAP_FREE(old_ctrl);
}

static inline void
dshmap__dense_grow_for_count(dshmap *st, size_t count)
{
    dshmap__dense_grow_to(st, dshmap__dense_cap_for_count(count));
}

static inline void
dshmap__promote_to_swiss(dshmap *st, size_t count)
{
    size_t old_cap = dshmap__dense_capacity(st);
    int8_t *old_ctrl = st->ctrl;
    void **old_slots = st->slots;
    dshmap_hash_t *old_hashes = st->hashes;

    size_t new_groups = dshmap__groups_for_count(count, 1);
    dshmap__swiss_alloc(st, dshmap__capacity_from_groups(new_groups));
    st->group_mask = new_groups - 1;
    st->size = 0;
    st->growth_left = dshmap__growth_left_for_cap(
        dshmap__capacity_from_groups(new_groups));

    for (size_t i = 0; i < old_cap; i++) {
        if (old_ctrl[i] != DSHMAP__EMPTY) {
            dshmap_hash_t hash = old_hashes != NULL ?
                old_hashes[i] : st->hash_fn(old_slots[i]);
            dshmap_insert(st, old_slots[i], hash);
        }
    }
    DSHMAP_FREE(old_ctrl);
}

static inline void
dshmap__dense_erase_at(dshmap *st, size_t hole)
{
    size_t mask = dshmap__dense_mask(st);
    size_t pos = (hole + 1) & mask;

    while (st->ctrl[pos] != DSHMAP__EMPTY) {
        size_t ideal = dshmap__slot_hash(st, pos) & mask;
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

    st->ctrl[hole] = DSHMAP__EMPTY;
    st->slots[hole] = NULL;
    if (st->hashes != NULL) {
        st->hashes[hole] = 0;
    }
    st->size--;
    st->growth_left = dshmap__dense_growth_left(st);
}

static inline void
dshmap__swiss_insert(dshmap *st, void *entry, dshmap_hash_t hash)
{
    uint8_t h2 = dshmap__h2(hash);

    if (DSHMAP__UNLIKELY(st->growth_left == 0)) {
        DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t deleted = dshmap__ctrl_deleted(ctrl);
            if (deleted) {
                size_t pos = dshmap__slot_pos(index, dshmap__match_slot(deleted));
                st->ctrl[pos] = (int8_t)h2;
                dshmap__set_slot_hash(st, pos, hash);
                st->slots[pos] = entry;
                st->size++;
                return;
            }

            if (dshmap__group_has_empty(ctrl)) {
                break;
            }
        }
        dshmap__swiss_grow(st);
    }

    DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
        uint64_t available = dshmap__ctrl_available(ctrl);
        if (DSHMAP__LIKELY(available)) {
            size_t pos = dshmap__slot_pos(index, dshmap__match_slot(available));
            bool was_empty = (st->ctrl[pos] == DSHMAP__EMPTY);
            st->ctrl[pos] = (int8_t)h2;
            dshmap__set_slot_hash(st, pos, hash);
            st->slots[pos] = entry;
            st->size++;
            if (DSHMAP__LIKELY(was_empty)) {
                st->growth_left--;
            }
            return;
        }
    }
}

/* ===========================================================================
 *                             IMPLEMENTATION
 * =========================================================================== */

static inline void
dshmap_init(dshmap *st, dshmap_hash_fn hash_fn)
{
    st->ctrl = (int8_t *)dshmap__empty_ctrl;
    st->slots = NULL;
    st->hashes = NULL;
    st->hash_fn = hash_fn;
    st->size = 0;
    st->group_mask = 0;
    st->growth_left = 0;
    st->dense = false;
}

static inline void
dshmap_destroy(dshmap *st)
{
    if (dshmap__is_allocated(st)) {
        DSHMAP_FREE(st->ctrl);
    }
    dshmap_hash_fn fn = st->hash_fn;
    dshmap_init(st, fn);
}

static inline size_t
dshmap_size(const dshmap *st)
{
    return st->size;
}

static inline bool
dshmap_is_empty(const dshmap *st)
{
    return st->size == 0;
}

static inline void
dshmap_clear(dshmap *st)
{
    if (dshmap__is_dense(st)) {
        size_t cap = dshmap__dense_capacity(st);
        memset(st->ctrl, DSHMAP__EMPTY, cap);
        st->size = 0;
        st->growth_left = dshmap__dense_growth_left_for_cap(cap);
    } else {
        if (dshmap__is_allocated(st)) {
            size_t cap = dshmap__capacity_from_groups(
                dshmap__checked_add(st->group_mask, 1));
            memset(st->ctrl, DSHMAP__EMPTY, cap);
            st->size = 0;
            st->growth_left = dshmap__growth_left_for_cap(cap);
        }
    }
}

static inline void
dshmap_reserve(dshmap *st, size_t count)
{
    if (DSHMAP_DENSE_THRESHOLD != 0 && count <= DSHMAP_DENSE_THRESHOLD &&
        !dshmap__is_allocated(st)) {
        if (count == 0) {
            return;
        }
        dshmap__dense_alloc(st, dshmap__dense_cap_for_count(count));
        return;
    }

    if (dshmap__is_dense(st)) {
        if (count <= DSHMAP_DENSE_THRESHOLD) {
            if (count <= st->size + st->growth_left) {
                return;
            }
            dshmap__dense_grow_for_count(st, count);
            return;
        }
        dshmap__promote_to_swiss(st, count);
        return;
    } else {
        if (count <= st->size + st->growth_left) {
            return;
        }

        bool was_allocated = dshmap__is_allocated(st);
        size_t new_groups;
        if (!was_allocated) {
            new_groups = 1;
        } else {
            new_groups = dshmap__checked_mul(
                dshmap__checked_add(st->group_mask, 1), 2);
        }
        new_groups = dshmap__groups_for_count(count, new_groups);
        dshmap__swiss_grow_to(st, new_groups);
    }
}

static inline void
dshmap_insert(dshmap *st, void *entry, dshmap_hash_t hash)
{
    if (dshmap__is_dense(st)) {
        size_t count = dshmap__checked_add(st->size, 1);
        if (DSHMAP__UNLIKELY(count > DSHMAP_DENSE_THRESHOLD)) {
            dshmap__promote_to_swiss(st, count);
            dshmap__swiss_insert(st, entry, hash);
        } else {
            if (DSHMAP__UNLIKELY(st->growth_left == 0)) {
                dshmap__dense_grow_for_count(st, count);
            }
            dshmap__dense_insert_no_grow(st, entry, hash);
        }
    } else {
        if (DSHMAP__UNLIKELY(!dshmap__is_allocated(st)) &&
            DSHMAP_DENSE_THRESHOLD != 0) {
            dshmap__dense_alloc(st, dshmap__dense_cap_for_count(1));
            dshmap__dense_insert_no_grow(st, entry, hash);
        } else {
            dshmap__swiss_insert(st, entry, hash);
        }
    }
}

static inline void
dshmap_remove(dshmap *st, const void *entry, dshmap_hash_t hash)
{
    if (dshmap__is_dense(st)) {
        size_t mask = dshmap__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != DSHMAP__EMPTY) {
            if (dshmap__slot_hash(st, pos) == hash &&
                st->slots[pos] == entry) {
                dshmap__dense_erase_at(st, pos);
                return;
            }
            pos = (pos + 1) & mask;
        }
        return;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if ((st->hashes == NULL || st->hashes[pos] == hash) &&
                    st->slots[pos] == entry) {
                    uint64_t empty = dshmap__group_has_empty(ctrl);
                    st->ctrl[pos] = empty ? DSHMAP__EMPTY : DSHMAP__DELETED;
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
            if (dshmap__group_has_empty(ctrl)) {
                return;
            }
        }
    }
}

static inline void *
dshmap_find(const dshmap *st, dshmap_hash_t hash)
{
    if (dshmap__is_dense(st)) {
        size_t mask = dshmap__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != DSHMAP__EMPTY) {
            if (dshmap__slot_hash(st, pos) == hash) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (dshmap__slot_hash(st, pos) == hash) {
                    return st->slots[pos];
                }
            }

            if (dshmap__group_has_empty(ctrl)) {
                return NULL;
            }
        }
        return NULL;
    }
}

static inline void *
dshmap_find_key(const dshmap *st, dshmap_hash_t hash, const void *key,
               dshmap_key_eq_fn eq_fn)
{
    if (dshmap__is_dense(st)) {
        int8_t h2 = (int8_t)dshmap__h2(hash);
        size_t mask = dshmap__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != DSHMAP__EMPTY) {
            if ((st->hashes != NULL ? st->hashes[pos] == hash :
                 st->ctrl[pos] == h2) &&
                eq_fn(st->slots[pos], key)) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if ((st->hashes == NULL || st->hashes[pos] == hash) &&
                    eq_fn(st->slots[pos], key)) {
                    return st->slots[pos];
                }
            }

            if (dshmap__group_has_empty(ctrl)) {
                return NULL;
            }
        }
        return NULL;
    }
}

static inline void *
dshmap_find_key_next(const dshmap *st, dshmap_hash_t hash, const void *key,
                    dshmap_key_eq_fn eq_fn, const void *prev)
{
    if (dshmap__is_dense(st)) {
        bool found_prev = false;
        int8_t h2 = (int8_t)dshmap__h2(hash);
        size_t mask = dshmap__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != DSHMAP__EMPTY) {
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
    } else {
        uint8_t h2 = dshmap__h2(hash);
        bool found_prev = false;
        DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
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

            if (dshmap__group_has_empty(ctrl)) {
                return NULL;
            }
        }
        return NULL;
    }
}

static inline void *
dshmap_find_next(const dshmap *st, dshmap_hash_t hash, const void *prev)
{
    if (dshmap__is_dense(st)) {
        bool found_prev = false;
        size_t mask = dshmap__dense_mask(st);
        size_t pos = hash & mask;
        while (st->ctrl[pos] != DSHMAP__EMPTY) {
            if (!found_prev) {
                if (st->slots[pos] == prev) {
                    found_prev = true;
                }
            } else if (dshmap__slot_hash(st, pos) == hash) {
                return st->slots[pos];
            }
            pos = (pos + 1) & mask;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        bool found_prev = false;
        DSHMAP__FOR_EACH_GROUP(st, hash, index, ctrl) {
            uint64_t match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (!found_prev) {
                    if (st->slots[pos] == prev) {
                        found_prev = true;
                    }
                    continue;
                }
                if (dshmap__slot_hash(st, pos) == hash) {
                    return st->slots[pos];
                }
            }

            if (dshmap__group_has_empty(ctrl)) {
                return NULL;
            }
        }
        return NULL;
    }
}

#endif
