#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../dshmap.h"

#ifdef DSHMAP_DENSE_THRESHOLD
#error "dshmap.h must not define legacy DSHMAP_DENSE_THRESHOLD"
#endif
#ifdef DSHMAP_DENSE_STORE_HASHES
#error "dshmap.h must not define legacy DSHMAP_DENSE_STORE_HASHES"
#endif

#if DSHMAP_DISABLE_SIMD && !DSHMAP__BACKEND_SWAR
#error "DSHMAP_DISABLE_SIMD must force the SWAR control backend"
#endif
#if DSHMAP_DISABLE_SIMD && DSHMAP__GROUP_WIDTH != 8
#error "SWAR control backend must use 8-slot groups"
#endif
#if !DSHMAP_DISABLE_SIMD && defined(__GNUC__) && !defined(__clang__) && \
    defined(__SSE2__) && !defined(__AVX2__) && !DSHMAP__BACKEND_SWAR
#error "GCC x86 without AVX2 must default to the SWAR control backend"
#endif
#if !DSHMAP_DISABLE_SIMD && defined(__AVX2__) && defined(__SSE2__) && \
    !DSHMAP__BACKEND_X86
#error "AVX2 x86 targets must use the x86 SIMD control backend"
#endif
#if !DSHMAP_DISABLE_SIMD && defined(__clang__) && defined(__SSE2__) && \
    !defined(__AVX2__) && !DSHMAP__BACKEND_SWAR
#error "Clang x86 without AVX2 must default to the SWAR control backend"
#endif
#if (DSHMAP__BACKEND_X86 || DSHMAP__BACKEND_NEON) && DSHMAP__GROUP_WIDTH != 16
#error "SIMD control backends must use 16-slot groups"
#endif
#if DSHMAP__BACKEND_SWAR && DSHMAP__GROUP_WIDTH != 8
#error "SWAR control backend must use 8-slot groups"
#endif

#define RUN_TEST(fn) do { printf("  %-40s ", #fn); fn(); printf("ok\n"); } while (0)

static dshmap_hash_t
dummy_hash(const void *entry)
{
    return (dshmap_hash_t)entry;
}

static size_t
fill_until_growth_left_zero(dshmap *map, size_t next)
{
    for (;;) {
        size_t target = dshmap_size(map) + map->growth_left;
        while (next <= target) {
            dshmap_insert(map, (void *)next, dummy_hash((void *)next));
            next++;
        }
        if (map->growth_left == 0) {
            return next;
        }
    }
}

static size_t
test_swiss_count_for_groups(size_t groups)
{
    return dshmap__growth_left_for_cap(
        dshmap__capacity_from_groups(groups));
}

static size_t
test_swiss_group_mask_for_count(size_t count)
{
    return dshmap__groups_for_count(count, 1) - 1;
}

static void
test_force_swiss(dshmap *map, size_t count)
{
    size_t reserve_count = count;
#if DSHMAP_SMALL_THRESHOLD != 0
    if (reserve_count <= DSHMAP_SMALL_THRESHOLD) {
        reserve_count = (size_t)DSHMAP_SMALL_THRESHOLD + 1;
    }
#endif
    dshmap_reserve(map, reserve_count);
    assert(!map->small);
}

static void
test_lifecycle(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    dshmap_destroy(&map);

    dshmap_init(&map, dummy_hash);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    dshmap_destroy(&map);
}

static void
test_static_initializer(void)
{
    static dshmap map = DSHMAP_INITIALIZER(dummy_hash);

    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));

    dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
    dshmap_insert(&map, (void *)2, dummy_hash((void *)2));
    assert(dshmap_size(&map) == 2);
    assert(dshmap_find(&map, dummy_hash((void *)1)) == (void *)1);
    assert(dshmap_find(&map, dummy_hash((void *)2)) == (void *)2);

    dshmap_clear(&map);
    assert(dshmap_is_empty(&map));

    dshmap_insert(&map, (void *)3, dummy_hash((void *)3));
    assert(dshmap_find(&map, dummy_hash((void *)3)) == (void *)3);

    dshmap_destroy(&map);
}

static void
test_insert_find(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    void *entry = (void *)42;
    dshmap_hash_t hash = dummy_hash(entry);
    dshmap_insert(&map, entry, hash);

    assert(dshmap_size(&map) == 1);
    assert(!dshmap_is_empty(&map));
    assert(dshmap_find(&map, hash) == entry);
    assert(dshmap_find(&map, dummy_hash((void *)99)) == NULL);

    dshmap_destroy(&map);
}

static void
test_insert_multiple(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    void *entries[] = {
        (void *)10, (void *)20, (void *)30, (void *)40, (void *)50,
    };
    size_t n = sizeof(entries) / sizeof(entries[0]);

    for (size_t i = 0; i < n; i++) {
        dshmap_insert(&map, entries[i], dummy_hash(entries[i]));
    }

    assert(dshmap_size(&map) == n);
    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&map, dummy_hash(entries[i])) == entries[i]);
    }

    dshmap_destroy(&map);
}

static dshmap_hash_t
colliding_hash(const void *entry)
{
    (void)entry;
    return 42;
}

struct keyed_entry {
    int key;
};

struct counted_entry {
    int key;
    dshmap_hash_t hash;
};

struct hashed_entry {
    int key;
    dshmap_hash_t hash;
};

struct model_entry {
    struct hashed_entry entry;
    bool live;
};

enum {
    MODEL_CAP = 128,
    MODEL_STEPS = 20000,
};

static size_t counted_hash_calls;
static size_t supplied_hash_calls;
static size_t macro_hash_calls;
static size_t macro_map_calls;
static size_t macro_shard_calls;
static size_t macro_shard_count_calls;
static uint64_t model_rng_state;

static bool
keyed_entry_eq(const void *entry, const void *key)
{
    const struct keyed_entry *e = entry;
    const int *k = key;
    return e->key == *k;
}

static dshmap_hash_t
counted_entry_hash(const void *entry)
{
    const struct counted_entry *e = entry;
    counted_hash_calls++;
    return e->hash;
}

static dshmap_hash_t
supplied_counted_entry_hash(const void *entry)
{
    const struct counted_entry *e = entry;
    supplied_hash_calls++;
    return e->hash;
}

static dshmap_hash_t
hashed_entry_hash(const void *entry)
{
    const struct hashed_entry *e = entry;
    return e->hash;
}

static dshmap_hash_t
macro_hash_arg(dshmap_hash_t hash)
{
    macro_hash_calls++;
    return hash;
}

static dshmap *
macro_map_arg(dshmap *map)
{
    macro_map_calls++;
    return map;
}

static size_t
macro_shard_arg(size_t shard)
{
    macro_shard_calls++;
    return shard;
}

static size_t
macro_shard_count_arg(size_t shard_count)
{
    macro_shard_count_calls++;
    return shard_count;
}

static bool
hashed_entry_eq(const void *entry, const void *key)
{
    const struct hashed_entry *e = entry;
    const int *k = key;
    return e->key == *k;
}

static void
model_rng_seed(uint64_t seed)
{
    model_rng_state = seed;
}

static uint64_t
model_rng_next(void)
{
    uint64_t x = model_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    model_rng_state = x;
    return x;
}

static dshmap_hash_t
model_random_hash(void)
{
    uint64_t r = model_rng_next();
    return (dshmap_hash_t)(((r >> 7) & 0x3F) << 7) | (r & 0x0F);
}

static size_t
model_index(const struct model_entry *entries, const void *entry)
{
    for (size_t i = 0; i < MODEL_CAP; i++) {
        if (&entries[i].entry == entry) {
            return i;
        }
    }
    return MODEL_CAP;
}

static size_t
model_find_free(const struct model_entry *entries, size_t start)
{
    for (size_t i = 0; i < MODEL_CAP; i++) {
        size_t idx = (start + i) % MODEL_CAP;
        if (!entries[idx].live) {
            return idx;
        }
    }
    return MODEL_CAP;
}

static size_t
model_find_live(const struct model_entry *entries, size_t start)
{
    for (size_t i = 0; i < MODEL_CAP; i++) {
        size_t idx = (start + i) % MODEL_CAP;
        if (entries[idx].live) {
            return idx;
        }
    }
    return MODEL_CAP;
}

static size_t
model_live_count(const struct model_entry *entries)
{
    size_t count = 0;
    for (size_t i = 0; i < MODEL_CAP; i++) {
        if (entries[i].live) {
            count++;
        }
    }
    return count;
}

static size_t
model_live_hash_count(const struct model_entry *entries, dshmap_hash_t hash)
{
    size_t count = 0;
    for (size_t i = 0; i < MODEL_CAP; i++) {
        if (entries[i].live && entries[i].entry.hash == hash) {
            count++;
        }
    }
    return count;
}

static bool
model_has_live_hash(const struct model_entry *entries, dshmap_hash_t hash)
{
    return model_live_hash_count(entries, hash) != 0;
}

static void
model_check_hash(const dshmap *map, const struct model_entry *entries,
                 dshmap_hash_t hash)
{
    bool seen[MODEL_CAP] = {0};
    size_t count = 0;

    for (void *entry = dshmap_find(map, hash);
         entry;
         entry = dshmap_find_next(map, hash, entry)) {
        size_t idx = model_index(entries, entry);
        assert(idx < MODEL_CAP);
        assert(entries[idx].live);
        assert(entries[idx].entry.hash == hash);
        assert(!seen[idx]);
        seen[idx] = true;
        count++;
    }

    assert(count == model_live_hash_count(entries, hash));
    for (size_t i = 0; i < MODEL_CAP; i++) {
        if (entries[i].live && entries[i].entry.hash == hash) {
            assert(seen[i]);
        }
    }
}

