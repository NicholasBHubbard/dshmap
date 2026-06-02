#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../dshmap.h"

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
static size_t macro_hash_calls;
static size_t macro_map_calls;
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
    if (!map.dense) {
        assert(map.group_mask == 1);
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
    dshmap_reserve(&map, 28);
    if (!map.dense) {
        assert(map.group_mask == 3);
    }

    struct hashed_entry entries[9];
    for (size_t i = 0; i < 9; i++) {
        dshmap_hash_t h1 = 3 + i * 4;
        entries[i].key = (int)i;
        entries[i].hash = (h1 << 7) | (20 + i);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == 9);

    for (size_t i = 0; i < 9; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }
    assert(dshmap_find(&map, ((dshmap_hash_t)43 << 7) | 60) == NULL);

    dshmap_remove(&map, &entries[0], entries[0].hash);
    assert(dshmap_find(&map, entries[0].hash) == NULL);
    assert(dshmap_find(&map, entries[8].hash) == &entries[8]);

    dshmap_remove(&map, &entries[8], entries[8].hash);
    assert(dshmap_find(&map, entries[8].hash) == NULL);
    assert(dshmap_size(&map) == 7);

    dshmap_destroy(&map);
}

static void
test_reuse_deep_tombstone_before_grow(void)
{
    dshmap map;
    dshmap_init(&map, hashed_entry_hash);
    dshmap_reserve(&map, 28);
    if (!map.dense) {
        assert(map.group_mask == 3);
    }

    struct hashed_entry entries[29];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&map) == 28);
    if (!map.dense) {
        assert(map.growth_left == 0);
    }

    dshmap_remove(&map, &entries[10], entries[10].hash);
    assert(dshmap_find(&map, entries[10].hash) == NULL);
    if (!map.dense) {
        assert(map.growth_left == 0);
    }

    size_t mask = map.group_mask;
    entries[28].key = 28;
    entries[28].hash = ((dshmap_hash_t)(100 * 4) << 7) | 90;
    dshmap_insert(&map, &entries[28], entries[28].hash);

    assert(map.group_mask == mask);
    assert(dshmap_size(&map) == 28);
    assert(dshmap_find(&map, entries[28].hash) == &entries[28]);

    for (size_t i = 0; i < 28; i++) {
        if (i != 10) {
            assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&map);
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
test_dense_hash_storage_policy(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
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

#if DSHMAP_DENSE_STORE_HASHES
    assert(map.hashes != NULL);
#else
    assert(map.hashes == NULL);
#endif
    assert(map.dense);
    counted_hash_calls = 0;

    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }
    assert(dshmap_find(&map, 999999) == NULL);
#if DSHMAP_DENSE_STORE_HASHES
    assert(counted_hash_calls == 0);
#else
    assert(counted_hash_calls > 0);
#endif

    dshmap_destroy(&map);
}

static void
test_dense_remove_backshifts_cluster(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 8 + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    assert(map.dense);

    dshmap_remove(&map, &entries[0], entries[0].hash);
    assert(dshmap_find(&map, entries[0].hash) == NULL);
    for (size_t i = 1; i < 8; i++) {
        assert(dshmap_find(&map, entries[i].hash) == &entries[i]);
    }

    struct hashed_entry extra = { .key = 99, .hash = (dshmap_hash_t)(99 * 8 + 1) };
    dshmap_insert(&map, &extra, extra.hash);
    assert(dshmap_find(&map, extra.hash) == &extra);
    assert(dshmap_size(&map) == 8);

    dshmap_destroy(&map);
}

