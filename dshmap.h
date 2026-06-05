#ifndef DSHMAP_H
#define DSHMAP_H

#define DSHMAP_VERSION_MAJOR 0
#define DSHMAP_VERSION_MINOR 2
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

#ifndef DSHMAP_DISABLE_SIMD
#define DSHMAP_DISABLE_SIMD 0
#endif

#if !DSHMAP_DISABLE_SIMD && defined(__AVX2__)
#include <immintrin.h>
#define DSHMAP__BACKEND_X86 1
#elif !DSHMAP_DISABLE_SIMD && \
      (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define DSHMAP__BACKEND_NEON 1
#else
#define DSHMAP__BACKEND_SWAR 1
#endif

#ifndef DSHMAP__BACKEND_X86
#define DSHMAP__BACKEND_X86 0
#endif
#ifndef DSHMAP__BACKEND_NEON
#define DSHMAP__BACKEND_NEON 0
#endif
#ifndef DSHMAP__BACKEND_SWAR
#define DSHMAP__BACKEND_SWAR 0
#endif

#if DSHMAP__BACKEND_SWAR
#define DSHMAP__GROUP_WIDTH 8
#define DSHMAP__GROUP_SHIFT 3
#else
#define DSHMAP__GROUP_WIDTH 16
#define DSHMAP__GROUP_SHIFT 4
#endif
#define DSHMAP__GROUP_SLOT_MASK (DSHMAP__GROUP_WIDTH - 1)
#define DSHMAP__GROUP_MASK_BITS \
    ((uint64_t)((1ULL << DSHMAP__GROUP_WIDTH) - 1ULL))

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 *                               PUBLIC API
 * =========================================================================== */

/* DSHMAP_LOAD_FACTOR_NUM / DSHMAP_LOAD_FACTOR_DEN - Maximum load factor.
 *
 * The Swiss layout resizes when occupancy exceeds NUM/DEN of capacity.
 * Default is 7/8 (87.5%). Define both macros before including this
 * header to override. Small chained mode uses a bucket/node capacity of
 * at least the requested entry count and promotes to Swiss when it
 * exceeds DSHMAP_SMALL_THRESHOLD.
 * SIMD Swiss groups require at least 1/16. The SWAR fallback uses
 * 8-slot groups and requires at least 1/8. Use at least 1/8 if the
 * same config must compile on all supported backends.
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

/* DSHMAP_SMALL_THRESHOLD - Maximum entries kept in small chained mode.
 *
 * Tables start as a pooled chained table. Once an insert would exceed
 * this threshold, the table promotes to the Swiss layout. Reserve can
 * also promote the table. Promotion is one-way; clear does not move a
 * table back to small mode. Define as 0 to disable small mode.
 */
#ifndef DSHMAP_SMALL_THRESHOLD
#define DSHMAP_SMALL_THRESHOLD 2048
#endif

/* DSHMAP_SWISS_STORE_HASHES - Swiss hash storage.
 *
 * Define as 1 to store one full hash per Swiss slot, or 0 to recompute
 * hashes from entries when needed. Swiss mode does not store full hashes
 * by default. Small chained mode always stores full hashes in its nodes.
 */
#ifndef DSHMAP_SWISS_STORE_HASHES
#define DSHMAP_SWISS_STORE_HASHES 0
#endif

/* DSHMAP_DISABLE_SIMD - Force the SWAR control-byte backend.
 *
 * By default, dshmap uses the fastest simple backend found in local
 * benchmarks:
 *
 * - x86 SIMD on AVX2 compiler targets
 * - NEON on ARM targets
 * - SWAR otherwise
 *
 * x86 builds use SIMD only when AVX2 is enabled; otherwise they use SWAR.
 * Define this as 1 before including dshmap.h to disable SIMD and force the
 * SWAR fallback.
 *
 *     #define DSHMAP_DISABLE_SIMD 1
 *     #include "dshmap.h"
 */
#if DSHMAP_LOAD_FACTOR_NUM < 1
#error "DSHMAP_LOAD_FACTOR_NUM must be at least 1"
#endif
#if DSHMAP_LOAD_FACTOR_DEN < 1
#error "DSHMAP_LOAD_FACTOR_DEN must be at least 1"
#endif
#if DSHMAP_LOAD_FACTOR_NUM > DSHMAP_LOAD_FACTOR_DEN
#error "DSHMAP_LOAD_FACTOR_NUM must be <= DSHMAP_LOAD_FACTOR_DEN"
#endif
#if DSHMAP_SMALL_THRESHOLD < 0
#error "DSHMAP_SMALL_THRESHOLD must be >= 0"
#endif
#if DSHMAP_SWISS_STORE_HASHES != 0 && DSHMAP_SWISS_STORE_HASHES != 1
#error "DSHMAP_SWISS_STORE_HASHES must be 0 or 1"
#endif
#if DSHMAP_DISABLE_SIMD != 0 && DSHMAP_DISABLE_SIMD != 1
#error "DSHMAP_DISABLE_SIMD must be 0 or 1"
#endif
#if DSHMAP_LOAD_FACTOR_DEN / DSHMAP__GROUP_WIDTH > DSHMAP_LOAD_FACTOR_NUM || \
    (DSHMAP_LOAD_FACTOR_DEN / DSHMAP__GROUP_WIDTH == DSHMAP_LOAD_FACTOR_NUM && \
     DSHMAP_LOAD_FACTOR_DEN % DSHMAP__GROUP_WIDTH != 0)
#if DSHMAP__GROUP_WIDTH == 8
#error "DSHMAP_LOAD_FACTOR_NUM / DSHMAP_LOAD_FACTOR_DEN must be at least 1/8"
#else
#error "DSHMAP_LOAD_FACTOR_NUM / DSHMAP_LOAD_FACTOR_DEN must be at least 1/16"
#endif
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

/* dshmap - A small-chained-to-Swiss hash map.
 *
 * Stores non-NULL void pointers to caller-owned entries. Entries are
 * located by hash; the caller provides a hash function that can
 * recompute the hash from an entry pointer. NULL entries are not
 * supported because NULL is used as the lookup miss result.
 * Tables use a pooled chained layout up to DSHMAP_SMALL_THRESHOLD
 * entries, then promote to the Swiss layout. Swiss full-hash storage can
 * be configured with DSHMAP_SWISS_STORE_HASHES. Small mode always stores
 * full hashes.
 * Promotion is one-way. A promoted table stays Swiss until destroy.
 *
 * The struct definition is exposed so the type can be stack allocated.
 * Its fields are implementation details. Application code should not read
 * or write them directly.
 *
 * dshmap has no internal locking. Use external locking if any thread may
 * mutate the table while another thread can access it.
 *
 * Must be initialized with dshmap_init() or DSHMAP_INITIALIZER before use
 * and cleaned up with dshmap_destroy(). Stack allocation is typical:
 *
 *     dshmap map;
 *     dshmap_init(&map, my_hash);
 */
typedef struct dshmap {
    int8_t *ctrl;         /* Swiss control bytes, or small allocation base */
    void **slots;         /* Swiss slots, or small node pool */
    dshmap_hash_t *hashes; /* stored Swiss full hashes, if enabled */
    dshmap_hash_fn hash_fn; /* recompute hashes when needed */
    size_t size;          /* number of entries */
    size_t group_mask;    /* Swiss groups - 1, or small buckets - 1 */
    size_t growth_left;   /* Swiss growth left, or small free-list head */
    bool small;           /* using the small chained layout */
} dshmap;

/* DSHMAP_INITIALIZER - Static initializer for an empty table.
 *
 * Use this for static storage or aggregate initialization. HASH_FN has
 * the same contract as the hash_fn argument to dshmap_init(). The table
 * may be used immediately and must still be cleaned up with
 * dshmap_destroy() when done.
 *
 *     static dshmap map = DSHMAP_INITIALIZER(my_hash);
 */
#define DSHMAP_INITIALIZER(HASH_FN) \
    { (int8_t *)dshmap__empty_ctrl, NULL, NULL, (HASH_FN), 0, 0, 0, false }

/* dshmap_iter - Cursor for iterating entries.
 *
 * The struct definition is exposed so iterators can be stack allocated.
 * Its fields are implementation details. Initialize with dshmap_iter_init()
 * for all entries, dshmap_iter_hash_init() for entries with one full hash,
 * dshmap_iter_hash_candidate_init() for entries that may have one full hash,
 * or dshmap_iter_shard_init() for one read-only shard. Then call the matching
 * next function until it returns NULL. Iteration order is arbitrary and may
 * change after inserts or removes.
 */
typedef struct dshmap_iter {
    dshmap_hash_t hash;
    size_t group;
    size_t match_group;
    size_t probe;
    size_t step;
    size_t limit;
    uint64_t occupied;
} dshmap_iter;

/* dshmap_init - Initialize a table.
 *
 * The caller must call dshmap_destroy() when done. No memory is
 * allocated until the first insert. hash_fn is normally required. It may
 * be NULL only when the compile-time settings guarantee that every active
 * layout stores full hashes. In the default config, pass a real hash
 * function.
 *
 *     dshmap map;
 *     dshmap_init(&map, my_hash);
 *     // ... use map ...
 *     dshmap_destroy(&map);
 */
static inline void
dshmap_init(dshmap *map, dshmap_hash_fn hash_fn);

/* dshmap_destroy - Free all memory owned by the table.
 *
 * Does not free the entries themselves; the caller owns those. The
 * table is reset to its initialized state and may be reused. The table
 * keeps the same hash_fn it was initialized with.
 *
 *     dshmap_destroy(&map);
 *     // map is now empty and valid, as if dshmap_init() was just called
 */
static inline void
dshmap_destroy(dshmap *map);

/* dshmap_clear - Mark all slots as empty.
 *
 * Entry pointers are discarded but not freed; the caller owns those.
 * The table keeps its allocated capacity so subsequent inserts avoid
 * reallocation. clear does not demote a Swiss table back to small mode.
 *
 *     dshmap_clear(&map);
 *     assert(dshmap_is_empty(&map));
 *     // map retains its capacity, ready for new inserts
 */
static inline void
dshmap_clear(dshmap *map);

/* dshmap_shrink - Reduce allocated capacity to fit the current size.
 *
 * Keeps all current entries. If the table is empty, frees its storage.
 * Small tables stay small. Swiss tables stay Swiss, even if the current
 * size is below DSHMAP_SMALL_THRESHOLD. This avoids changing the table
 * mode after promotion. The table keeps the same hash_fn.
 *
 *     dshmap_shrink(&map);
 */
static inline void
dshmap_shrink(dshmap *map);

/* dshmap_reserve - Pre-allocate capacity for at least 'count' entries.
 *
 * No-op if the table can already hold 'count' entries without resizing.
 * Call this before a batch of inserts to allocate once upfront instead
 * of resizing repeatedly as the table grows. Reserving more than
 * DSHMAP_SMALL_THRESHOLD entries promotes the table to Swiss mode.
 * Promotion is one-way.
 *
 *     dshmap_reserve(&map, n);
 *     for (size_t i = 0; i < n; i++) {
 *         dshmap_insert(&map, entries[i], hashes[i]);
 *     }
 */
static inline void
dshmap_reserve(dshmap *map, size_t count);

/* dshmap_capacity - Return the current live-entry capacity.
 *
 * Returns 0 when the table has no allocation. Small mode returns the
 * current pooled-node capacity, capped by DSHMAP_SMALL_THRESHOLD. Swiss
 * mode returns the maximum live entries allowed by the load factor for
 * the current slot allocation.
 *
 * Deletes can leave tombstones in Swiss mode, so this is not a promise
 * that every insert sequence can reach this count without resizing.
 *
 *     size_t capacity = dshmap_capacity(&map);
 */
static inline size_t
dshmap_capacity(const dshmap *map);

/* dshmap_size - Return the number of entries in the table.
 *
 *     if (dshmap_size(&map) > 1000) {
 *         // table is large
 *     }
 */
static inline size_t
dshmap_size(const dshmap *map);

/* dshmap_is_empty - Return true if the table contains no entries.
 *
 *     if (dshmap_is_empty(&map)) {
 *         printf("nothing to process\n");
 *     }
 */
static inline bool
dshmap_is_empty(const dshmap *map);

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
 *     dshmap_insert(&map, obj, my_hash(obj));
 */
static inline void
dshmap_insert(dshmap *map, void *entry, dshmap_hash_t hash);

/* dshmap_find - Look up an entry by hash.
 *
 * Returns the first entry whose hash matches, or NULL if none. This is
 * a hash-only lookup: callers that need key equality must either check
 * candidate entries with dshmap_find_next() or use dshmap_find_key().
 * When multiple entries share a hash, use dshmap_find_next() to iterate
 * through them.
 *
 *     void *obj = dshmap_find(&map, hash);
 *     if (obj) {
 *         printf("found: %s\n", ((struct my_obj *)obj)->name);
 *     }
 */
static inline void *
dshmap_find(const dshmap *map, dshmap_hash_t hash);

/* dshmap_find_next - Continue a lookup after dshmap_find().
 *
 * Returns the next entry with the same hash after 'prev', or NULL if
 * there are no more. 'prev' must be a pointer previously returned by
 * dshmap_find() or dshmap_find_next() for the same hash. For key-aware
 * lookup, prefer dshmap_find_key() and dshmap_find_key_next().
 *
 *     dshmap_hash_t h = my_hash(key);
 *     for (void *e = dshmap_find(&map, h); e; e = dshmap_find_next(&map, h, e)) {
 *         process(e);
 *     }
 */
static inline void *
dshmap_find_next(const dshmap *map, dshmap_hash_t hash, const void *prev);

/* dshmap_find_with_hash_fn - Look up by hash with a caller hash function.
 *
 * Same as dshmap_find(), but when Swiss full hashes are not stored it calls
 * hash_fn(entry) to check a candidate's full hash instead of map->hash_fn.
 * This is useful for wrappers whose entries already store their full hash.
 *
 * hash_fn must return the same full hash that was used when the entry was
 * inserted. Pass NULL to use map->hash_fn.
 *
 *     void *obj = dshmap_find_with_hash_fn(&map, hash, entry_hash);
 */
static inline void *
dshmap_find_with_hash_fn(const dshmap *map, dshmap_hash_t hash,
                         dshmap_hash_fn hash_fn);

/* dshmap_find_next_with_hash_fn - Continue a caller-hash lookup.
 *
 * Same as dshmap_find_next(), but uses hash_fn for candidate full-hash checks
 * when Swiss full hashes are not stored. 'prev' must be a pointer previously
 * returned by dshmap_find_with_hash_fn() or dshmap_find_next_with_hash_fn()
 * for the same hash.
 *
 *     for (void *e = dshmap_find_with_hash_fn(&map, h, entry_hash);
 *          e;
 *          e = dshmap_find_next_with_hash_fn(&map, h, e, entry_hash)) {
 *         process(e);
 *     }
 */
static inline void *
dshmap_find_next_with_hash_fn(const dshmap *map, dshmap_hash_t hash,
                              const void *prev, dshmap_hash_fn hash_fn);

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
 *     struct my_obj *obj = dshmap_find_key(&map, hash, name, my_eq);
 */
static inline void *
dshmap_find_key(const dshmap *map, dshmap_hash_t hash, const void *key,
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
 *     for (void *e = dshmap_find_key(&map, h, name, my_eq);
 *          e;
 *          e = dshmap_find_key_next(&map, h, name, my_eq, e)) {
 *         process(e);
 *     }
 */
static inline void *
dshmap_find_key_next(const dshmap *map, dshmap_hash_t hash, const void *key,
                    dshmap_key_eq_fn eq_fn, const void *prev);

/* dshmap_remove - Remove an entry from the table.
 *
 * Removes by pointer identity, not by hash equality. The caller must
 * pass the exact pointer that was inserted and the same full hash used
 * for insertion. No-op if the entry is not found. Does not free the
 * entry; the caller owns it.
 *
 *     dshmap_remove(&map, obj, my_hash(obj));
 *     free(obj);
 */
static inline void
dshmap_remove(dshmap *map, const void *entry, dshmap_hash_t hash);

/* dshmap_iter_init - Initialize an iterator.
 *
 * The iterator starts before the first entry. The map argument is accepted
 * for future compatibility and should be the map that will be iterated.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_init(&iter, &map);
 */
static inline void
dshmap_iter_init(dshmap_iter *iter, const dshmap *map);

/* dshmap_iter_hash_init - Initialize a hash iterator.
 *
 * Sets up iter to visit each entry in map whose full hash equals hash.
 * The iterator starts before the first matching entry.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_hash_init(&iter, &map, hash);
 */
static inline void
dshmap_iter_hash_init(dshmap_iter *iter, const dshmap *map,
                      dshmap_hash_t hash);

/* dshmap_iter_hash_candidate_init - Initialize a hash-candidate iterator.
 *
 * Sets up iter to visit entries that may have the full hash 'hash'. The
 * caller must check each returned entry's full hash before treating it as
 * a match. This is useful for wrappers whose entries already store their
 * hash, because dshmap_iter_hash_candidate_next() does not call map->hash_fn.
 *
 * In small mode this visits entries in the matching small bucket. In Swiss
 * mode this visits entries with the matching H2 tag.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_hash_candidate_init(&iter, &map, hash);
 */
static inline void
dshmap_iter_hash_candidate_init(dshmap_iter *iter, const dshmap *map,
                                dshmap_hash_t hash);

/* dshmap_iter_shard_init - Initialize a read-only shard iterator.
 *
 * Sets up iter to visit the entries in one shard of map. shard_count must be
 * nonzero, and shard must be less than shard_count. Running every shard from
 * 0 to shard_count - 1 visits every entry exactly once across all shards.
 *
 * This only splits iteration work. It does not make map thread-safe. Do not
 * insert, remove, clear, shrink, reserve, or destroy the table while any shard
 * iterator is active. It is fine for several threads to read different shards
 * of the same table if no thread mutates the table.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_shard_init(&iter, &map, shard, shard_count);
 */
static inline void
dshmap_iter_shard_init(dshmap_iter *iter, const dshmap *map,
                       size_t shard, size_t shard_count);

/* dshmap_iter_next - Return the next entry from an iterator.
 *
 * Returns NULL when there are no more entries. Do not insert or remove
 * entries while using this iterator. Use DSHMAP_FOR_EACH_SAFE when the
 * current entry may be removed during iteration.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_init(&iter, &map);
 *     for (void *entry = dshmap_iter_next(&map, &iter);
 *          entry;
 *          entry = dshmap_iter_next(&map, &iter)) {
 *         process(entry);
 *     }
 */
static inline void *
dshmap_iter_next(const dshmap *map, dshmap_iter *iter);

/* dshmap_iter_hash_next - Return the next entry from a hash iterator.
 *
 * Returns NULL when there are no more entries with the hash passed to
 * dshmap_iter_hash_init(). Do not insert or remove entries while using
 * this iterator.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_hash_init(&iter, &map, hash);
 *     for (void *entry = dshmap_iter_hash_next(&map, &iter);
 *          entry;
 *          entry = dshmap_iter_hash_next(&map, &iter)) {
 *         process(entry);
 *     }
 */
static inline void *
dshmap_iter_hash_next(const dshmap *map, dshmap_iter *iter);

/* dshmap_iter_hash_next_with_hash_fn - Return the next exact hash match.
 *
 * Same as dshmap_iter_hash_next(), but when Swiss full hashes are not stored
 * it calls hash_fn(entry) to check each candidate's full hash instead of
 * map->hash_fn. This is useful for wrappers whose entries already store their
 * full hash.
 *
 * hash_fn must return the same full hash that was used when the entry was
 * inserted. Pass NULL to use map->hash_fn.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_hash_init(&iter, &map, hash);
 *     for (void *entry = dshmap_iter_hash_next_with_hash_fn(&map, &iter,
 *                                                           entry_hash);
 *          entry;
 *          entry = dshmap_iter_hash_next_with_hash_fn(&map, &iter,
 *                                                     entry_hash)) {
 *         process(entry);
 *     }
 */
static inline void *
dshmap_iter_hash_next_with_hash_fn(const dshmap *map, dshmap_iter *iter,
                                   dshmap_hash_fn hash_fn);

/* dshmap_iter_hash_candidate_next - Return the next hash candidate.
 *
 * Returns NULL when there are no more candidates for the hash passed to
 * dshmap_iter_hash_candidate_init(). Returned entries may have a different
 * full hash; the caller must check. Do not insert or remove entries while
 * using this iterator.
 *
 * This function does not call map->hash_fn. It only uses hashes already stored
 * by the table layout and the Swiss control-byte tag.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_hash_candidate_init(&iter, &map, hash);
 *     for (void *entry = dshmap_iter_hash_candidate_next(&map, &iter);
 *          entry;
 *          entry = dshmap_iter_hash_candidate_next(&map, &iter)) {
 *         if (entry_hash(entry) == hash) {
 *             process(entry);
 *         }
 *     }
 */
static inline void *
dshmap_iter_hash_candidate_next(const dshmap *map, dshmap_iter *iter);

/* dshmap_iter_shard_next - Return the next entry from a shard iterator.
 *
 * Returns NULL when there are no more entries in the shard passed to
 * dshmap_iter_shard_init(). Do not mutate the table while using this iterator.
 *
 *     dshmap_iter iter;
 *     dshmap_iter_shard_init(&iter, &map, shard, shard_count);
 *     for (void *entry = dshmap_iter_shard_next(&map, &iter);
 *          entry;
 *          entry = dshmap_iter_shard_next(&map, &iter)) {
 *         process(entry);
 *     }
 */
static inline void *
dshmap_iter_shard_next(const dshmap *map, dshmap_iter *iter);

/* dshmap_iter_next_after - Return the entry after another entry.
 *
 * entry must be non-NULL and currently present in map. This is mainly used
 * by DSHMAP_FOR_EACH_SAFE to pick the next entry before the current entry is
 * removed. It scans from the start of the table, so use dshmap_iter_next()
 * for normal iteration.
 */
static inline void *
dshmap_iter_next_after(const dshmap *map, const void *entry);

/* dshmap_iter_next_after_hash - Return the entry after another entry.
 *
 * entry must be non-NULL and currently present in map. hash must be the
 * entry's full hash. This is for wrappers that keep only an entry pointer
 * but can cheaply get its hash. It uses the hash to find entry, then returns
 * the next entry in normal all-entry iteration order.
 *
 * Prefer dshmap_iter_next() for normal loops. Use this only when the API
 * shape cannot keep iterator state between calls.
 */
static inline void *
dshmap_iter_next_after_hash(const dshmap *map, const void *entry,
                            dshmap_hash_t hash);

/* DSHMAP_FOR_EACH - Iterate over all entries in the table.
 *
 * 'var' is declared as void * in the loop scope. Entries must be
 * non-NULL; NULL is used internally as the loop sentinel. Iteration
 * order is arbitrary and not related to insertion order. Do not insert
 * or remove entries during iteration. The 'map' argument is evaluated once.
 *
 *     DSHMAP_FOR_EACH(entry, &map) {
 *         printf("%s\n", ((struct my_obj *)entry)->name);
 *     }
 */
#define DSHMAP_FOR_EACH(var, map) \
    for (const dshmap *var##_map_ = (map); var##_map_; var##_map_ = NULL) \
    for (dshmap_iter var##_iter_, *var##_iterp_ = \
             (dshmap_iter_init(&var##_iter_, var##_map_), &var##_iter_); \
         var##_iterp_; \
         var##_iterp_ = NULL) \
    for (void *var = dshmap_iter_next(var##_map_, var##_iterp_); \
         var; \
         var = dshmap_iter_next(var##_map_, var##_iterp_))

/* DSHMAP_FOR_EACH_SAFE - Iterate while the current entry may be removed.
 *
 * 'var' and 'next' are declared as void * in the loop scope. The loop is
 * safe when the body removes or frees only the current entry, var. Do not
 * insert entries, clear the table, or remove other entries during the loop.
 * Iteration order is arbitrary. The 'map' argument is evaluated once.
 *
 *     DSHMAP_FOR_EACH_SAFE(entry, next, &map) {
 *         dshmap_remove(&map, entry, my_hash(entry));
 *         free(entry);
 *     }
 */
#define DSHMAP_FOR_EACH_SAFE(var, next, map) \
    for (const dshmap *var##_map_ = (map); var##_map_; var##_map_ = NULL) \
    for (dshmap_iter var##_iter_, *var##_iterp_ = \
             (dshmap_iter_init(&var##_iter_, var##_map_), &var##_iter_); \
         var##_iterp_; \
         var##_iterp_ = NULL) \
    for (void *var = dshmap_iter_next(var##_map_, var##_iterp_), *next = NULL; \
         var && ((next = dshmap__iter_next_safe(var##_map_, var##_iterp_, var)), true); \
         var = next)

/* DSHMAP_FOR_EACH_WITH_HASH - Iterate over entries with a full hash.
 *
 * 'var' is declared as void * in the loop scope. Visits each entry whose
 * full hash equals 'hash'. The map and hash expressions are evaluated
 * once. Iteration order is arbitrary and not related to insertion order.
 * Do not insert or remove entries during iteration.
 *
 *     DSHMAP_FOR_EACH_WITH_HASH(entry, &map, hash) {
 *         process(entry);
 *     }
 */
#define DSHMAP_FOR_EACH_WITH_HASH(var, map, hash) \
    for (const dshmap *var##_map_ = (map); var##_map_; var##_map_ = NULL) \
    for (dshmap_iter var##_iter_, *var##_iterp_ = \
             (dshmap_iter_hash_init(&var##_iter_, var##_map_, (hash)), \
              &var##_iter_); \
         var##_iterp_; var##_iterp_ = NULL) \
    for (void *var = dshmap_iter_hash_next(var##_map_, var##_iterp_); \
         var; \
         var = dshmap_iter_hash_next(var##_map_, var##_iterp_))

/* DSHMAP_FOR_EACH_SHARD - Iterate over one read-only shard.
 *
 * 'var' is declared as void * in the loop scope. shard_count must be nonzero,
 * and shard must be less than shard_count. Running this macro for every shard
 * from 0 to shard_count - 1 visits every entry exactly once across all shards.
 * The map, shard, and shard_count expressions are evaluated once. Iteration
 * order is arbitrary. Do not mutate the table while shard iteration is active.
 *
 *     for (size_t shard = 0; shard < shard_count; shard++) {
 *         DSHMAP_FOR_EACH_SHARD(entry, &map, shard, shard_count) {
 *             process(entry);
 *         }
 *     }
 */
#define DSHMAP_FOR_EACH_SHARD(var, map, shard, shard_count) \
    for (const dshmap *var##_map_ = (map); var##_map_; var##_map_ = NULL) \
    for (size_t var##_shard_ = (shard), \
                var##_shard_count_ = (shard_count), \
                var##_once_ = 1; \
         var##_once_; \
         var##_once_ = 0) \
    for (dshmap_iter var##_iter_, *var##_iterp_ = \
             (dshmap_iter_shard_init(&var##_iter_, var##_map_, \
                                     var##_shard_, var##_shard_count_), \
              &var##_iter_); \
         var##_iterp_; var##_iterp_ = NULL) \
    for (void *var = dshmap_iter_shard_next(var##_map_, var##_iterp_); \
         var; \
         var = dshmap_iter_shard_next(var##_map_, var##_iterp_))

/* ===========================================================================
 *                                INTERNAL
 * =========================================================================== */

#define DSHMAP__LIKELY(x)   __builtin_expect(!!(x), 1)
#define DSHMAP__UNLIKELY(x) __builtin_expect(!!(x), 0)
#define DSHMAP__ALIGNOF(type) __alignof__(type)

enum {
    DSHMAP__EMPTY   = (int8_t)0x80,
    DSHMAP__DELETED = (int8_t)0xFE,
};

#define DSHMAP__SMALL_END ((size_t)-1)

typedef struct dshmap__small_node {
    dshmap_hash_t hash;
    void *entry;
    size_t next;
} dshmap__small_node;

typedef uint64_t dshmap__ctrl_mask;

#if DSHMAP__BACKEND_X86
typedef __m128i dshmap__ctrl_group;
#elif DSHMAP__BACKEND_NEON
typedef int8x16_t dshmap__ctrl_group;
#else
typedef uint64_t dshmap__ctrl_group;

static const uint64_t DSHMAP__BROADCAST_BYTE = 0x0101010101010101ULL;
static const uint64_t DSHMAP__HIGH_BITS      = 0x8080808080808080ULL;
#endif

#if DSHMAP__GROUP_WIDTH == 16
static const int8_t dshmap__empty_ctrl[DSHMAP__GROUP_WIDTH] = {
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
};
#else
static const int8_t dshmap__empty_ctrl[DSHMAP__GROUP_WIDTH] = {
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
    DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY, DSHMAP__EMPTY,
};
#endif

static inline dshmap_hash_t dshmap__h1(dshmap_hash_t hash) { return hash >> 7; }
static inline uint8_t dshmap__h2(dshmap_hash_t hash) { return hash & 0x7F; }

static inline size_t
dshmap__slot_pos(size_t group_index, size_t slot)
{
    return group_index * DSHMAP__GROUP_WIDTH + slot;
}

static inline size_t
dshmap__group_index(const dshmap *map, dshmap_hash_t hash)
{
    return dshmap__h1(hash) & map->group_mask;
}

static inline size_t
dshmap__next_group_index(const dshmap *map, size_t index, size_t probe)
{
    return (index + probe + 1) & map->group_mask;
}

static inline size_t
dshmap__match_slot(dshmap__ctrl_mask mask)
{
#if DSHMAP__BACKEND_SWAR
    return (size_t)__builtin_ctzll(mask) / 8;
#else
    return (size_t)__builtin_ctzll(mask);
#endif
}

#if DSHMAP__BACKEND_X86
static inline dshmap__ctrl_group
dshmap__load_ctrl_from(const int8_t *ctrl, size_t group_index)
{
    return _mm_loadu_si128((const dshmap__ctrl_group *)(const void *)
                           &ctrl[group_index * DSHMAP__GROUP_WIDTH]);
}

static inline dshmap__ctrl_mask
dshmap__ctrl_available(dshmap__ctrl_group ctrl)
{
    return (dshmap__ctrl_mask)_mm_movemask_epi8(ctrl) &
           DSHMAP__GROUP_MASK_BITS;
}

static inline dshmap__ctrl_mask
dshmap__ctrl_occupied(dshmap__ctrl_group ctrl)
{
    return ~(dshmap__ctrl_mask)_mm_movemask_epi8(ctrl) &
           DSHMAP__GROUP_MASK_BITS;
}

static inline dshmap__ctrl_mask
dshmap__ctrl_match(dshmap__ctrl_group ctrl, uint8_t h2)
{
    dshmap__ctrl_group match = _mm_cmpeq_epi8(ctrl,
                                              _mm_set1_epi8((char)h2));
    return (dshmap__ctrl_mask)_mm_movemask_epi8(match);
}

static inline dshmap__ctrl_mask
dshmap__ctrl_deleted(dshmap__ctrl_group ctrl)
{
    dshmap__ctrl_group match = _mm_cmpeq_epi8(
        ctrl, _mm_set1_epi8((char)DSHMAP__DELETED));
    return (dshmap__ctrl_mask)_mm_movemask_epi8(match);
}

static inline dshmap__ctrl_mask
dshmap__group_has_empty(dshmap__ctrl_group ctrl)
{
    dshmap__ctrl_group match = _mm_cmpeq_epi8(
        ctrl, _mm_set1_epi8((char)DSHMAP__EMPTY));
    return (dshmap__ctrl_mask)_mm_movemask_epi8(match);
}
#elif DSHMAP__BACKEND_NEON
static inline dshmap__ctrl_group
dshmap__load_ctrl_from(const int8_t *ctrl, size_t group_index)
{
    return vld1q_s8(&ctrl[group_index * DSHMAP__GROUP_WIDTH]);
}

static inline uint8_t
dshmap__neon_sum_u8(uint8x8_t v)
{
    uint16x4_t sum16 = vpaddl_u8(v);
    uint32x2_t sum32 = vpaddl_u16(sum16);
    uint64x1_t sum64 = vpaddl_u32(sum32);
    return (uint8_t)vget_lane_u64(sum64, 0);
}

static inline dshmap__ctrl_mask
dshmap__neon_movemask(uint8x16_t v)
{
    static const uint8_t bitmask_data[DSHMAP__GROUP_WIDTH] = {
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128,
    };
    uint8x16_t bits = vmulq_u8(vshrq_n_u8(v, 7),
                               vld1q_u8(bitmask_data));
    return (dshmap__ctrl_mask)dshmap__neon_sum_u8(vget_low_u8(bits)) |
           ((dshmap__ctrl_mask)dshmap__neon_sum_u8(vget_high_u8(bits)) << 8);
}

static inline dshmap__ctrl_mask
dshmap__ctrl_available(dshmap__ctrl_group ctrl)
{
    return dshmap__neon_movemask(vreinterpretq_u8_s8(ctrl));
}

static inline dshmap__ctrl_mask
dshmap__ctrl_occupied(dshmap__ctrl_group ctrl)
{
    return ~dshmap__neon_movemask(vreinterpretq_u8_s8(ctrl)) &
           DSHMAP__GROUP_MASK_BITS;
}

static inline dshmap__ctrl_mask
dshmap__ctrl_match(dshmap__ctrl_group ctrl, uint8_t h2)
{
    return dshmap__neon_movemask(vceqq_s8(ctrl, vdupq_n_s8((int8_t)h2)));
}

static inline dshmap__ctrl_mask
dshmap__ctrl_deleted(dshmap__ctrl_group ctrl)
{
    return dshmap__neon_movemask(
        vceqq_s8(ctrl, vdupq_n_s8((int8_t)DSHMAP__DELETED)));
}

static inline dshmap__ctrl_mask
dshmap__group_has_empty(dshmap__ctrl_group ctrl)
{
    return dshmap__neon_movemask(
        vceqq_s8(ctrl, vdupq_n_s8((int8_t)DSHMAP__EMPTY)));
}
#else
static inline dshmap__ctrl_group
dshmap__load_ctrl_from(const int8_t *ctrl, size_t group_index)
{
    dshmap__ctrl_group group;
    memcpy(&group, &ctrl[group_index * DSHMAP__GROUP_WIDTH], sizeof group);
    return group;
}

static inline uint64_t
dshmap__swar_match8(uint64_t ctrl, uint8_t byte)
{
    uint64_t match = ctrl ^ (DSHMAP__BROADCAST_BYTE * byte);
    return (match - DSHMAP__BROADCAST_BYTE) & ~match & DSHMAP__HIGH_BITS;
}

static inline dshmap__ctrl_mask
dshmap__ctrl_available(dshmap__ctrl_group ctrl)
{
    return ctrl & DSHMAP__HIGH_BITS;
}

static inline dshmap__ctrl_mask
dshmap__ctrl_occupied(dshmap__ctrl_group ctrl)
{
    return ~ctrl & DSHMAP__HIGH_BITS;
}

static inline dshmap__ctrl_mask
dshmap__ctrl_match(dshmap__ctrl_group ctrl, uint8_t h2)
{
    return dshmap__swar_match8(ctrl, h2);
}

static inline dshmap__ctrl_mask
dshmap__ctrl_deleted(dshmap__ctrl_group ctrl)
{
    return dshmap__swar_match8(ctrl, (uint8_t)DSHMAP__DELETED);
}

static inline dshmap__ctrl_mask
dshmap__group_has_empty(dshmap__ctrl_group ctrl)
{
    return (ctrl & ~(ctrl << 1)) & DSHMAP__HIGH_BITS;
}
#endif

static inline dshmap__ctrl_group
dshmap__load_ctrl(const dshmap *map, size_t group_index)
{
    return dshmap__load_ctrl_from(map->ctrl, group_index);
}

static inline size_t
dshmap__ctrl_next_match(dshmap__ctrl_mask *match)
{
    size_t slot = dshmap__match_slot(*match);
    *match &= *match - 1;
    return slot;
}

static inline bool
dshmap__is_allocated(const dshmap *map)
{
    return map->slots != NULL;
}

static inline bool
dshmap__is_small(const dshmap *map)
{
    return map->small;
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
dshmap__align_up(size_t n, size_t align)
{
    size_t rem = n % align;
    return rem ? dshmap__checked_add(n, align - rem) : n;
}

static inline size_t
dshmap__capacity_from_groups(size_t groups)
{
    return dshmap__checked_mul(groups, DSHMAP__GROUP_WIDTH);
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
dshmap__small_capacity(const dshmap *map)
{
    return map->group_mask + 1;
}

static inline size_t *
dshmap__small_buckets(const dshmap *map)
{
    return (size_t *)(void *)map->ctrl;
}

static inline dshmap__small_node *
dshmap__small_nodes(const dshmap *map)
{
    return (dshmap__small_node *)(void *)map->slots;
}

static inline size_t
dshmap__small_bucket_index(const dshmap *map, dshmap_hash_t hash)
{
    return hash & map->group_mask;
}

static inline size_t
dshmap__small_cap_for_count(size_t count)
{
    size_t cap = 1;
    while (cap < count) {
        cap = dshmap__checked_mul(cap, 2);
    }
    return cap;
}

static inline dshmap_hash_t
dshmap__slot_hash(const dshmap *map, size_t pos)
{
#if DSHMAP_SWISS_STORE_HASHES
    if (map->hashes != NULL) {
        return map->hashes[pos];
    }
#endif
    return map->hash_fn(map->slots[pos]);
}

static inline dshmap_hash_t
dshmap__slot_hash_with_hash_fn(const dshmap *map, size_t pos,
                               dshmap_hash_fn hash_fn)
{
#if DSHMAP_SWISS_STORE_HASHES
    if (map->hashes != NULL) {
        return map->hashes[pos];
    }
#endif
    return hash_fn(map->slots[pos]);
}

static inline void
dshmap__set_slot_hash(dshmap *map, size_t pos, dshmap_hash_t hash)
{
#if DSHMAP_SWISS_STORE_HASHES
    if (map->hashes != NULL) {
        map->hashes[pos] = hash;
    }
#else
    (void)map;
    (void)pos;
    (void)hash;
#endif
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

#define DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) \
    for (dshmap__ctrl_group ctrl, *index##_ctrl_once_ = &ctrl; \
         index##_ctrl_once_; index##_ctrl_once_ = NULL) \
    for (size_t index = dshmap__group_index(map, hash), index##_probe_ = 0; \
         index##_probe_ <= (map)->group_mask && (ctrl = dshmap__load_ctrl(map, index), 1); \
         index = dshmap__next_group_index(map, index, index##_probe_++))

static inline void
dshmap__swiss_alloc(dshmap *map, size_t cap)
{
    size_t offset = cap;
#if DSHMAP_SWISS_STORE_HASHES
    size_t hashes_off = dshmap__align_up(offset, DSHMAP__ALIGNOF(dshmap_hash_t));
    size_t hashes_bytes = dshmap__checked_mul(cap, sizeof(dshmap_hash_t));
    offset = dshmap__checked_add(hashes_off, hashes_bytes);
#endif
    size_t slots_off = dshmap__align_up(offset, DSHMAP__ALIGNOF(void *));
    size_t slots_bytes = dshmap__checked_mul(cap, sizeof(void *));
    size_t alloc_size = dshmap__checked_add(slots_off, slots_bytes);
    char *mem = (char *)DSHMAP_MALLOC(alloc_size);
    if (!mem) {
        dshmap__oom();
    }
    map->ctrl = (int8_t *)mem;
    memset(map->ctrl, DSHMAP__EMPTY, cap);
    map->slots = (void **)(void *)(mem + slots_off);
#if DSHMAP_SWISS_STORE_HASHES
    map->hashes = (dshmap_hash_t *)(void *)(mem + hashes_off);
#else
    map->hashes = NULL;
#endif
    map->small = false;
}

static inline void
dshmap__small_alloc(dshmap *map, size_t cap)
{
    size_t buckets_bytes = dshmap__checked_mul(cap, sizeof(size_t));
    size_t nodes_off = buckets_bytes;
    size_t nodes_bytes = dshmap__checked_mul(cap, sizeof(dshmap__small_node));
    size_t alloc_size = dshmap__checked_add(nodes_off, nodes_bytes);
    char *mem = (char *)DSHMAP_MALLOC(alloc_size);
    if (!mem) {
        dshmap__oom();
    }
    map->ctrl = (int8_t *)mem;
    size_t *buckets = dshmap__small_buckets(map);
    for (size_t i = 0; i < cap; i++) {
        buckets[i] = DSHMAP__SMALL_END;
    }
    map->hashes = NULL;
    map->slots = (void **)(void *)(mem + nodes_off);
    dshmap__small_node *nodes = dshmap__small_nodes(map);
    for (size_t i = 0; i < cap; i++) {
        nodes[i].hash = 0;
        nodes[i].entry = NULL;
        nodes[i].next = i + 1 < cap ? i + 1 : DSHMAP__SMALL_END;
    }
    map->group_mask = cap - 1;
    map->size = 0;
    map->growth_left = 0;
    map->small = true;
}

static inline void
dshmap__swiss_insert_no_grow(dshmap *map, void *entry, dshmap_hash_t hash)
{
    uint8_t h2 = dshmap__h2(hash);

    DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
        dshmap__ctrl_mask empty = dshmap__group_has_empty(ctrl);
        if (DSHMAP__LIKELY(empty)) {
            size_t pos = dshmap__slot_pos(index, dshmap__match_slot(empty));
            map->ctrl[pos] = (int8_t)h2;
            dshmap__set_slot_hash(map, pos, hash);
            map->slots[pos] = entry;
            map->size++;
            map->growth_left--;
            return;
        }
    }
}

static inline void
dshmap__swiss_grow_to(dshmap *map, size_t new_groups)
{
    size_t old_groups = dshmap__checked_add(map->group_mask, 1);
    size_t old_cap = dshmap__capacity_from_groups(old_groups);
    int8_t *old_ctrl = map->ctrl;
    void **old_slots = map->slots;
    dshmap_hash_t *old_hashes = map->hashes;
    bool was_allocated = dshmap__is_allocated(map);

    size_t new_cap = dshmap__capacity_from_groups(new_groups);

    dshmap__swiss_alloc(map, new_cap);
    map->group_mask = new_groups - 1;
    map->size = 0;
    map->growth_left = dshmap__growth_left_for_cap(new_cap);

    if (was_allocated) {
        size_t old_groups_n = old_cap / DSHMAP__GROUP_WIDTH;
        for (size_t g = 0; g < old_groups_n; g++) {
            dshmap__ctrl_group ctrl = dshmap__load_ctrl_from(old_ctrl, g);
            dshmap__ctrl_mask occ = dshmap__ctrl_occupied(ctrl);
            while (occ) {
                size_t pos = dshmap__slot_pos(g, dshmap__ctrl_next_match(&occ));
                dshmap_hash_t hash = old_hashes != NULL ?
                    old_hashes[pos] : map->hash_fn(old_slots[pos]);
                dshmap__swiss_insert_no_grow(map, old_slots[pos], hash);
            }
        }
        DSHMAP_FREE(old_ctrl);
    }
}

static inline void
dshmap__swiss_grow(dshmap *map)
{
    bool was_allocated = dshmap__is_allocated(map);
    size_t new_groups;
    if (!was_allocated) {
        new_groups = 1;
    } else {
        size_t old_groups = dshmap__checked_add(map->group_mask, 1);
        new_groups = dshmap__checked_mul(old_groups, 2);
    }
    dshmap__swiss_grow_to(map, new_groups);
}

static inline void
dshmap__small_insert_no_grow(dshmap *map, void *entry, dshmap_hash_t hash)
{
    size_t index = map->growth_left;
    size_t bucket = dshmap__small_bucket_index(map, hash);
    size_t *buckets = dshmap__small_buckets(map);
    dshmap__small_node *nodes = dshmap__small_nodes(map);

    if (DSHMAP__UNLIKELY(index == DSHMAP__SMALL_END)) {
        dshmap__oom();
    }
    map->growth_left = nodes[index].next;
    nodes[index].hash = hash;
    nodes[index].entry = entry;
    nodes[index].next = buckets[bucket];
    buckets[bucket] = index;
    map->size++;
}

static inline void
dshmap__small_grow_to(dshmap *map, size_t new_cap)
{
    size_t old_cap = dshmap__small_capacity(map);
    int8_t *old_base = map->ctrl;
    dshmap__small_node *old_nodes = dshmap__small_nodes(map);

    dshmap__small_alloc(map, new_cap);
    for (size_t i = 0; i < old_cap; i++) {
        if (old_nodes[i].entry != NULL) {
            dshmap__small_insert_no_grow(
                map, old_nodes[i].entry, old_nodes[i].hash);
        }
    }
    DSHMAP_FREE(old_base);
}

static inline void
dshmap__small_grow_for_count(dshmap *map, size_t count)
{
    dshmap__small_grow_to(map, dshmap__small_cap_for_count(count));
}

static inline void
dshmap__promote_to_swiss(dshmap *map, size_t count)
{
    size_t old_cap = dshmap__small_capacity(map);
    int8_t *old_base = map->ctrl;
    dshmap__small_node *old_nodes = dshmap__small_nodes(map);

    size_t new_groups = dshmap__groups_for_count(count, 1);
    dshmap__swiss_alloc(map, dshmap__capacity_from_groups(new_groups));
    map->group_mask = new_groups - 1;
    map->size = 0;
    map->growth_left = dshmap__growth_left_for_cap(
        dshmap__capacity_from_groups(new_groups));

    for (size_t i = 0; i < old_cap; i++) {
        if (old_nodes[i].entry != NULL) {
            dshmap__swiss_insert_no_grow(
                map, old_nodes[i].entry, old_nodes[i].hash);
        }
    }
    DSHMAP_FREE(old_base);
}

static inline void
dshmap__small_erase_at(dshmap *map, size_t index, size_t *link)
{
    dshmap__small_node *nodes = dshmap__small_nodes(map);

    *link = nodes[index].next;
    nodes[index].hash = 0;
    nodes[index].entry = NULL;
    nodes[index].next = map->growth_left;
    map->size--;
    map->growth_left = index;
}

static inline void
dshmap__swiss_insert(dshmap *map, void *entry, dshmap_hash_t hash)
{
    uint8_t h2 = dshmap__h2(hash);

    if (DSHMAP__UNLIKELY(map->growth_left == 0)) {
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask deleted = dshmap__ctrl_deleted(ctrl);
            if (deleted) {
                size_t pos = dshmap__slot_pos(
                    index, dshmap__match_slot(deleted));
                map->ctrl[pos] = (int8_t)h2;
                dshmap__set_slot_hash(map, pos, hash);
                map->slots[pos] = entry;
                map->size++;
                return;
            }

            if (dshmap__group_has_empty(ctrl)) {
                break;
            }
        }
        dshmap__swiss_grow(map);
    }

    DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
        dshmap__ctrl_mask available = dshmap__ctrl_available(ctrl);
        if (DSHMAP__LIKELY(available)) {
            size_t pos = dshmap__slot_pos(index, dshmap__match_slot(available));
            bool was_empty = (map->ctrl[pos] == DSHMAP__EMPTY);
            map->ctrl[pos] = (int8_t)h2;
            dshmap__set_slot_hash(map, pos, hash);
            map->slots[pos] = entry;
            map->size++;
            if (DSHMAP__LIKELY(was_empty)) {
                map->growth_left--;
            }
            return;
        }
    }
}

/* ===========================================================================
 *                             IMPLEMENTATION
 * =========================================================================== */

static inline void
dshmap_init(dshmap *map, dshmap_hash_fn hash_fn)
{
    map->ctrl = (int8_t *)dshmap__empty_ctrl;
    map->slots = NULL;
    map->hashes = NULL;
    map->hash_fn = hash_fn;
    map->size = 0;
    map->group_mask = 0;
    map->growth_left = 0;
    map->small = false;
}

static inline void
dshmap_destroy(dshmap *map)
{
    if (dshmap__is_allocated(map)) {
        DSHMAP_FREE(map->ctrl);
    }
    dshmap_hash_fn fn = map->hash_fn;
    dshmap_init(map, fn);
}

static inline void
dshmap_clear(dshmap *map)
{
    if (dshmap__is_small(map)) {
        size_t cap = dshmap__small_capacity(map);
        size_t *buckets = dshmap__small_buckets(map);
        for (size_t i = 0; i < cap; i++) {
            buckets[i] = DSHMAP__SMALL_END;
        }
        memset(dshmap__small_nodes(map), 0,
               dshmap__checked_mul(cap, sizeof(dshmap__small_node)));
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        for (size_t i = 0; i < cap; i++) {
            nodes[i].next = i + 1 < cap ? i + 1 : DSHMAP__SMALL_END;
        }
        map->size = 0;
        map->growth_left = 0;
    } else {
        if (dshmap__is_allocated(map)) {
            size_t cap = dshmap__capacity_from_groups(
                dshmap__checked_add(map->group_mask, 1));
            memset(map->ctrl, DSHMAP__EMPTY, cap);
            map->size = 0;
            map->growth_left = dshmap__growth_left_for_cap(cap);
        }
    }
}

static inline void
dshmap_shrink(dshmap *map)
{
    if (map->size == 0) {
        dshmap_destroy(map);
    } else if (dshmap__is_small(map)) {
        size_t new_cap = dshmap__small_cap_for_count(map->size);
        if (new_cap < dshmap__small_capacity(map)) {
            dshmap__small_grow_to(map, new_cap);
        }
    } else {
        size_t old_groups = dshmap__checked_add(map->group_mask, 1);
        size_t new_groups = dshmap__groups_for_count(map->size, 1);
        if (new_groups < old_groups) {
            dshmap__swiss_grow_to(map, new_groups);
        }
    }
}

static inline void
dshmap_reserve(dshmap *map, size_t count)
{
    if (DSHMAP_SMALL_THRESHOLD != 0 && count <= DSHMAP_SMALL_THRESHOLD &&
        !dshmap__is_allocated(map)) {
        if (count == 0) {
            return;
        }
        dshmap__small_alloc(map, dshmap__small_cap_for_count(count));
        return;
    }

    if (dshmap__is_small(map)) {
        if (count <= DSHMAP_SMALL_THRESHOLD) {
            if (count <= dshmap__small_capacity(map)) {
                return;
            }
            dshmap__small_grow_for_count(map, count);
            return;
        }
        dshmap__promote_to_swiss(map, count);
        return;
    } else {
        if (count <= map->size + map->growth_left) {
            return;
        }

        bool was_allocated = dshmap__is_allocated(map);
        size_t new_groups;
        if (!was_allocated) {
            new_groups = 1;
        } else {
            new_groups = dshmap__checked_mul(
                dshmap__checked_add(map->group_mask, 1), 2);
        }
        new_groups = dshmap__groups_for_count(count, new_groups);
        dshmap__swiss_grow_to(map, new_groups);
    }
}

static inline size_t
dshmap_capacity(const dshmap *map)
{
    if (!dshmap__is_allocated(map)) {
        return 0;
    }

    if (dshmap__is_small(map)) {
        size_t cap = dshmap__small_capacity(map);
        return cap > DSHMAP_SMALL_THRESHOLD ? DSHMAP_SMALL_THRESHOLD : cap;
    }

    return dshmap__growth_left_for_cap(dshmap__capacity_from_groups(
        dshmap__checked_add(map->group_mask, 1)));
}

static inline size_t
dshmap_size(const dshmap *map)
{
    return map->size;
}

static inline bool
dshmap_is_empty(const dshmap *map)
{
    return map->size == 0;
}

static inline void
dshmap_iter_init(dshmap_iter *iter, const dshmap *map)
{
    (void)map;
    iter->hash = 0;
    iter->group = 0;
    iter->match_group = 0;
    iter->probe = 0;
    iter->step = 1;
    iter->limit = 0;
    iter->occupied = 0;
}

static inline void
dshmap_iter_hash_init(dshmap_iter *iter, const dshmap *map,
                      dshmap_hash_t hash)
{
    iter->hash = hash;
    iter->match_group = 0;
    iter->probe = 0;
    iter->step = 1;
    iter->limit = 0;
    iter->occupied = 0;

    if (!dshmap__is_allocated(map)) {
        iter->group = DSHMAP__SMALL_END;
    } else if (dshmap__is_small(map)) {
        iter->group = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
    } else {
        iter->group = dshmap__group_index(map, hash);
    }
}

static inline void
dshmap_iter_hash_candidate_init(dshmap_iter *iter, const dshmap *map,
                                dshmap_hash_t hash)
{
    dshmap_iter_hash_init(iter, map, hash);
}

static inline void
dshmap_iter_shard_init(dshmap_iter *iter, const dshmap *map,
                       size_t shard, size_t shard_count)
{
    iter->hash = 0;
    iter->group = DSHMAP__SMALL_END;
    iter->match_group = 0;
    iter->probe = 0;
    iter->step = shard_count;
    iter->limit = 0;
    iter->occupied = 0;

    if (DSHMAP__UNLIKELY(shard_count == 0 || shard >= shard_count) ||
        !dshmap__is_allocated(map)) {
        return;
    }

    iter->group = shard;
    iter->limit = dshmap__is_small(map) ? dshmap__small_capacity(map)
                                        : map->group_mask + 1;
}

static inline void *
dshmap_iter_next(const dshmap *map, dshmap_iter *iter)
{
    if (!dshmap__is_allocated(map)) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t cap = dshmap__small_capacity(map);
        while (iter->group < cap) {
            void *entry = nodes[iter->group++].entry;
            if (entry != NULL) {
                return entry;
            }
        }
        return NULL;
    }

    for (;;) {
        if (iter->occupied) {
            size_t group = iter->group - 1;
            size_t pos = dshmap__slot_pos(
                group, dshmap__ctrl_next_match(&iter->occupied));
            return map->slots[pos];
        }
        if (iter->group > map->group_mask) {
            return NULL;
        }
        iter->occupied = dshmap__ctrl_occupied(
            dshmap__load_ctrl(map, iter->group));
        iter->group++;
    }
}

static inline void *
dshmap_iter_hash_next(const dshmap *map, dshmap_iter *iter)
{
    if (!dshmap__is_allocated(map)) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        while (iter->group != DSHMAP__SMALL_END) {
            size_t index = iter->group;
            iter->group = nodes[index].next;
            if (nodes[index].hash == iter->hash) {
                return nodes[index].entry;
            }
        }
        return NULL;
    }

    uint8_t h2 = dshmap__h2(iter->hash);
    for (;;) {
        if (iter->occupied) {
            size_t pos = dshmap__slot_pos(
                iter->match_group,
                dshmap__ctrl_next_match(&iter->occupied));
            if (dshmap__slot_hash(map, pos) == iter->hash) {
                return map->slots[pos];
            }
            continue;
        }

        if (iter->probe > map->group_mask) {
            return NULL;
        }

        size_t group = iter->group;
        dshmap__ctrl_group ctrl = dshmap__load_ctrl(map, group);
        iter->match_group = group;
        iter->occupied = dshmap__ctrl_match(ctrl, h2);
        if (dshmap__group_has_empty(ctrl)) {
            iter->probe = map->group_mask + 1;
        } else {
            iter->group = dshmap__next_group_index(map, group, iter->probe);
            iter->probe++;
        }
    }
}

static inline void *
dshmap_iter_hash_next_with_hash_fn(const dshmap *map, dshmap_iter *iter,
                                   dshmap_hash_fn hash_fn)
{
    if (!dshmap__is_allocated(map)) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        while (iter->group != DSHMAP__SMALL_END) {
            size_t index = iter->group;
            iter->group = nodes[index].next;
            if (nodes[index].hash == iter->hash) {
                return nodes[index].entry;
            }
        }
        return NULL;
    }

    if (hash_fn == NULL) {
        hash_fn = map->hash_fn;
    }

    uint8_t h2 = dshmap__h2(iter->hash);
    for (;;) {
        if (iter->occupied) {
            size_t pos = dshmap__slot_pos(
                iter->match_group,
                dshmap__ctrl_next_match(&iter->occupied));
            if (dshmap__slot_hash_with_hash_fn(map, pos, hash_fn) ==
                iter->hash) {
                return map->slots[pos];
            }
            continue;
        }

        if (iter->probe > map->group_mask) {
            return NULL;
        }

        size_t group = iter->group;
        dshmap__ctrl_group ctrl = dshmap__load_ctrl(map, group);
        iter->match_group = group;
        iter->occupied = dshmap__ctrl_match(ctrl, h2);
        if (dshmap__group_has_empty(ctrl)) {
            iter->probe = map->group_mask + 1;
        } else {
            iter->group = dshmap__next_group_index(map, group, iter->probe);
            iter->probe++;
        }
    }
}

static inline void *
dshmap_iter_hash_candidate_next(const dshmap *map, dshmap_iter *iter)
{
    if (!dshmap__is_allocated(map)) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        while (iter->group != DSHMAP__SMALL_END) {
            size_t index = iter->group;
            iter->group = nodes[index].next;
            return nodes[index].entry;
        }
        return NULL;
    }

    uint8_t h2 = dshmap__h2(iter->hash);
    for (;;) {
        if (iter->occupied) {
            size_t pos = dshmap__slot_pos(
                iter->match_group,
                dshmap__ctrl_next_match(&iter->occupied));
            return map->slots[pos];
        }

        if (iter->probe > map->group_mask) {
            return NULL;
        }

        size_t group = iter->group;
        dshmap__ctrl_group ctrl = dshmap__load_ctrl(map, group);
        iter->match_group = group;
        iter->occupied = dshmap__ctrl_match(ctrl, h2);
        if (dshmap__group_has_empty(ctrl)) {
            iter->probe = map->group_mask + 1;
        } else {
            iter->group = dshmap__next_group_index(map, group, iter->probe);
            iter->probe++;
        }
    }
}

static inline void *
dshmap_iter_shard_next(const dshmap *map, dshmap_iter *iter)
{
    if (!dshmap__is_allocated(map) || iter->group == DSHMAP__SMALL_END) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        while (iter->group < iter->limit) {
            size_t index = iter->group;
            iter->group = iter->step > iter->limit - iter->group
                        ? iter->limit
                        : iter->group + iter->step;
            if (nodes[index].entry != NULL) {
                return nodes[index].entry;
            }
        }
        return NULL;
    }

    for (;;) {
        if (iter->occupied) {
            size_t pos = dshmap__slot_pos(
                iter->match_group,
                dshmap__ctrl_next_match(&iter->occupied));
            return map->slots[pos];
        }
        if (iter->group >= iter->limit) {
            return NULL;
        }
        iter->match_group = iter->group;
        iter->occupied = dshmap__ctrl_occupied(
            dshmap__load_ctrl(map, iter->group));
        iter->group = iter->step > iter->limit - iter->group
                    ? iter->limit
                    : iter->group + iter->step;
    }
}

static inline void *
dshmap_iter_next_after(const dshmap *map, const void *entry)
{
    bool found = false;

    if (entry == NULL || !dshmap__is_allocated(map)) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t cap = dshmap__small_capacity(map);
        for (size_t i = 0; i < cap; i++) {
            if (nodes[i].entry == NULL) {
                continue;
            }
            if (found) {
                return nodes[i].entry;
            }
            if (nodes[i].entry == entry) {
                found = true;
            }
        }
        return NULL;
    }

    for (size_t group = 0; group <= map->group_mask; group++) {
        dshmap__ctrl_mask occupied = dshmap__ctrl_occupied(
            dshmap__load_ctrl(map, group));
        while (occupied) {
            size_t pos = dshmap__slot_pos(
                group, dshmap__ctrl_next_match(&occupied));
            if (found) {
                return map->slots[pos];
            }
            if (map->slots[pos] == entry) {
                found = true;
            }
        }
    }
    return NULL;
}

static inline void *
dshmap_iter_next_after_hash(const dshmap *map, const void *entry,
                            dshmap_hash_t hash)
{
    if (entry == NULL || !dshmap__is_allocated(map)) {
        return NULL;
    }

    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (nodes[index].hash == hash && nodes[index].entry == entry) {
                size_t cap = dshmap__small_capacity(map);
                for (size_t i = index + 1; i < cap; i++) {
                    if (nodes[i].entry != NULL) {
                        return nodes[i].entry;
                    }
                }
                return NULL;
            }
            index = nodes[index].next;
        }
        return NULL;
    }

    uint8_t h2 = dshmap__h2(hash);
    DSHMAP__FOR_EACH_GROUP(map, hash, group, ctrl) {
        dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
        while (match) {
            size_t slot = dshmap__ctrl_next_match(&match);
            size_t pos = dshmap__slot_pos(group, slot);
#if DSHMAP_SWISS_STORE_HASHES
            if ((map->hashes == NULL || map->hashes[pos] == hash) &&
                map->slots[pos] == entry) {
#else
            if (map->slots[pos] == entry) {
#endif
                dshmap__ctrl_mask occupied = dshmap__ctrl_occupied(ctrl);
                while (occupied) {
                    size_t next_slot = dshmap__ctrl_next_match(&occupied);
                    if (next_slot > slot) {
                        return map->slots[dshmap__slot_pos(group, next_slot)];
                    }
                }

                for (size_t next_group = group + 1;
                     next_group <= map->group_mask;
                     next_group++) {
                    occupied = dshmap__ctrl_occupied(
                        dshmap__load_ctrl(map, next_group));
                    if (occupied) {
                        return map->slots[dshmap__slot_pos(
                            next_group, dshmap__ctrl_next_match(&occupied))];
                    }
                }
                return NULL;
            }
        }

        if (dshmap__group_has_empty(ctrl)) {
            return NULL;
        }
    }
    return NULL;
}

static inline void *
dshmap__iter_next_safe(const dshmap *map, dshmap_iter *iter, const void *entry)
{
    return dshmap__is_small(map) ? dshmap_iter_next_after(map, entry)
                                 : dshmap_iter_next(map, iter);
}

static inline void
dshmap_insert(dshmap *map, void *entry, dshmap_hash_t hash)
{
    if (dshmap__is_small(map)) {
        size_t count = dshmap__checked_add(map->size, 1);
        if (DSHMAP__UNLIKELY(count > DSHMAP_SMALL_THRESHOLD)) {
            dshmap__promote_to_swiss(map, count);
            dshmap__swiss_insert_no_grow(map, entry, hash);
        } else {
            if (DSHMAP__UNLIKELY(count > dshmap__small_capacity(map))) {
                dshmap__small_grow_for_count(map, count);
            }
            dshmap__small_insert_no_grow(map, entry, hash);
        }
    } else {
        if (DSHMAP__UNLIKELY(!dshmap__is_allocated(map)) &&
            DSHMAP_SMALL_THRESHOLD != 0) {
            dshmap__small_alloc(map, dshmap__small_cap_for_count(1));
            dshmap__small_insert_no_grow(map, entry, hash);
        } else {
            dshmap__swiss_insert(map, entry, hash);
        }
    }
}

static inline void *
dshmap_find(const dshmap *map, dshmap_hash_t hash)
{
    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (nodes[index].hash == hash) {
                return nodes[index].entry;
            }
            index = nodes[index].next;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (dshmap__slot_hash(map, pos) == hash) {
                    return map->slots[pos];
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
dshmap_find_next(const dshmap *map, dshmap_hash_t hash, const void *prev)
{
    if (dshmap__is_small(map)) {
        bool found_prev = false;
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (!found_prev) {
                if (nodes[index].entry == prev) {
                    found_prev = true;
                }
            } else if (nodes[index].hash == hash) {
                return nodes[index].entry;
            }
            index = nodes[index].next;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        bool found_prev = false;
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (!found_prev) {
                    if (map->slots[pos] == prev) {
                        found_prev = true;
                    }
                    continue;
                }
                if (dshmap__slot_hash(map, pos) == hash) {
                    return map->slots[pos];
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
dshmap_find_with_hash_fn(const dshmap *map, dshmap_hash_t hash,
                         dshmap_hash_fn hash_fn)
{
    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (nodes[index].hash == hash) {
                return nodes[index].entry;
            }
            index = nodes[index].next;
        }
        return NULL;
    } else {
        if (hash_fn == NULL) {
            hash_fn = map->hash_fn;
        }

        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (dshmap__slot_hash_with_hash_fn(map, pos, hash_fn) ==
                    hash) {
                    return map->slots[pos];
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
dshmap_find_next_with_hash_fn(const dshmap *map, dshmap_hash_t hash,
                              const void *prev, dshmap_hash_fn hash_fn)
{
    if (dshmap__is_small(map)) {
        bool found_prev = false;
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (!found_prev) {
                if (nodes[index].entry == prev) {
                    found_prev = true;
                }
            } else if (nodes[index].hash == hash) {
                return nodes[index].entry;
            }
            index = nodes[index].next;
        }
        return NULL;
    } else {
        if (hash_fn == NULL) {
            hash_fn = map->hash_fn;
        }

        uint8_t h2 = dshmap__h2(hash);
        bool found_prev = false;
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (!found_prev) {
                    if (map->slots[pos] == prev) {
                        found_prev = true;
                    }
                    continue;
                }
                if (dshmap__slot_hash_with_hash_fn(map, pos, hash_fn) ==
                    hash) {
                    return map->slots[pos];
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
dshmap_find_key(const dshmap *map, dshmap_hash_t hash, const void *key,
               dshmap_key_eq_fn eq_fn)
{
    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (nodes[index].hash == hash && eq_fn(nodes[index].entry, key)) {
                return nodes[index].entry;
            }
            index = nodes[index].next;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            if (DSHMAP__UNLIKELY(match)) {
                do {
                    size_t pos = dshmap__slot_pos(
                        index, dshmap__ctrl_next_match(&match));
#if DSHMAP_SWISS_STORE_HASHES
                    if ((map->hashes == NULL || map->hashes[pos] == hash) &&
                        eq_fn(map->slots[pos], key)) {
#else
                    if (eq_fn(map->slots[pos], key)) {
#endif
                        return map->slots[pos];
                    }
                } while (match);
            }

            if (dshmap__group_has_empty(ctrl)) {
                return NULL;
            }
        }
        return NULL;
    }
}

static inline void *
dshmap_find_key_next(const dshmap *map, dshmap_hash_t hash, const void *key,
                    dshmap_key_eq_fn eq_fn, const void *prev)
{
    if (dshmap__is_small(map)) {
        bool found_prev = false;
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t index = dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (index != DSHMAP__SMALL_END) {
            if (!found_prev) {
                if (nodes[index].entry == prev) {
                    found_prev = true;
                }
            } else if (nodes[index].hash == hash &&
                       eq_fn(nodes[index].entry, key)) {
                return nodes[index].entry;
            }
            index = nodes[index].next;
        }
        return NULL;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        bool found_prev = false;
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
                if (!found_prev) {
                    if (map->slots[pos] == prev) {
                        found_prev = true;
                    }
                    continue;
                }
#if DSHMAP_SWISS_STORE_HASHES
                if ((map->hashes == NULL || map->hashes[pos] == hash) &&
                    eq_fn(map->slots[pos], key)) {
#else
                if (eq_fn(map->slots[pos], key)) {
#endif
                    return map->slots[pos];
                }
            }

            if (dshmap__group_has_empty(ctrl)) {
                return NULL;
            }
        }
        return NULL;
    }
}

static inline void
dshmap_remove(dshmap *map, const void *entry, dshmap_hash_t hash)
{
    if (dshmap__is_small(map)) {
        dshmap__small_node *nodes = dshmap__small_nodes(map);
        size_t *link = &dshmap__small_buckets(map)[
            dshmap__small_bucket_index(map, hash)];
        while (*link != DSHMAP__SMALL_END) {
            size_t index = *link;
            if (nodes[index].hash == hash && nodes[index].entry == entry) {
                dshmap__small_erase_at(map, index, link);
                return;
            }
            link = &nodes[index].next;
        }
        return;
    } else {
        uint8_t h2 = dshmap__h2(hash);
        DSHMAP__FOR_EACH_GROUP(map, hash, index, ctrl) {
            dshmap__ctrl_mask match = dshmap__ctrl_match(ctrl, h2);
            while (match) {
                size_t pos = dshmap__slot_pos(index,
                                              dshmap__ctrl_next_match(&match));
#if DSHMAP_SWISS_STORE_HASHES
                if ((map->hashes == NULL || map->hashes[pos] == hash) &&
                    map->slots[pos] == entry) {
#else
                if (map->slots[pos] == entry) {
#endif
                    dshmap__ctrl_mask empty = dshmap__group_has_empty(ctrl);
                    map->ctrl[pos] = empty ? DSHMAP__EMPTY : DSHMAP__DELETED;
#if DSHMAP_SWISS_STORE_HASHES
                    if (map->hashes != NULL) {
                        map->hashes[pos] = 0;
                    }
#endif
                    map->slots[pos] = NULL;
                    map->size--;
                    if (empty) {
                        map->growth_left++;
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

#ifdef __cplusplus
}
#endif

#endif