static void
model_check_all(const dshmap *map, const struct model_entry *entries)
{
    bool seen[MODEL_CAP] = {0};
    size_t live = model_live_count(entries);
    size_t iter_count = 0;

    assert(dshmap_size(map) == live);
    assert(dshmap_is_empty(map) == (live == 0));
    assert(dshmap_capacity(map) >= live);

    DSHMAP_FOR_EACH(entry, map) {
        size_t idx = model_index(entries, entry);
        assert(idx < MODEL_CAP);
        assert(entries[idx].live);
        assert(!seen[idx]);
        seen[idx] = true;
        iter_count++;
    }
    assert(iter_count == live);

    for (size_t i = 0; i < MODEL_CAP; i++) {
        dshmap_hash_t hash = entries[i].entry.hash;
        if (entries[i].live) {
            int key = entries[i].entry.key;
            void *found = dshmap_find(map, hash);
            size_t idx = model_index(entries, found);
            assert(seen[i]);
            assert(idx < MODEL_CAP);
            assert(entries[idx].live);
            assert(entries[idx].entry.hash == hash);
            assert(dshmap_find_key(map, hash, &key, hashed_entry_eq) ==
                   &entries[i].entry);
            assert(dshmap_find_key_next(map, hash, &key, hashed_entry_eq,
                                       &entries[i].entry) == NULL);
        } else if (!model_has_live_hash(entries, hash)) {
            assert(dshmap_find(map, hash) == NULL);
        }
    }
}

static bool
counted_entry_eq(const void *entry, const void *key)
{
    const struct counted_entry *e = entry;
    const int *k = key;
    return e->key == *k;
}

static void
test_duplicate_hashes(void)
{
    dshmap map;
    dshmap_init(&map, colliding_hash);

    void *a = (void *)1;
    void *b = (void *)2;
    void *c = (void *)3;
    dshmap_hash_t hash = 42;

    dshmap_insert(&map, a, hash);
    dshmap_insert(&map, b, hash);
    dshmap_insert(&map, c, hash);
    assert(dshmap_size(&map) == 3);

    bool found_a = false, found_b = false, found_c = false;
    size_t found_count = 0;
    void *last = NULL;
    void *e = dshmap_find(&map, hash);
    while (e) {
        found_count++;
        last = e;
        if (e == a) found_a = true;
        else if (e == b) found_b = true;
        else if (e == c) found_c = true;
        e = dshmap_find_next(&map, hash, e);
    }
    assert(found_a && found_b && found_c);
    assert(found_count == 3);
    assert(last != NULL);
    assert(dshmap_find_next(&map, hash, last) == NULL);

    dshmap_destroy(&map);
}

static void
test_same_h2_different_full_hashes(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry a = { .key = 1, .hash = 0x05 };
    struct hashed_entry b = { .key = 2, .hash = 0x85 };
    struct hashed_entry c = { .key = 3, .hash = 0x105 };

    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &b, b.hash);
    dshmap_insert(&map, &c, c.hash);

    assert(dshmap_find(&map, a.hash) == &a);
    assert(dshmap_find(&map, b.hash) == &b);
    assert(dshmap_find(&map, c.hash) == &c);
    assert(dshmap_find(&map, 0x185) == NULL);

    dshmap_destroy(&map);
}

static void
test_tombstone_preserves_probe_chain(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    dshmap_reserve(&map, 14);
    if (!map.small) {
        assert(map.group_mask == test_swiss_group_mask_for_count(14));
    }

    struct hashed_entry entries[9];
    for (size_t i = 0; i < 9; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 2) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == 9);

    dshmap_remove(&map, &entries[3], entries[3].hash);
    assert(dshmap_find(&map, entries[3].hash) == NULL);
    assert(dshmap_find(&map, entries[8].hash) == &entries[8]);

    for (size_t i = 0; i < 9; i++) {
        if (i != 3) {
            assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&map);
}

static void
test_probe_wraparound(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    size_t reserve_count = test_swiss_count_for_groups(4);
    size_t entry_count = DSHMAP__GROUP_WIDTH + 1;
    dshmap_reserve(&map, reserve_count);
    if (!map.small) {
        assert(map.group_mask == 3);
    }

    struct hashed_entry entries[DSHMAP__GROUP_WIDTH + 1];
    for (size_t i = 0; i < entry_count; i++) {
        dshmap_hash_t h1 = 3 + i * 4;
        entries[i].key = (int)i;
        entries[i].hash = (h1 << 7) | (20 + i);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == entry_count);

    for (size_t i = 0; i < entry_count; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }
    assert(dshmap_find(&map, ((dshmap_hash_t)43 << 7) | 60) == NULL);

    dshmap_remove(&map, &entries[0], entries[0].hash);
    assert(dshmap_find(&map, entries[0].hash) == NULL);
    assert(dshmap_find(&map, entries[entry_count - 1].hash) ==
           &entries[entry_count - 1]);

    dshmap_remove(&map, &entries[entry_count - 1],
                  entries[entry_count - 1].hash);
    assert(dshmap_find(&map, entries[entry_count - 1].hash) == NULL);
    assert(dshmap_size(&map) == entry_count - 2);

    dshmap_destroy(&map);
}

static void
test_reuse_deep_tombstone_before_grow(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    size_t count = test_swiss_count_for_groups(2);
    if (count <= DSHMAP__GROUP_WIDTH) {
        dshmap_destroy(&map);
        return;
    }
    dshmap_reserve(&map, count);
    if (!map.small) {
        assert(map.group_mask == 1);
    }

    struct hashed_entry entries[count + 1];
    for (size_t i = 0; i < count; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == count);
    if (!map.small) {
        assert(map.growth_left == 0);
    }

    size_t remove_idx = DSHMAP__GROUP_WIDTH / 2;
    dshmap_remove(&map, &entries[remove_idx], entries[remove_idx].hash);
    assert(dshmap_find(&map, entries[remove_idx].hash) == NULL);
    if (!map.small) {
        assert(map.growth_left == 0);
    }

    size_t mask = map.group_mask;
    entries[count].key = (int)count;
    entries[count].hash = ((dshmap_hash_t)(100 * 4) << 7) | 90;
    dshmap_insert(&map, &entries[count], entries[count].hash);

    assert(map.group_mask == mask);
    assert(dshmap_size(&map) == count);
    assert(dshmap_find(&map, entries[count].hash) == &entries[count]);

    for (size_t i = 0; i < count; i++) {
        if (i != remove_idx) {
            assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&map);
}

static void
test_swiss_reuses_tombstone_before_growth(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);
    test_force_swiss(&map, 32);

    for (size_t i = 1; i <= DSHMAP__GROUP_WIDTH; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    fill_until_growth_left_zero(&map, DSHMAP__GROUP_WIDTH + 1);

    size_t mask = map.group_mask;
    size_t size = dshmap_size(&map);
    assert(map.growth_left == 0);

    dshmap_remove(&map, (void *)1, dummy_hash((void *)1));
    assert(map.growth_left == 0);

    void *replacement = (void *)(((map.group_mask + 1) << 7) | 77);
    dshmap_insert(&map, replacement, dummy_hash(replacement));

    assert(map.group_mask == mask);
    assert(map.growth_left == 0);
    assert(dshmap_size(&map) == size);
    assert(dshmap_find(&map, dummy_hash((void *)1)) == NULL);
    assert(dshmap_find(&map, dummy_hash(replacement)) == replacement);

    dshmap_destroy(&map);
}

static void
test_swiss_single_group_upper_half(void)
{
#if DSHMAP_SMALL_THRESHOLD != 0 || DSHMAP__GROUP_WIDTH != 16
    return;
#else
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    size_t count = DSHMAP__GROUP_WIDTH - 2;
    dshmap_reserve(&map, count);
    assert(!map.small);
    assert(map.group_mask == 0);

    struct hashed_entry entries[DSHMAP__GROUP_WIDTH - 2];
    for (size_t i = 0; i < count; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    assert(dshmap_size(&map) == count);
    for (size_t i = 0; i < count; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }

    dshmap_destroy(&map);
#endif
}

static void
test_find_key_resolves_hash_collision(void)
{
    dshmap map;
    dshmap_init(&map, colliding_hash);

    struct keyed_entry a = { .key = 1 };
    struct keyed_entry b = { .key = 2 };
    struct keyed_entry c = { .key = 3 };
    dshmap_hash_t hash = 42;

    dshmap_insert(&map, &a, hash);
    dshmap_insert(&map, &b, hash);
    dshmap_insert(&map, &c, hash);

    int key = 2;
    assert(dshmap_find_key(&map, hash, &key, keyed_entry_eq) == &b);

    key = 99;
    assert(dshmap_find_key(&map, hash, &key, keyed_entry_eq) == NULL);

    dshmap_destroy(&map);
}

static void
test_find_key_next(void)
{
    dshmap map;
    dshmap_init(&map, colliding_hash);

    struct keyed_entry a = { .key = 7 };
    struct keyed_entry b = { .key = 8 };
    struct keyed_entry c = { .key = 7 };
    dshmap_hash_t hash = 42;

    dshmap_insert(&map, &a, hash);
    dshmap_insert(&map, &b, hash);
    dshmap_insert(&map, &c, hash);

    int key = 7;
    void *first = dshmap_find_key(&map, hash, &key, keyed_entry_eq);
    void *second = dshmap_find_key_next(&map, hash, &key, keyed_entry_eq, first);
    void *third = dshmap_find_key_next(&map, hash, &key, keyed_entry_eq, second);

    assert((first == &a && second == &c) ||
           (first == &c && second == &a));
    assert(third == NULL);

    dshmap_destroy(&map);
}

static void
test_find_key_does_not_rehash_candidates(void)
{
    dshmap map;
    dshmap_init(&map, counted_entry_hash);

    struct counted_entry a = { .key = 7, .hash = 42 };
    struct counted_entry b = { .key = 8, .hash = 42 };
    struct counted_entry c = { .key = 7, .hash = 42 };
    dshmap_hash_t hash = 42;

    dshmap_insert(&map, &a, hash);
    dshmap_insert(&map, &b, hash);
    dshmap_insert(&map, &c, hash);

    counted_hash_calls = 0;

    int key = 7;
    void *first = dshmap_find_key(&map, hash, &key, counted_entry_eq);
    void *second = dshmap_find_key_next(&map, hash, &key, counted_entry_eq, first);
    void *third = dshmap_find_key_next(&map, hash, &key, counted_entry_eq, second);

    assert((first == &a && second == &c) ||
           (first == &c && second == &a));
    assert(third == NULL);
    assert(counted_hash_calls == 0);

    dshmap_destroy(&map);
}

static void
test_find_key_skips_same_h2_noise(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_hash_t target_hash = 0x55;
    struct hashed_entry noise1 = { .key = 1, .hash = 0xD5 };
    struct hashed_entry a = { .key = 7, .hash = target_hash };
    struct hashed_entry noise2 = { .key = 2, .hash = 0x155 };
    struct hashed_entry b = { .key = 7, .hash = target_hash };
    struct hashed_entry noise3 = { .key = 3, .hash = 0x1D5 };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);
    dshmap_insert(&map, &noise3, noise3.hash);

    int key = 7;
    void *first = dshmap_find_key(&map, target_hash, &key, hashed_entry_eq);
    void *second = dshmap_find_key_next(&map, target_hash, &key,
                                       hashed_entry_eq, first);
    void *third = dshmap_find_key_next(&map, target_hash, &key,
                                      hashed_entry_eq, second);

    assert((first == &a && second == &b) ||
           (first == &b && second == &a));
    assert(third == NULL);

    key = 99;
    assert(dshmap_find_key(&map, target_hash, &key, hashed_entry_eq) == NULL);

    dshmap_destroy(&map);
}

static void
test_swiss_find_key_miss_paths(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    test_force_swiss(&map, 16);

    dshmap_hash_t target_hash = 0x2A;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = 0xAA };
    struct hashed_entry c = { .key = 3, .hash = 0x12A };

    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &b, b.hash);
    dshmap_insert(&map, &c, c.hash);

    int key = 99;
    assert(dshmap_find_key(&map, target_hash, &key, hashed_entry_eq) == NULL);

    key = 1;
    void *first = dshmap_find_key(&map, target_hash, &key, hashed_entry_eq);
    assert(first == &a);
    assert(dshmap_find_key_next(&map, target_hash, &key,
                                hashed_entry_eq, first) == NULL);

    dshmap_destroy(&map);
}