static void
test_dense_promotes_to_swiss_at_threshold(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
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
        assert(map.dense);
    }

    entries[threshold].key = (int)threshold;
    entries[threshold].hash = (dshmap_hash_t)(threshold * 2654435761u + 1);
    dshmap_insert(&map, &entries[threshold], entries[threshold].hash);

    assert(!map.dense);
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
test_dense_to_swiss_post_promotion_operations(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
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
        assert(map.dense);
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
    assert(!map.dense);
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
    assert(!map.dense);
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
test_reserve_above_dense_threshold_uses_swiss(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_reserve(&map, threshold + 1);
    assert(map.slots != NULL);
    assert(!map.dense);
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

    size_t reserve_n = (size_t)DSHMAP_DENSE_THRESHOLD + 1;
    if (reserve_n < 16) {
        reserve_n = 16;
    }
    dshmap_reserve(&map, reserve_n);

    assert(map.slots != NULL);
    assert(!map.dense);
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
test_reserve_promotes_dense_table(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entry = { .key = 1, .hash = 1 };
    dshmap_insert(&map, &entry, entry.hash);
    assert(map.dense);

    dshmap_reserve(&map, threshold + 1);
    assert(!map.dense);
    assert(dshmap_size(&map) == 1);
    assert(dshmap_find(&map, entry.hash) == &entry);

    dshmap_destroy(&map);
}

static void
test_swiss_reserve_noop_and_growth(void)
{
    size_t reserve_n = (size_t)DSHMAP_DENSE_THRESHOLD + 1;
    if (reserve_n < 16) {
        reserve_n = 16;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    dshmap_reserve(&map, reserve_n);
    assert(!map.dense);

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

static void
test_safe_iteration_removes_dense_cluster(void)
{
    if (DSHMAP_DENSE_THRESHOLD < 8) {
        return;
    }

    dshmap map;
    dshmap_init(&map, hashed_entry_hash);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 16 + 1);
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }
    assert(map.dense);

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

    size_t reserve_n = (size_t)DSHMAP_DENSE_THRESHOLD + 1;
    if (reserve_n < 32) {
        reserve_n = 32;
    }
    dshmap_reserve(&map, reserve_n);
    assert(!map.dense);

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

    dshmap_reserve(&map, 14);
    if (!map.dense) {
        assert(map.group_mask == 1);
    }

    for (size_t i = 1; i <= 8; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 129; i <= 134; i++) {
        dshmap_insert(&map, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&map) == 14);
    if (!map.dense) {
        assert(map.growth_left == 0);
    }

    dshmap_remove(&map, (void *)1, dummy_hash((void *)1));
    if (!map.dense) {
        assert(map.group_mask == 1);
        assert(map.growth_left == 0);
    }

    dshmap_insert(&map, (void *)9, dummy_hash((void *)9));
    if (!map.dense) {
        assert(map.group_mask == 1);
        assert(map.growth_left == 0);
    }
    assert(dshmap_size(&map) == 14);
    assert(dshmap_find(&map, dummy_hash((void *)9)) == (void *)9);

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

#if DSHMAP_DENSE_THRESHOLD != 0
    dshmap_reserve(&map, DSHMAP_DENSE_THRESHOLD + 1);
#endif
    assert(!map.dense);

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
            dshmap_reserve(&map, model_rng_next() % (MODEL_CAP * 3));
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
    RUN_TEST(test_insert_find);
    RUN_TEST(test_insert_multiple);
    RUN_TEST(test_duplicate_hashes);
    RUN_TEST(test_same_h2_different_full_hashes);
    RUN_TEST(test_tombstone_preserves_probe_chain);
    RUN_TEST(test_probe_wraparound);
    RUN_TEST(test_reuse_deep_tombstone_before_grow);
    RUN_TEST(test_find_key_resolves_hash_collision);
    RUN_TEST(test_find_key_next);
    RUN_TEST(test_find_key_does_not_rehash_candidates);
    RUN_TEST(test_find_key_skips_same_h2_noise);
    RUN_TEST(test_dense_hash_storage_policy);
    RUN_TEST(test_dense_remove_backshifts_cluster);
    RUN_TEST(test_dense_promotes_to_swiss_at_threshold);
    RUN_TEST(test_dense_to_swiss_post_promotion_operations);
    RUN_TEST(test_reserve_above_dense_threshold_uses_swiss);
    RUN_TEST(test_swiss_hash_storage_policy);
    RUN_TEST(test_remove);
    RUN_TEST(test_clear);
    RUN_TEST(test_clear_after_tombstone_heavy_table);
    RUN_TEST(test_reserve);
    RUN_TEST(test_reserve_zero_noop);
    RUN_TEST(test_reserve_noop);
    RUN_TEST(test_reserve_promotes_dense_table);
    RUN_TEST(test_swiss_reserve_noop_and_growth);
    RUN_TEST(test_reserve_after_tombstone_churn);
    RUN_TEST(test_growth);
    RUN_TEST(test_large_table);
    RUN_TEST(test_iteration);
    RUN_TEST(test_iteration_empty);
    RUN_TEST(test_iterator_api);
    RUN_TEST(test_safe_iteration_removes_dense_cluster);
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