static void
test_small_hash_storage_policy(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap map;
    dshmap_init(&map, counted_entry_hash);

    struct counted_entry entries[8];
    size_t n = threshold < 8 ? threshold : 8;
    for (size_t i = 0; i < n; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 17 + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    assert(map.hashes == NULL);
    assert(map.small);
    dshmap__small_node *nodes = dshmap__small_nodes(&map);
    for (size_t i = 0; i < n; i++) {
        assert(nodes[i].hash == entries[i].hash);
    }
    counted_hash_calls = 0;

    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }
    assert(dshmap_find(&map, 999999) == NULL);
    assert(counted_hash_calls == 0);

    dshmap_destroy(&map);
}

static void
test_small_mode_uses_pooled_chaining(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold < 4) {
        return;
    }

    dshmap map;
    dshmap_init(&map, colliding_hash);

    struct keyed_entry entries[4] = {
        { .key = 1 },
        { .key = 2 },
        { .key = 3 },
        { .key = 4 },
    };

    for (size_t i = 0; i < 4; i++) {
        dshmap_insert(&map, &entries[i], 42);
    }

    assert(map.small);
    assert(dshmap_size(&map) == 4);
    for (size_t i = 0; i < 4; i++) {
        assert(dshmap_find(&map, 42) != NULL);
        dshmap_remove(&map, &entries[i], 42);
        assert(dshmap_size(&map) == 3 - i);
    }

    dshmap_destroy(&map);
}

static void
test_small_remove_preserves_chain(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * DSHMAP__GROUP_WIDTH + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    assert(map.small);

    dshmap_remove(&map, &entries[0], entries[0].hash);
    assert(dshmap_find(&map, entries[0].hash) == NULL);
    for (size_t i = 1; i < 8; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }

    struct hashed_entry extra = {
        .key = 99,
        .hash = (dshmap_hash_t)(99 * DSHMAP__GROUP_WIDTH + 1),
    };
    dshmap_insert(&map, &extra, extra.hash);
    assert(dshmap_find(&map, extra.hash) == &extra);
    assert(dshmap_size(&map) == 8);

    dshmap_destroy(&map);
}

static void
test_small_mode_reuses_free_list_nodes(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(map.small);

    dshmap_remove(&map, &entries[2], entries[2].hash);
    dshmap_remove(&map, &entries[5], entries[5].hash);

    struct hashed_entry extra1 = { .key = 99, .hash = 99 };
    struct hashed_entry extra2 = { .key = 100, .hash = 100 };
    dshmap_insert(&map, &extra1, extra1.hash);
    dshmap_insert(&map, &extra2, extra2.hash);

    dshmap__small_node *nodes = dshmap__small_nodes(&map);
    assert(nodes[5].entry == &extra1);
    assert(nodes[2].entry == &extra2);
    assert(dshmap_find(&map, extra1.hash) == &extra1);
    assert(dshmap_find(&map, extra2.hash) == &extra2);

    dshmap_destroy(&map);
}

static void
test_small_promotes_to_swiss_at_threshold(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    size_t n = threshold + 1;
    struct hashed_entry *entries = malloc(n * sizeof(*entries));
    assert(entries != NULL);

    for (size_t i = 0; i < threshold; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 2654435761u + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
        assert(map.small);
    }

    entries[threshold].key = (int)threshold;
    entries[threshold].hash = (dshmap_hash_t)(threshold * 2654435761u + 1);
    dshmap_insert(&map, &entries[threshold], entries[threshold].hash);

    assert(!map.small);
#if DSHMAP_SWISS_STORE_HASHES
    assert(map.hashes != NULL);
#else
    assert(map.hashes == NULL);
#endif
    assert(dshmap_size(&map) == n);
    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }

    free(entries);
    dshmap_destroy(&map);
}

static void
test_small_to_swiss_post_promotion_operations(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold < 4) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    size_t n = threshold + 3;
    struct hashed_entry *entries = malloc(n * sizeof(*entries));
    bool *seen = calloc(n, sizeof(*seen));
    assert(entries != NULL);
    assert(seen != NULL);

    dshmap_hash_t shared_hash = 0x55;
    for (size_t i = 0; i < n; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i + 1) << 8) | 0xA6;
    }
    entries[0].key = 777;
    entries[0].hash = shared_hash;
    entries[1].key = 777;
    entries[1].hash = shared_hash;
    entries[2].key = 888;
    entries[2].hash = shared_hash;
    entries[3].key = 999;
    entries[3].hash = shared_hash + 0x80;

    for (size_t i = 0; i < threshold; i++) {
        dshmap_insert(&map, &entries[i], entries[i].hash);
        assert(map.small);
    }

    size_t shared_count = 0;
    for (void *e = dshmap_find(&map, shared_hash); e;
         e = dshmap_find_next(&map, shared_hash, e)) {
        assert(e == &entries[0] || e == &entries[1] || e == &entries[2]);
        shared_count++;
    }
    assert(shared_count == 3);

    int key = 777;
    void *first = dshmap_find_key(&map, shared_hash, &key, hashed_entry_eq);
    void *second = dshmap_find_key_next(&map, shared_hash, &key,
                                       hashed_entry_eq, first);
    void *third = dshmap_find_key_next(&map, shared_hash, &key,
                                      hashed_entry_eq, second);
    assert((first == &entries[0] && second == &entries[1]) ||
           (first == &entries[1] && second == &entries[0]));
    assert(third == NULL);

    dshmap_insert(&map, &entries[threshold], entries[threshold].hash);
    assert(!map.small);
    assert(dshmap_size(&map) == threshold + 1);

    shared_count = 0;
    for (void *e = dshmap_find(&map, shared_hash); e;
         e = dshmap_find_next(&map, shared_hash, e)) {
        assert(e == &entries[0] || e == &entries[1] || e == &entries[2]);
        shared_count++;
    }
    assert(shared_count == 3);

    first = dshmap_find_key(&map, shared_hash, &key, hashed_entry_eq);
    second = dshmap_find_key_next(&map, shared_hash, &key,
                                 hashed_entry_eq, first);
    third = dshmap_find_key_next(&map, shared_hash, &key,
                                hashed_entry_eq, second);
    assert((first == &entries[0] && second == &entries[1]) ||
           (first == &entries[1] && second == &entries[0]));
    assert(third == NULL);

    dshmap_remove(&map, &entries[1], entries[1].hash);
    dshmap_remove(&map, &entries[threshold], entries[threshold].hash);
    assert(dshmap_size(&map) == threshold - 1);
    assert(dshmap_find(&map, entries[threshold].hash) == NULL);
    assert(dshmap_find_key(&map, shared_hash, &key, hashed_entry_eq) ==
           &entries[0]);
    assert(dshmap_find_key_next(&map, shared_hash, &key,
                                hashed_entry_eq, &entries[0]) == NULL);

    shared_count = 0;
    for (void *e = dshmap_find(&map, shared_hash); e;
         e = dshmap_find_next(&map, shared_hash, e)) {
        assert(e == &entries[0] || e == &entries[2]);
        shared_count++;
    }
    assert(shared_count == 2);

    DSHMAP_FOR_EACH(entry, &map) {
        struct hashed_entry *e = entry;
        assert(e >= entries && e < entries + n);
        seen[(size_t)(e - entries)] = true;
    }
    for (size_t i = 0; i <= threshold; i++) {
        assert(seen[i] == (i != 1 && i != threshold));
    }

    dshmap_clear(&map);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    DSHMAP_FOR_EACH(entry, &map) {
        (void)entry;
        assert(0 && "cleared promoted table yielded an entry");
    }
    for (size_t i = 0; i <= threshold; i++) {
        assert(dshmap_find(&map, entries[i].hash) == NULL);
    }

    dshmap_insert(&map, &entries[threshold + 1],
                  entries[threshold + 1].hash);
    dshmap_insert(&map, &entries[threshold + 2],
                  entries[threshold + 2].hash);
    assert(!map.small);
    assert(dshmap_size(&map) == 2);
    assert(dshmap_find(&map, entries[threshold + 1].hash) ==
           &entries[threshold + 1]);
    assert(dshmap_find(&map, entries[threshold + 2].hash) ==
           &entries[threshold + 2]);

    free(seen);
    free(entries);
    dshmap_destroy(&map);
}

static void
test_reserve_above_small_threshold_uses_swiss(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_reserve(&map, threshold + 1);
    assert(map.slots != NULL);
    assert(!map.small);
#if DSHMAP_SWISS_STORE_HASHES
    assert(map.hashes != NULL);
#else
    assert(map.hashes == NULL);
#endif

    struct hashed_entry entries[16];
    for (size_t i = 0; i < 16; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    for (size_t i = 0; i < 16; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }

    dshmap_destroy(&map);
}

static void
test_swiss_hash_storage_policy(void)
{
    dshmap map;
    dshmap_init(&map, counted_entry_hash);

    size_t reserve_n = (size_t)DSHMAP_SMALL_THRESHOLD + 1;
    if (reserve_n < 16) {
        reserve_n = 16;
    }
    dshmap_reserve(&map, reserve_n);

    assert(map.slots != NULL);
    assert(!map.small);
#if DSHMAP_SWISS_STORE_HASHES
    assert(map.hashes != NULL);
#else
    assert(map.hashes == NULL);
#endif

    struct counted_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 17 + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    counted_hash_calls = 0;
    for (size_t i = 0; i < 8; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }
#if DSHMAP_SWISS_STORE_HASHES
    assert(counted_hash_calls == 0);
#else
    assert(counted_hash_calls > 0);
#endif

    dshmap_destroy(&map);
}

static void
test_remove(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    void *c = (void *)30;
    dshmap_insert(&map, a, dummy_hash(a));
    dshmap_insert(&map, b, dummy_hash(b));
    dshmap_insert(&map, c, dummy_hash(c));

    dshmap_remove(&map, b, dummy_hash(b));
    assert(dshmap_size(&map) == 2);
    assert(dshmap_find(&map, dummy_hash(b)) == NULL);
    assert(dshmap_find(&map, dummy_hash(a)) == a);
    assert(dshmap_find(&map, dummy_hash(c)) == c);

    dshmap_remove(&map, a, dummy_hash(a));
    assert(dshmap_size(&map) == 1);
    assert(dshmap_find(&map, dummy_hash(a)) == NULL);
    assert(dshmap_find(&map, dummy_hash(c)) == c);

    dshmap_remove(&map, c, dummy_hash(c));
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));

    dshmap_destroy(&map);
}

static void
test_clear(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    for (size_t i = 1; i <= 10; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == 10);

    dshmap_clear(&map);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    for (size_t i = 1; i <= 10; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == NULL);
    }

    for (size_t i = 100; i <= 105; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == 6);
    for (size_t i = 100; i <= 105; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&map);
}

static void
test_clear_after_tombstone_heavy_table(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    dshmap_reserve(&map, 28);

    struct hashed_entry entries[28];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == 28);

    for (size_t i = 0; i < 20; i++) {
        dshmap_remove(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == 8);

    dshmap_clear(&map);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));

    size_t count = 0;
    DSHMAP_FOR_EACH(entry, &map) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    for (size_t i = 0; i < 28; i++) {
        assert(dshmap_find(&map, entries[i].hash) == NULL);
    }

    struct hashed_entry fresh[5];
    for (size_t i = 0; i < 5; i++) {
        fresh[i].key = (int)i + 100;
        fresh[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (40 + i);
        dshmap_insert(&map, &fresh[i], fresh[i].hash);
    }
    assert(dshmap_size(&map) == 5);

    for (size_t i = 0; i < 5; i++) {
        assert(dshmap_find(&map, fresh[i].hash) == &fresh[i]);
    }

    dshmap_destroy(&map);
}

static void
test_reserve(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    dshmap_reserve(&map, 100);
    size_t mask_after_reserve = map.group_mask;
    assert(mask_after_reserve > 0);

    for (size_t i = 1; i <= 100; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(map.group_mask == mask_after_reserve);
    assert(dshmap_size(&map) == 100);

    for (size_t i = 1; i <= 100; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&map);
}

static void
test_reserve_zero_noop(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    dshmap_reserve(&map, 0);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    assert(map.slots == NULL);

    dshmap_destroy(&map);
}

static void
test_reserve_noop(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    dshmap_reserve(&map, 5);
    size_t mask = map.group_mask;

    for (size_t i = 1; i <= 5; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }

    dshmap_reserve(&map, 3);
    assert(map.group_mask == mask);

    dshmap_destroy(&map);
}

static void
test_reserve_promotes_small_table(void)
{
    size_t threshold = DSHMAP_SMALL_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entry = { .key = 1, .hash = 1 };
    dshmap_insert(&map, &entry, entry.hash);
    assert(map.small);

    dshmap_reserve(&map, threshold + 1);
    assert(!map.small);
    assert(dshmap_size(&map) == 1);
    assert(dshmap_find(&map, entry.hash) == &entry);

    dshmap_destroy(&map);
}

static void
test_swiss_reserve_noop_and_growth(void)
{
    size_t reserve_n = (size_t)DSHMAP_SMALL_THRESHOLD + 1;
    if (reserve_n < 16) {
        reserve_n = 16;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_reserve(&map, reserve_n);
    assert(!map.small);

    size_t old_mask = map.group_mask;
    dshmap_reserve(&map, 1);
    assert(map.group_mask == old_mask);

    dshmap_reserve(&map, map.size + map.growth_left + 1);
    assert(map.group_mask > old_mask);

    dshmap_destroy(&map);
}

static void
test_reserve_after_tombstone_churn(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    dshmap_reserve(&map, 28);

    struct hashed_entry entries[28];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == 28);

    size_t removed = 0;
    for (size_t i = 0; i < 28; i += 3) {
        dshmap_remove(&map, &entries[i], entries[i].hash);
        removed++;
    }
    assert(dshmap_size(&map) == 28 - removed);

    size_t old_mask = map.group_mask;
    dshmap_reserve(&map, 100);
    assert(map.group_mask > old_mask);
    assert(dshmap_size(&map) == 28 - removed);

    for (size_t i = 0; i < 28; i++) {
        if (i % 3 == 0) {
            assert(dshmap_find(&map, entries[i].hash) == NULL);
        } else {
            assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&map);
}

static void
test_capacity_empty(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    assert(dshmap_capacity(&map) == 0);

    dshmap_destroy(&map);
}

static void
test_capacity_small_table(void)
{
#if DSHMAP_SMALL_THRESHOLD == 0
    return;
#else
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t reserve_count = DSHMAP_SMALL_THRESHOLD < 5 ?
        (size_t)DSHMAP_SMALL_THRESHOLD : 5;
    dshmap_reserve(&map, reserve_count);
    assert(map.small);

    size_t expected = dshmap__small_capacity(&map);
    if (expected > DSHMAP_SMALL_THRESHOLD) {
        expected = DSHMAP_SMALL_THRESHOLD;
    }
    assert(dshmap_capacity(&map) == expected);
    assert(dshmap_capacity(&map) >= reserve_count);

    for (size_t i = 1; i <= reserve_count; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_capacity(&map) == expected);

    dshmap_destroy(&map);
#endif
}

static void
test_capacity_swiss_table(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    test_force_swiss(&map, 128);
    assert(!map.small);

    size_t expected = dshmap__growth_left_for_cap(
        dshmap__capacity_from_groups(map.group_mask + 1));
    assert(dshmap_capacity(&map) == expected);

    struct hashed_entry entries[16];
    for (size_t i = 0; i < 16; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_capacity(&map) == expected);

    for (size_t i = 0; i < 16; i += 2) {
        dshmap_remove(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_capacity(&map) == expected);

    dshmap_shrink(&map);
    expected = dshmap__growth_left_for_cap(
        dshmap__capacity_from_groups(map.group_mask + 1));
    assert(dshmap_capacity(&map) == expected);
    assert(dshmap_capacity(&map) >= dshmap_size(&map));

    dshmap_destroy(&map);
}

static void
test_shrink_empty_frees_storage(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    dshmap_reserve(&map, (size_t)DSHMAP_SMALL_THRESHOLD + 1);
    assert(map.slots != NULL);

    dshmap_shrink(&map);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    assert(map.slots == NULL);
    assert(map.hash_fn == dummy_hash);

    dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
    assert(dshmap_find(&map, dummy_hash((void *)1)) == (void *)1);

    dshmap_destroy(&map);
}

static void
test_shrink_small_table(void)
{
#if DSHMAP_SMALL_THRESHOLD < 8
    return;
#else
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t reserve_count = DSHMAP_SMALL_THRESHOLD < 64 ?
        (size_t)DSHMAP_SMALL_THRESHOLD : 64;
    dshmap_reserve(&map, reserve_count);
    assert(map.small);
    size_t old_cap = dshmap__small_capacity(&map);

    size_t live_count = reserve_count / 2;
    for (size_t i = 1; i <= live_count; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }

    dshmap_shrink(&map);
    assert(map.small);
    assert(dshmap__small_capacity(&map) == dshmap__small_cap_for_count(live_count));
    assert(dshmap__small_capacity(&map) < old_cap);
    assert(dshmap_size(&map) == live_count);

    for (size_t i = 1; i <= live_count; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&map);
#endif
}

static void
test_shrink_swiss_table_does_not_demote(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    test_force_swiss(&map, 128);
    assert(!map.small);
    size_t old_mask = map.group_mask;

    struct hashed_entry entries[16];
    for (size_t i = 0; i < 16; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    for (size_t i = 0; i < 16; i += 2) {
        dshmap_remove(&map, &entries[i], entries[i].hash);
    }

    dshmap_shrink(&map);
    assert(!map.small);
    assert(map.group_mask < old_mask);
    assert(dshmap_size(&map) == 8);

    for (size_t i = 0; i < 16; i++) {
        if (i % 2 == 0) {
            assert(dshmap_find(&map, entries[i].hash) == NULL);
        } else {
            assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&map);
}

static void
test_growth(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t n = 500;
    size_t prev_mask = 0;
    int resizes = 0;

    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
        if (map.group_mask != prev_mask) {
            resizes++;
            prev_mask = map.group_mask;

            for (size_t j = 1; j <= i; j++) {
                assert(dshmap_find(&map, dummy_hash((void *)j)) == (void *)j);
            }
        }
    }

    assert(resizes >= 3);
    assert(dshmap_size(&map) == n);

    for (size_t i = 1; i <= n; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&map);
}

static void
test_large_table(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t n = 50000;
    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == n);

    for (size_t i = 1; i <= n; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    assert(dshmap_find(&map, dummy_hash((void *)(n + 1))) == NULL);
    assert(dshmap_find(&map, dummy_hash((void *)(n + 1000))) == NULL);

    dshmap_destroy(&map);
}

static void
test_iteration(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t n = 200;
    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }

    bool seen[201] = {0};
    size_t count = 0;
    DSHMAP_FOR_EACH(entry, &map) {
        size_t val = (size_t)entry;
        assert(val >= 1 && val <= n);
        assert(!seen[val]);
        seen[val] = true;
        count++;
    }
    assert(count == n);

    dshmap_destroy(&map);
}

static void
test_iteration_empty(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t count = 0;
    DSHMAP_FOR_EACH(entry, &map) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    dshmap_destroy(&map);
}

static void
test_iterator_api(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t n = 64;
    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }

    bool seen[65] = {0};
    size_t count = 0;
    dshmap_iter iter;
    dshmap_iter_init(&iter, &map);
    for (void *entry = dshmap_iter_next(&map, &iter);
         entry;
         entry = dshmap_iter_next(&map, &iter)) {
        size_t val = (size_t)entry;
        assert(val >= 1 && val <= n);
        assert(!seen[val]);
        seen[val] = true;
        count++;
    }
    assert(count == n);
    assert(dshmap_iter_next(&map, &iter) == NULL);

    dshmap_destroy(&map);
}

enum {
    SHARD_TEST_CAP = 96,
};

static size_t
shard_entry_index(const struct hashed_entry *entries, size_t n,
                  const void *entry)
{
    for (size_t i = 0; i < n; i++) {
        if (&entries[i] == entry) {
            return i;
        }
    }
    return n;
}

static void
shard_check_entry(const struct hashed_entry *entries, const bool *live,
                  size_t n, void *entry, bool *seen, size_t *count)
{
    size_t idx = shard_entry_index(entries, n, entry);
    assert(idx < n);
    assert(live[idx]);
    assert(!seen[idx]);
    seen[idx] = true;
    (*count)++;
}

static void
check_shards_cover_entries(const dshmap *map, struct hashed_entry *entries,
                           const bool *live, size_t n, size_t shard_count,
                           bool use_macro)
{
    bool seen[SHARD_TEST_CAP] = {0};
    size_t count = 0;
    size_t live_count = 0;

    assert(n <= SHARD_TEST_CAP);
    for (size_t i = 0; i < n; i++) {
        if (live[i]) {
            live_count++;
        }
    }

    for (size_t shard = 0; shard < shard_count; shard++) {
        if (use_macro) {
            DSHMAP_FOR_EACH_SHARD(entry, map, shard, shard_count) {
                shard_check_entry(entries, live, n, entry, seen, &count);
            }
        } else {
            dshmap_iter iter;
            dshmap_iter_shard_init(&iter, map, shard, shard_count);
            for (void *entry = dshmap_iter_shard_next(map, &iter);
                 entry;
                 entry = dshmap_iter_shard_next(map, &iter)) {
                shard_check_entry(entries, live, n, entry, seen, &count);
            }
            assert(dshmap_iter_shard_next(map, &iter) == NULL);
        }
    }

    assert(count == live_count);
    for (size_t i = 0; i < n; i++) {
        assert(seen[i] == live[i]);
    }
}

static void
test_shard_iteration_empty(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    for (size_t shard = 0; shard < 7; shard++) {
        dshmap_iter iter;
        dshmap_iter_shard_init(&iter, &map, shard, 7);
        assert(dshmap_iter_shard_next(&map, &iter) == NULL);
    }

    size_t count = 0;
    DSHMAP_FOR_EACH_SHARD(entry, &map, 3, 7) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    dshmap_destroy(&map);
}

static void
test_shard_iteration_small_table(void)
{
    if (DSHMAP_SMALL_THRESHOLD < 12) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entries[12];
    bool live[12] = {0};
    for (size_t i = 0; i < 10; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)((i % 3) * 16 + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
        live[i] = true;
    }

    dshmap_remove(&map, &entries[2], entries[2].hash);
    dshmap_remove(&map, &entries[7], entries[7].hash);
    live[2] = false;
    live[7] = false;

    for (size_t i = 10; i < 12; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)((i % 3) * 16 + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
        live[i] = true;
    }
    assert(map.small);

    check_shards_cover_entries(&map, entries, live, 12, 1, false);
    check_shards_cover_entries(&map, entries, live, 12, 2, false);
    check_shards_cover_entries(&map, entries, live, 12, 3, true);
    check_shards_cover_entries(&map, entries, live, 12, 32, true);

    dshmap_destroy(&map);
}

static void
test_shard_iteration_swiss_table(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    test_force_swiss(&map, 80);

    struct hashed_entry entries[80];
    bool live[80] = {0};
    for (size_t i = 0; i < 80; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i % 11) << 7) | (i & 0x7f);
        dshmap_insert(&map, &entries[i], entries[i].hash);
        live[i] = true;
    }

    for (size_t i = 0; i < 80; i += 7) {
        dshmap_remove(&map, &entries[i], entries[i].hash);
        live[i] = false;
    }
    assert(!map.small);

    check_shards_cover_entries(&map, entries, live, 80, 1, false);
    check_shards_cover_entries(&map, entries, live, 80, 2, false);
    check_shards_cover_entries(&map, entries, live, 80, 5, true);
    check_shards_cover_entries(&map, entries, live, 80,
                               map.group_mask + 3, true);

    dshmap_iter iter;
    dshmap_iter_shard_init(&iter, &map, map.group_mask + 1,
                           map.group_mask + 3);
    assert(dshmap_iter_shard_next(&map, &iter) == NULL);

    dshmap_destroy(&map);
}

static void
test_for_each_shard_evaluates_args_once(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    for (size_t i = 1; i <= 4; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }

    macro_map_calls = 0;
    macro_shard_calls = 0;
    macro_shard_count_calls = 0;
    size_t count = 0;
    DSHMAP_FOR_EACH_SHARD(entry, macro_map_arg(&map),
                          macro_shard_arg(0),
                          macro_shard_count_arg(1)) {
        (void)entry;
        count++;
    }

    assert(count == 4);
    assert(macro_map_calls == 1);
    assert(macro_shard_calls == 1);
    assert(macro_shard_count_calls == 1);

    dshmap_destroy(&map);
}

static void
test_hash_iterator_api(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_hash_t target_hash = 0x2A;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = target_hash };
    struct hashed_entry c = { .key = 3, .hash = target_hash };
    struct hashed_entry noise1 = { .key = 4, .hash = 0xAA };
    struct hashed_entry noise2 = { .key = 5, .hash = 0x12A };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);
    dshmap_insert(&map, &c, c.hash);

    dshmap_iter iter;
    dshmap_iter_hash_init(&iter, &map, target_hash);

    bool found_a = false, found_b = false, found_c = false;
    size_t count = 0;
    for (void *entry = dshmap_iter_hash_next(&map, &iter);
         entry;
         entry = dshmap_iter_hash_next(&map, &iter)) {
        count++;
        if (entry == &a) found_a = true;
        else if (entry == &b) found_b = true;
        else if (entry == &c) found_c = true;
        else assert(0 && "hash iterator returned same-H2 noise");
    }
    assert(count == 3);
    assert(found_a && found_b && found_c);
    assert(dshmap_iter_hash_next(&map, &iter) == NULL);

    dshmap_iter_hash_init(&iter, &map, 0x1234);
    assert(dshmap_iter_hash_next(&map, &iter) == NULL);

#if DSHMAP_SMALL_THRESHOLD != 0
    dshmap_reserve(&map, DSHMAP_SMALL_THRESHOLD + 1);
#endif
    assert(!map.small);

    dshmap_iter_hash_init(&iter, &map, target_hash);
    found_a = found_b = found_c = false;
    count = 0;
    for (void *entry = dshmap_iter_hash_next(&map, &iter);
         entry;
         entry = dshmap_iter_hash_next(&map, &iter)) {
        count++;
        if (entry == &a) found_a = true;
        else if (entry == &b) found_b = true;
        else if (entry == &c) found_c = true;
        else assert(0 && "Swiss hash iterator returned same-H2 noise");
    }
    assert(count == 3);
    assert(found_a && found_b && found_c);

    dshmap_iter_hash_init(&iter, &map, 0x1234);
    assert(dshmap_iter_hash_next(&map, &iter) == NULL);

    dshmap_destroy(&map);
}

static void
test_hash_candidate_iterator_small_table(void)
{
    if (DSHMAP_SMALL_THRESHOLD < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    dshmap_reserve(&map, 8);

    dshmap_hash_t target_hash = 0x11;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = target_hash };
    struct hashed_entry noise1 = { .key = 3, .hash = 0x19 };
    struct hashed_entry noise2 = { .key = 4, .hash = 0x21 };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);
    assert(map.small);

    dshmap_iter iter;
    dshmap_iter_hash_candidate_init(&iter, &map, target_hash);

    bool found_a = false, found_b = false, found_noise = false;
    size_t exact_count = 0;
    size_t candidate_count = 0;
    for (void *entry = dshmap_iter_hash_candidate_next(&map, &iter);
         entry;
         entry = dshmap_iter_hash_candidate_next(&map, &iter)) {
        struct hashed_entry *e = entry;
        candidate_count++;
        if (e->hash == target_hash) {
            exact_count++;
            if (e == &a) found_a = true;
            else if (e == &b) found_b = true;
            else assert(0 && "candidate iterator returned wrong exact hash");
        } else {
            found_noise = true;
        }
    }

    assert(candidate_count == 4);
    assert(exact_count == 2);
    assert(found_a && found_b && found_noise);
    assert(dshmap_iter_hash_candidate_next(&map, &iter) == NULL);

    dshmap_iter_hash_candidate_init(&iter, &map, 0x12);
    assert(dshmap_iter_hash_candidate_next(&map, &iter) == NULL);

    dshmap_destroy(&map);
}

static void
test_hash_candidate_iterator_swiss_table(void)
{
    dshmap map;
    dshmap_init(&map, counted_entry_hash);
    test_force_swiss(&map, 32);

    size_t groups = map.group_mask + 1;
    dshmap_hash_t target_hash = 0x2A;
    struct counted_entry a = { .key = 1, .hash = target_hash };
    struct counted_entry b = { .key = 2, .hash = target_hash };
    struct counted_entry noise1 = {
        .key = 3,
        .hash = ((dshmap_hash_t)groups << 7) | dshmap__h2(target_hash),
    };
    struct counted_entry noise2 = {
        .key = 4,
        .hash = ((dshmap_hash_t)(groups * 2) << 7) | dshmap__h2(target_hash),
    };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);
    assert(!map.small);

    counted_hash_calls = 0;

    dshmap_iter iter;
    dshmap_iter_hash_candidate_init(&iter, &map, target_hash);

    bool found_a = false, found_b = false, found_noise1 = false;
    bool found_noise2 = false;
    size_t exact_count = 0;
    size_t candidate_count = 0;
    for (void *entry = dshmap_iter_hash_candidate_next(&map, &iter);
         entry;
         entry = dshmap_iter_hash_candidate_next(&map, &iter)) {
        struct counted_entry *e = entry;
        candidate_count++;
        if (e->hash == target_hash) {
            exact_count++;
            if (e == &a) found_a = true;
            else if (e == &b) found_b = true;
            else assert(0 && "candidate iterator returned wrong exact hash");
        } else if (e == &noise1) {
            found_noise1 = true;
        } else if (e == &noise2) {
            found_noise2 = true;
        } else {
            assert(0 && "candidate iterator returned wrong noise");
        }
    }

    assert(candidate_count == 4);
    assert(exact_count == 2);
    assert(found_a && found_b && found_noise1 && found_noise2);
    assert(counted_hash_calls == 0);

    dshmap_iter_hash_candidate_init(&iter, &map, 0x1234);
    assert(dshmap_iter_hash_candidate_next(&map, &iter) == NULL);
    assert(counted_hash_calls == 0);

    dshmap_destroy(&map);
}

static void
test_find_with_hash_fn_uses_supplied_hash(void)
{
    dshmap map;
    dshmap_init(&map, counted_entry_hash);
    test_force_swiss(&map, 32);

    size_t groups = map.group_mask + 1;
    dshmap_hash_t target_hash = 0x2A;
    struct counted_entry a = { .key = 1, .hash = target_hash };
    struct counted_entry b = { .key = 2, .hash = target_hash };
    struct counted_entry noise1 = {
        .key = 3,
        .hash = ((dshmap_hash_t)groups << 7) | dshmap__h2(target_hash),
    };
    struct counted_entry noise2 = {
        .key = 4,
        .hash = ((dshmap_hash_t)(groups * 2) << 7) | dshmap__h2(target_hash),
    };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);

    counted_hash_calls = 0;
    supplied_hash_calls = 0;

    void *first = dshmap_find_with_hash_fn(&map, target_hash,
                                           supplied_counted_entry_hash);
    void *second = dshmap_find_next_with_hash_fn(
        &map, target_hash, first, supplied_counted_entry_hash);
    void *third = dshmap_find_next_with_hash_fn(
        &map, target_hash, second, supplied_counted_entry_hash);

    assert((first == &a && second == &b) ||
           (first == &b && second == &a));
    assert(third == NULL);
    assert(counted_hash_calls == 0);
#if DSHMAP_SWISS_STORE_HASHES
    assert(supplied_hash_calls == 0);
#else
    assert(supplied_hash_calls > 0);
#endif

    dshmap_destroy(&map);
}

static void
test_hash_iterator_with_hash_fn_uses_supplied_hash(void)
{
    dshmap map;
    dshmap_init(&map, counted_entry_hash);
    test_force_swiss(&map, 32);

    size_t groups = map.group_mask + 1;
    dshmap_hash_t target_hash = 0x55;
    struct counted_entry a = { .key = 1, .hash = target_hash };
    struct counted_entry b = { .key = 2, .hash = target_hash };
    struct counted_entry noise1 = {
        .key = 3,
        .hash = ((dshmap_hash_t)groups << 7) | dshmap__h2(target_hash),
    };
    struct counted_entry noise2 = {
        .key = 4,
        .hash = ((dshmap_hash_t)(groups * 2) << 7) | dshmap__h2(target_hash),
    };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);

    counted_hash_calls = 0;
    supplied_hash_calls = 0;

    dshmap_iter iter;
    dshmap_iter_hash_init(&iter, &map, target_hash);

    bool found_a = false, found_b = false;
    size_t count = 0;
    for (void *entry = dshmap_iter_hash_next_with_hash_fn(
             &map, &iter, supplied_counted_entry_hash);
         entry;
         entry = dshmap_iter_hash_next_with_hash_fn(
             &map, &iter, supplied_counted_entry_hash)) {
        count++;
        if (entry == &a) found_a = true;
        else if (entry == &b) found_b = true;
        else assert(0 && "hash-fn iterator returned same-H2 noise");
    }

    assert(count == 2);
    assert(found_a && found_b);
    assert(counted_hash_calls == 0);
#if DSHMAP_SWISS_STORE_HASHES
    assert(supplied_hash_calls == 0);
#else
    assert(supplied_hash_calls > 0);
#endif

    dshmap_destroy(&map);
}

static void
test_swiss_iter_next_after(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    assert(dshmap_iter_next_after(&map, (void *)1) == NULL);
    test_force_swiss(&map, 32);
    assert(dshmap_iter_next_after(&map, NULL) == NULL);

    void *entries[32];
    for (size_t i = 0; i < 32; i++) {
        entries[i] = (void *)(i + 1);
        dshmap_insert(&map, entries[i], dummy_hash(entries[i]));
    }

    void *seen[32];
    size_t count = 0;
    dshmap_iter iter;
    dshmap_iter_init(&iter, &map);
    for (void *entry = dshmap_iter_next(&map, &iter);
         entry;
         entry = dshmap_iter_next(&map, &iter)) {
        assert(count < 32);
        seen[count++] = entry;
    }
    assert(count == 32);

    for (size_t i = 0; i + 1 < count; i++) {
        assert(dshmap_iter_next_after(&map, seen[i]) == seen[i + 1]);
    }
    assert(dshmap_iter_next_after(&map, seen[count - 1]) == NULL);

    dshmap_destroy(&map);
}

static void
test_small_iter_next_after_hash(void)
{
    if (DSHMAP_SMALL_THRESHOLD < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    assert(dshmap_iter_next_after_hash(&map, NULL, 0) == NULL);
    assert(dshmap_iter_next_after_hash(&map, (void *)1, 1) == NULL);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(0x100 + i * 0x80);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(map.small);

    dshmap_remove(&map, &entries[2], entries[2].hash);
    dshmap_remove(&map, &entries[5], entries[5].hash);

    void *seen[8];
    size_t count = 0;
    dshmap_iter iter;
    dshmap_iter_init(&iter, &map);
    for (void *entry = dshmap_iter_next(&map, &iter);
         entry;
         entry = dshmap_iter_next(&map, &iter)) {
        assert(count < 8);
        seen[count++] = entry;
    }
    assert(count == 6);

    for (size_t i = 0; i + 1 < count; i++) {
        struct hashed_entry *e = seen[i];
        assert(dshmap_iter_next_after_hash(&map, e, e->hash) == seen[i + 1]);
    }
    struct hashed_entry *last = seen[count - 1];
    assert(dshmap_iter_next_after_hash(&map, last, last->hash) == NULL);

    struct hashed_entry missing = { .key = 99, .hash = 0x100 };
    assert(dshmap_iter_next_after_hash(&map, &missing, missing.hash) == NULL);

    dshmap_destroy(&map);
}

static void
test_swiss_iter_next_after_hash(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    assert(dshmap_iter_next_after_hash(&map, NULL, 0) == NULL);
    assert(dshmap_iter_next_after_hash(&map, (void *)1, 1) == NULL);

    test_force_swiss(&map, 96);
    assert(!map.small);

    struct hashed_entry entries[48];
    for (size_t i = 0; i < 48; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(0x4000 + (i % 24) * 0x80);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    dshmap_remove(&map, &entries[7], entries[7].hash);
    dshmap_remove(&map, &entries[31], entries[31].hash);

    void *seen[48];
    size_t count = 0;
    dshmap_iter iter;
    dshmap_iter_init(&iter, &map);
    for (void *entry = dshmap_iter_next(&map, &iter);
         entry;
         entry = dshmap_iter_next(&map, &iter)) {
        assert(count < 48);
        seen[count++] = entry;
    }
    assert(count == 46);

    for (size_t i = 0; i + 1 < count; i++) {
        struct hashed_entry *e = seen[i];
        assert(dshmap_iter_next_after_hash(&map, e, e->hash) == seen[i + 1]);
    }
    struct hashed_entry *last = seen[count - 1];
    assert(dshmap_iter_next_after_hash(&map, last, last->hash) == NULL);

    struct hashed_entry missing = { .key = 99, .hash = 0x4000 };
    assert(dshmap_iter_next_after_hash(&map, &missing, missing.hash) == NULL);

    dshmap_destroy(&map);
}

static void
test_safe_iteration_removes_small_chain(void)
{
    if (DSHMAP_SMALL_THRESHOLD < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * DSHMAP__GROUP_WIDTH + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(map.small);

    bool seen[8] = {0};
    size_t count = 0;
    DSHMAP_FOR_EACH_SAFE(entry, next, &map) {
        struct hashed_entry *e = entry;
        (void)next;
        assert(e >= entries && e < entries + 8);
        assert(!seen[e->key]);
        seen[e->key] = true;
        count++;
        dshmap_remove(&map, e, e->hash);
    }

    assert(count == 8);
    assert(dshmap_is_empty(&map));
    for (size_t i = 0; i < 8; i++) {
        assert(seen[i]);
    }

    dshmap_destroy(&map);
}

static void
test_safe_iteration_removes_swiss_table(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    size_t reserve_n = (size_t)DSHMAP_SMALL_THRESHOLD + 1;
    if (reserve_n < 32) {
        reserve_n = 32;
    }
    dshmap_reserve(&map, reserve_n);
    assert(!map.small);

    struct hashed_entry entries[32];
    for (size_t i = 0; i < 32; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 5) << 7) | (i & 0x7f);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    bool seen[32] = {0};
    size_t count = 0;
    DSHMAP_FOR_EACH_SAFE(entry, next, &map) {
        struct hashed_entry *e = entry;
        (void)next;
        assert(e >= entries && e < entries + 32);
        assert(!seen[e->key]);
        seen[e->key] = true;
        count++;
        dshmap_remove(&map, e, e->hash);
    }

    assert(count == 32);
    assert(dshmap_is_empty(&map));
    for (size_t i = 0; i < 32; i++) {
        assert(seen[i]);
    }

    dshmap_destroy(&map);
}

static void
test_find_empty(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);
    assert(dshmap_find(&map, dummy_hash((void *)1)) == NULL);
    assert(dshmap_find(&map, 0) == NULL);
    assert(dshmap_find(&map, 999) == NULL);
    dshmap_destroy(&map);
}

static void
test_remove_empty(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);
    dshmap_remove(&map, (void *)1, dummy_hash((void *)1));
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    dshmap_destroy(&map);
}

static void
test_remove_nonexistent(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    dshmap_insert(&map, a, dummy_hash(a));
    dshmap_insert(&map, b, dummy_hash(b));

    dshmap_remove(&map, (void *)30, dummy_hash((void *)30));
    assert(dshmap_size(&map) == 2);
    assert(dshmap_find(&map, dummy_hash(a)) == a);
    assert(dshmap_find(&map, dummy_hash(b)) == b);

    dshmap_destroy(&map);
}

static void
test_clear_empty(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);
    dshmap_clear(&map);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    dshmap_destroy(&map);
}

static void
test_insert_after_remove(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    for (size_t i = 1; i <= 20; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 1; i <= 20; i++) {
        dshmap_remove(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == 0);

    for (size_t i = 100; i <= 119; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == 20);
    for (size_t i = 100; i <= 119; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&map);
}

static void
test_reinsert_same(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    void *entry = (void *)42;
    dshmap_insert(&map, entry, dummy_hash(entry));
    assert(dshmap_find(&map, dummy_hash(entry)) == entry);

    dshmap_remove(&map, entry, dummy_hash(entry));
    assert(dshmap_find(&map, dummy_hash(entry)) == NULL);

    dshmap_insert(&map, entry, dummy_hash(entry));
    assert(dshmap_find(&map, dummy_hash(entry)) == entry);
    assert(dshmap_size(&map) == 1);

    dshmap_destroy(&map);
}

static void
test_churn(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    for (int round = 0; round < 10; round++) {
        size_t base = (size_t)round * 100 + 1;
        for (size_t i = base; i < base + 100; i++) {
            dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
        }
        for (size_t i = base; i < base + 50; i++) {
            dshmap_remove(&map, (void *)i, dummy_hash((void *)i));
        }
    }

    for (int round = 0; round < 10; round++) {
        size_t base = (size_t)round * 100 + 1;
        for (size_t i = base; i < base + 50; i++) {
            assert(dshmap_find(&map, dummy_hash((void *)i)) == NULL);
        }
        for (size_t i = base + 50; i < base + 100; i++) {
            assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
        }
    }
    assert(dshmap_size(&map) == 500);

    dshmap_destroy(&map);
}

static void
test_load_factor_boundary(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

#if DSHMAP_SMALL_THRESHOLD != 0
    dshmap_reserve(&map, DSHMAP_SMALL_THRESHOLD + 1);
    assert(!map.small);
#endif
    dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
    size_t next = fill_until_growth_left_zero(&map, 2);
    size_t mask = map.group_mask;

    dshmap_insert(&map, (void *)next, dummy_hash((void *)next));
    assert(map.group_mask > mask);

    for (size_t i = 1; i <= next; i++) {
        assert(dshmap_find(&map, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&map);
}

static void
test_remove_empty_slot_restores_growth(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

#if DSHMAP_SMALL_THRESHOLD != 0
    dshmap_reserve(&map, DSHMAP_SMALL_THRESHOLD + 1);
    assert(!map.small);
#endif
    dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
    size_t next = fill_until_growth_left_zero(&map, 2);
    size_t mask = map.group_mask;
    size_t last = next - 1;
    assert(map.growth_left == 0);

    dshmap_remove(&map, (void *)last, dummy_hash((void *)last));
    assert(map.growth_left == 1);

    dshmap_insert(&map, (void *)100, dummy_hash((void *)100));
    assert(map.group_mask == mask);
    assert(map.growth_left == 0);
    assert(dshmap_size(&map) == last);
    assert(dshmap_find(&map, dummy_hash((void *)100)) == (void *)100);

    dshmap_destroy(&map);
}

static void
test_reuse_tombstone_at_boundary(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    size_t count = test_swiss_count_for_groups(2);
    if (count <= DSHMAP__GROUP_WIDTH) {
        dshmap_destroy(&map);
        return;
    }
    size_t second_group_count = count - DSHMAP__GROUP_WIDTH;
    dshmap_reserve(&map, count);
    if (!map.small) {
        assert(map.group_mask == 1);
    }

    for (size_t i = 1; i <= DSHMAP__GROUP_WIDTH; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 129; i < 129 + second_group_count; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == count);
    if (!map.small) {
        assert(map.growth_left == 0);
    }

    dshmap_remove(&map, (void *)1, dummy_hash((void *)1));
    if (!map.small) {
        assert(map.group_mask == 1);
        assert(map.growth_left == 0);
    }

    void *replacement = (void *)(DSHMAP__GROUP_WIDTH + 1);
    dshmap_insert(&map, replacement, dummy_hash(replacement));
    if (!map.small) {
        assert(map.group_mask == 1);
        assert(map.growth_left == 0);
    }
    assert(dshmap_size(&map) == count);
    assert(dshmap_find(&map, dummy_hash(replacement)) == replacement);

    dshmap_destroy(&map);
}

static void
test_find_next_no_duplicates(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    dshmap_insert(&map, a, dummy_hash(a));
    dshmap_insert(&map, b, dummy_hash(b));

    assert(dshmap_find_next(&map, dummy_hash(a), a) == NULL);
    assert(dshmap_find_next(&map, dummy_hash(b), b) == NULL);

    dshmap_destroy(&map);
}

static void
test_find_next_filters_same_h2(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_hash_t target_hash = 0x2A;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = target_hash };
    struct hashed_entry c = { .key = 3, .hash = target_hash };
    struct hashed_entry noise1 = { .key = 4, .hash = 0xAA };
    struct hashed_entry noise2 = { .key = 5, .hash = 0x12A };
    struct hashed_entry noise3 = { .key = 6, .hash = 0x1AA };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);
    dshmap_insert(&map, &noise3, noise3.hash);
    dshmap_insert(&map, &c, c.hash);

    bool found_a = false, found_b = false, found_c = false;
    size_t count = 0;
    for (void *e = dshmap_find(&map, target_hash);
         e;
         e = dshmap_find_next(&map, target_hash, e)) {
        count++;
        if (e == &a) found_a = true;
        else if (e == &b) found_b = true;
        else if (e == &c) found_c = true;
        else assert(0 && "find_next returned same-H2 noise");
    }

    assert(count == 3);
    assert(found_a && found_b && found_c);

    dshmap_destroy(&map);
}

static void
test_for_each_with_hash(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_hash_t target_hash = 0x2A;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = target_hash };
    struct hashed_entry c = { .key = 3, .hash = target_hash };
    struct hashed_entry noise1 = { .key = 4, .hash = 0xAA };
    struct hashed_entry noise2 = { .key = 5, .hash = 0x12A };

    dshmap_insert(&map, &noise1, noise1.hash);
    dshmap_insert(&map, &a, a.hash);
    dshmap_insert(&map, &noise2, noise2.hash);
    dshmap_insert(&map, &b, b.hash);
    dshmap_insert(&map, &c, c.hash);

    macro_hash_calls = 0;
    macro_map_calls = 0;
    bool found_a = false, found_b = false, found_c = false;
    size_t count = 0;
    DSHMAP_FOR_EACH_WITH_HASH(e, macro_map_arg(&map),
                              macro_hash_arg(target_hash)) {
        count++;
        if (e == &a) found_a = true;
        else if (e == &b) found_b = true;
        else if (e == &c) found_c = true;
        else assert(0 && "FOR_EACH_WITH_HASH returned same-H2 noise");
    }
    assert(count == 3);
    assert(found_a && found_b && found_c);
    assert(macro_hash_calls == 1);
    assert(macro_map_calls == 1);

    macro_hash_calls = 0;
    macro_map_calls = 0;
    DSHMAP_FOR_EACH_WITH_HASH(e, macro_map_arg(&map),
                              macro_hash_arg(0x1234)) {
        (void)e;
        assert(0 && "FOR_EACH_WITH_HASH returned missing hash");
    }
    assert(macro_hash_calls == 1);
    assert(macro_map_calls == 1);

#if DSHMAP_SMALL_THRESHOLD != 0
    dshmap_reserve(&map, DSHMAP_SMALL_THRESHOLD + 1);
#endif
    assert(!map.small);

    count = 0;
    found_a = found_b = found_c = false;
    DSHMAP_FOR_EACH_WITH_HASH(e, &map, target_hash) {
        count++;
        if (e == &a) found_a = true;
        else if (e == &b) found_b = true;
        else if (e == &c) found_c = true;
        else assert(0 && "promoted FOR_EACH_WITH_HASH returned same-H2 noise");
    }
    assert(count == 3);
    assert(found_a && found_b && found_c);

    dshmap_destroy(&map);
}

static void
test_randomized_reference_model(void)
{
    dshmap map;
    struct model_entry entries[MODEL_CAP] = {0};

    dshmap_init(&map, hashed_entry_hash);
    model_rng_seed(0x123456789ABCDEF0ULL);

    for (size_t i = 0; i < MODEL_CAP; i++) {
        entries[i].entry.key = (int)i;
        entries[i].entry.hash = model_random_hash();
    }

    for (size_t step = 0; step < MODEL_STEPS; step++) {
        switch (model_rng_next() % 10) {
        case 0:
        case 1:
        case 2: {
            size_t idx = model_find_free(entries,
                                         model_rng_next() % MODEL_CAP);
            if (idx < MODEL_CAP) {
                entries[idx].entry.hash = model_random_hash();
                dshmap_insert(&map, &entries[idx].entry,
                             entries[idx].entry.hash);
                entries[idx].live = true;
            }
            break;
        }
        case 3:
        case 4: {
            size_t idx = model_find_live(entries,
                                         model_rng_next() % MODEL_CAP);
            if (idx < MODEL_CAP) {
                dshmap_remove(&map, &entries[idx].entry,
                             entries[idx].entry.hash);
                entries[idx].live = false;
            }
            break;
        }
        case 5: {
            struct hashed_entry missing = {
                .key = -1,
                .hash = model_random_hash(),
            };
            dshmap_remove(&map, &missing, missing.hash);
            break;
        }
        case 6:
        case 7: {
            dshmap_hash_t hash = model_random_hash();
            size_t idx = model_find_live(entries,
                                         model_rng_next() % MODEL_CAP);
            if (idx < MODEL_CAP && (model_rng_next() & 1)) {
                hash = entries[idx].entry.hash;
            }
            model_check_hash(&map, entries, hash);
            break;
        }
        case 8:
            if (model_rng_next() & 1) {
                dshmap_reserve(&map, model_rng_next() % (MODEL_CAP * 3));
            } else {
                dshmap_shrink(&map);
            }
            break;
        case 9:
            if ((model_rng_next() & 7) == 0) {
                dshmap_clear(&map);
                for (size_t i = 0; i < MODEL_CAP; i++) {
                    entries[i].live = false;
                }
            } else {
                model_check_hash(&map, entries, model_random_hash());
            }
            break;
        }

        model_check_all(&map, entries);
    }

    dshmap_destroy(&map);
}

static void
test_mixed_operations(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
    dshmap_insert(&map, (void *)2, dummy_hash((void *)2));
    dshmap_insert(&map, (void *)3, dummy_hash((void *)3));
    assert(dshmap_size(&map) == 3);

    dshmap_remove(&map, (void *)2, dummy_hash((void *)2));
    assert(dshmap_size(&map) == 2);

    dshmap_clear(&map);
    assert(dshmap_size(&map) == 0);

    dshmap_insert(&map, (void *)10, dummy_hash((void *)10));
    dshmap_insert(&map, (void *)11, dummy_hash((void *)11));
    assert(dshmap_size(&map) == 2);

    assert(dshmap_find(&map, dummy_hash((void *)1)) == NULL);
    assert(dshmap_find(&map, dummy_hash((void *)10)) == (void *)10);
    assert(dshmap_find(&map, dummy_hash((void *)11)) == (void *)11);

    dshmap_destroy(&map);
}

int
main(void)
{
    printf("dshmap tests:\n");
    RUN_TEST(test_lifecycle);
    RUN_TEST(test_static_initializer);
    RUN_TEST(test_insert_find);
    RUN_TEST(test_insert_multiple);
    RUN_TEST(test_duplicate_hashes);
    RUN_TEST(test_same_h2_different_full_hashes);
    RUN_TEST(test_tombstone_preserves_probe_chain);
    RUN_TEST(test_probe_wraparound);
    RUN_TEST(test_reuse_deep_tombstone_before_grow);
    RUN_TEST(test_swiss_reuses_tombstone_before_growth);
    RUN_TEST(test_swiss_single_group_upper_half);
    RUN_TEST(test_find_key_resolves_hash_collision);
    RUN_TEST(test_find_key_next);
    RUN_TEST(test_find_key_does_not_rehash_candidates);
    RUN_TEST(test_find_key_skips_same_h2_noise);
    RUN_TEST(test_swiss_find_key_miss_paths);
    RUN_TEST(test_small_hash_storage_policy);
    RUN_TEST(test_small_mode_uses_pooled_chaining);
    RUN_TEST(test_small_remove_preserves_chain);
    RUN_TEST(test_small_mode_reuses_free_list_nodes);
    RUN_TEST(test_small_promotes_to_swiss_at_threshold);
    RUN_TEST(test_small_to_swiss_post_promotion_operations);
    RUN_TEST(test_reserve_above_small_threshold_uses_swiss);
    RUN_TEST(test_swiss_hash_storage_policy);
    RUN_TEST(test_remove);
    RUN_TEST(test_clear);
    RUN_TEST(test_clear_after_tombstone_heavy_table);
    RUN_TEST(test_reserve);
    RUN_TEST(test_reserve_zero_noop);
    RUN_TEST(test_reserve_noop);
    RUN_TEST(test_reserve_promotes_small_table);
    RUN_TEST(test_swiss_reserve_noop_and_growth);
    RUN_TEST(test_reserve_after_tombstone_churn);
    RUN_TEST(test_capacity_empty);
    RUN_TEST(test_capacity_small_table);
    RUN_TEST(test_capacity_swiss_table);
    RUN_TEST(test_shrink_empty_frees_storage);
    RUN_TEST(test_shrink_small_table);
    RUN_TEST(test_shrink_swiss_table_does_not_demote);
    RUN_TEST(test_growth);
    RUN_TEST(test_large_table);
    RUN_TEST(test_iteration);
    RUN_TEST(test_iteration_empty);
    RUN_TEST(test_iterator_api);
    RUN_TEST(test_shard_iteration_empty);
    RUN_TEST(test_shard_iteration_small_table);
    RUN_TEST(test_shard_iteration_swiss_table);
    RUN_TEST(test_for_each_shard_evaluates_args_once);
    RUN_TEST(test_hash_iterator_api);
    RUN_TEST(test_hash_candidate_iterator_small_table);
    RUN_TEST(test_hash_candidate_iterator_swiss_table);
    RUN_TEST(test_find_with_hash_fn_uses_supplied_hash);
    RUN_TEST(test_hash_iterator_with_hash_fn_uses_supplied_hash);
    RUN_TEST(test_swiss_iter_next_after);
    RUN_TEST(test_small_iter_next_after_hash);
    RUN_TEST(test_swiss_iter_next_after_hash);
    RUN_TEST(test_safe_iteration_removes_small_chain);
    RUN_TEST(test_safe_iteration_removes_swiss_table);
    RUN_TEST(test_find_empty);
    RUN_TEST(test_remove_empty);
    RUN_TEST(test_remove_nonexistent);
    RUN_TEST(test_clear_empty);
    RUN_TEST(test_insert_after_remove);
    RUN_TEST(test_reinsert_same);
    RUN_TEST(test_churn);
    RUN_TEST(test_load_factor_boundary);
    RUN_TEST(test_remove_empty_slot_restores_growth);
    RUN_TEST(test_reuse_tombstone_at_boundary);
    RUN_TEST(test_find_next_no_duplicates);
    RUN_TEST(test_find_next_filters_same_h2);
    RUN_TEST(test_for_each_with_hash);
    RUN_TEST(test_randomized_reference_model);
    RUN_TEST(test_mixed_operations);

    printf("\nAll tests passed.\n");
    return 0;
}
