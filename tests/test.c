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
fill_until_growth_left_zero(dshmap *st, size_t next)
{
    for (;;) {
        size_t target = dshmap_size(st) + st->growth_left;
        while (next <= target) {
            dshmap_insert(st, (void *)next, dummy_hash((void *)next));
            next++;
        }
        if (st->growth_left == 0) {
            return next;
        }
    }
}

static void
test_lifecycle(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));
    dshmap_destroy(&st);

    dshmap_init(&st, dummy_hash);
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));
    dshmap_destroy(&st);
}

static void
test_insert_find(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    void *entry = (void *)42;
    dshmap_hash_t hash = dummy_hash(entry);
    dshmap_insert(&st, entry, hash);

    assert(dshmap_size(&st) == 1);
    assert(!dshmap_is_empty(&st));
    assert(dshmap_find(&st, hash) == entry);
    assert(dshmap_find(&st, dummy_hash((void *)99)) == NULL);

    dshmap_destroy(&st);
}

static void
test_insert_multiple(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    void *entries[] = {
        (void *)10, (void *)20, (void *)30, (void *)40, (void *)50,
    };
    size_t n = sizeof(entries) / sizeof(entries[0]);

    for (size_t i = 0; i < n; i++) {
        dshmap_insert(&st, entries[i], dummy_hash(entries[i]));
    }

    assert(dshmap_size(&st) == n);
    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&st, dummy_hash(entries[i])) == entries[i]);
    }

    dshmap_destroy(&st);
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
model_check_hash(const dshmap *st, const struct model_entry *entries,
                 dshmap_hash_t hash)
{
    bool seen[MODEL_CAP] = {0};
    size_t count = 0;

    for (void *entry = dshmap_find(st, hash);
         entry;
         entry = dshmap_find_next(st, hash, entry)) {
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
model_check_all(const dshmap *st, const struct model_entry *entries)
{
    bool seen[MODEL_CAP] = {0};
    size_t live = model_live_count(entries);
    size_t iter_count = 0;

    assert(dshmap_size(st) == live);
    assert(dshmap_is_empty(st) == (live == 0));

    DSHMAP_FOR_EACH(entry, st) {
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
            void *found = dshmap_find(st, hash);
            size_t idx = model_index(entries, found);
            assert(seen[i]);
            assert(idx < MODEL_CAP);
            assert(entries[idx].live);
            assert(entries[idx].entry.hash == hash);
            assert(dshmap_find_key(st, hash, &key, hashed_entry_eq) ==
                   &entries[i].entry);
            assert(dshmap_find_key_next(st, hash, &key, hashed_entry_eq,
                                       &entries[i].entry) == NULL);
        } else if (!model_has_live_hash(entries, hash)) {
            assert(dshmap_find(st, hash) == NULL);
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
    dshmap st;
    dshmap_init(&st, colliding_hash);

    void *a = (void *)1;
    void *b = (void *)2;
    void *c = (void *)3;
    dshmap_hash_t hash = 42;

    dshmap_insert(&st, a, hash);
    dshmap_insert(&st, b, hash);
    dshmap_insert(&st, c, hash);
    assert(dshmap_size(&st) == 3);

    bool found_a = false, found_b = false, found_c = false;
    size_t found_count = 0;
    void *last = NULL;
    void *e = dshmap_find(&st, hash);
    while (e) {
        found_count++;
        last = e;
        if (e == a) found_a = true;
        else if (e == b) found_b = true;
        else if (e == c) found_c = true;
        e = dshmap_find_next(&st, hash, e);
    }
    assert(found_a && found_b && found_c);
    assert(found_count == 3);
    assert(last != NULL);
    assert(dshmap_find_next(&st, hash, last) == NULL);

    dshmap_destroy(&st);
}

static void
test_same_h2_different_full_hashes(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);

    struct hashed_entry a = { .key = 1, .hash = 0x05 };
    struct hashed_entry b = { .key = 2, .hash = 0x85 };
    struct hashed_entry c = { .key = 3, .hash = 0x105 };

    dshmap_insert(&st, &a, a.hash);
    dshmap_insert(&st, &b, b.hash);
    dshmap_insert(&st, &c, c.hash);

    assert(dshmap_find(&st, a.hash) == &a);
    assert(dshmap_find(&st, b.hash) == &b);
    assert(dshmap_find(&st, c.hash) == &c);
    assert(dshmap_find(&st, 0x185) == NULL);

    dshmap_destroy(&st);
}

static void
test_tombstone_preserves_probe_chain(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);
    dshmap_reserve(&st, 14);
    if (!st.dense) {
        assert(st.group_mask == 1);
    }

    struct hashed_entry entries[9];
    for (size_t i = 0; i < 9; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 2) << 7) | (i + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&st) == 9);

    dshmap_remove(&st, &entries[3], entries[3].hash);
    assert(dshmap_find(&st, entries[3].hash) == NULL);
    assert(dshmap_find(&st, entries[8].hash) == &entries[8]);

    for (size_t i = 0; i < 9; i++) {
        if (i != 3) {
            assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&st);
}

static void
test_probe_wraparound(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);
    dshmap_reserve(&st, 28);
    if (!st.dense) {
        assert(st.group_mask == 3);
    }

    struct hashed_entry entries[9];
    for (size_t i = 0; i < 9; i++) {
        dshmap_hash_t h1 = 3 + i * 4;
        entries[i].key = (int)i;
        entries[i].hash = (h1 << 7) | (20 + i);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&st) == 9);

    for (size_t i = 0; i < 9; i++) {
        assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
    }
    assert(dshmap_find(&st, ((dshmap_hash_t)43 << 7) | 60) == NULL);

    dshmap_remove(&st, &entries[0], entries[0].hash);
    assert(dshmap_find(&st, entries[0].hash) == NULL);
    assert(dshmap_find(&st, entries[8].hash) == &entries[8]);

    dshmap_remove(&st, &entries[8], entries[8].hash);
    assert(dshmap_find(&st, entries[8].hash) == NULL);
    assert(dshmap_size(&st) == 7);

    dshmap_destroy(&st);
}

static void
test_reuse_deep_tombstone_before_grow(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);
    dshmap_reserve(&st, 28);
    if (!st.dense) {
        assert(st.group_mask == 3);
    }

    struct hashed_entry entries[29];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&st) == 28);
    if (!st.dense) {
        assert(st.growth_left == 0);
    }

    dshmap_remove(&st, &entries[10], entries[10].hash);
    assert(dshmap_find(&st, entries[10].hash) == NULL);
    if (!st.dense) {
        assert(st.growth_left == 0);
    }

    size_t mask = st.group_mask;
    entries[28].key = 28;
    entries[28].hash = ((dshmap_hash_t)(100 * 4) << 7) | 90;
    dshmap_insert(&st, &entries[28], entries[28].hash);

    assert(st.group_mask == mask);
    assert(dshmap_size(&st) == 28);
    assert(dshmap_find(&st, entries[28].hash) == &entries[28]);

    for (size_t i = 0; i < 28; i++) {
        if (i != 10) {
            assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&st);
}

static void
test_find_key_resolves_hash_collision(void)
{
    dshmap st;
    dshmap_init(&st, colliding_hash);

    struct keyed_entry a = { .key = 1 };
    struct keyed_entry b = { .key = 2 };
    struct keyed_entry c = { .key = 3 };
    dshmap_hash_t hash = 42;

    dshmap_insert(&st, &a, hash);
    dshmap_insert(&st, &b, hash);
    dshmap_insert(&st, &c, hash);

    int key = 2;
    assert(dshmap_find_key(&st, hash, &key, keyed_entry_eq) == &b);

    key = 99;
    assert(dshmap_find_key(&st, hash, &key, keyed_entry_eq) == NULL);

    dshmap_destroy(&st);
}

static void
test_find_key_next(void)
{
    dshmap st;
    dshmap_init(&st, colliding_hash);

    struct keyed_entry a = { .key = 7 };
    struct keyed_entry b = { .key = 8 };
    struct keyed_entry c = { .key = 7 };
    dshmap_hash_t hash = 42;

    dshmap_insert(&st, &a, hash);
    dshmap_insert(&st, &b, hash);
    dshmap_insert(&st, &c, hash);

    int key = 7;
    void *first = dshmap_find_key(&st, hash, &key, keyed_entry_eq);
    void *second = dshmap_find_key_next(&st, hash, &key, keyed_entry_eq, first);
    void *third = dshmap_find_key_next(&st, hash, &key, keyed_entry_eq, second);

    assert((first == &a && second == &c) ||
           (first == &c && second == &a));
    assert(third == NULL);

    dshmap_destroy(&st);
}

static void
test_find_key_does_not_rehash_candidates(void)
{
    dshmap st;
    dshmap_init(&st, counted_entry_hash);

    struct counted_entry a = { .key = 7, .hash = 42 };
    struct counted_entry b = { .key = 8, .hash = 42 };
    struct counted_entry c = { .key = 7, .hash = 42 };
    dshmap_hash_t hash = 42;

    dshmap_insert(&st, &a, hash);
    dshmap_insert(&st, &b, hash);
    dshmap_insert(&st, &c, hash);

    counted_hash_calls = 0;

    int key = 7;
    void *first = dshmap_find_key(&st, hash, &key, counted_entry_eq);
    void *second = dshmap_find_key_next(&st, hash, &key, counted_entry_eq, first);
    void *third = dshmap_find_key_next(&st, hash, &key, counted_entry_eq, second);

    assert((first == &a && second == &c) ||
           (first == &c && second == &a));
    assert(third == NULL);
    assert(counted_hash_calls == 0);

    dshmap_destroy(&st);
}

static void
test_find_key_skips_same_h2_noise(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);

    dshmap_hash_t target_hash = 0x55;
    struct hashed_entry noise1 = { .key = 1, .hash = 0xD5 };
    struct hashed_entry a = { .key = 7, .hash = target_hash };
    struct hashed_entry noise2 = { .key = 2, .hash = 0x155 };
    struct hashed_entry b = { .key = 7, .hash = target_hash };
    struct hashed_entry noise3 = { .key = 3, .hash = 0x1D5 };

    dshmap_insert(&st, &noise1, noise1.hash);
    dshmap_insert(&st, &a, a.hash);
    dshmap_insert(&st, &noise2, noise2.hash);
    dshmap_insert(&st, &b, b.hash);
    dshmap_insert(&st, &noise3, noise3.hash);

    int key = 7;
    void *first = dshmap_find_key(&st, target_hash, &key, hashed_entry_eq);
    void *second = dshmap_find_key_next(&st, target_hash, &key,
                                       hashed_entry_eq, first);
    void *third = dshmap_find_key_next(&st, target_hash, &key,
                                      hashed_entry_eq, second);

    assert((first == &a && second == &b) ||
           (first == &b && second == &a));
    assert(third == NULL);

    key = 99;
    assert(dshmap_find_key(&st, target_hash, &key, hashed_entry_eq) == NULL);

    dshmap_destroy(&st);
}

static void
test_dense_hash_storage_policy(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap st;
    dshmap_init(&st, counted_entry_hash);

    struct counted_entry entries[8];
    size_t n = threshold < 8 ? threshold : 8;
    for (size_t i = 0; i < n; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 17 + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }

#if DSHMAP_DENSE_STORE_HASHES
    assert(st.hashes != NULL);
#else
    assert(st.hashes == NULL);
#endif
    assert(st.dense);
    counted_hash_calls = 0;

    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
    }
    assert(dshmap_find(&st, 999999) == NULL);
#if DSHMAP_DENSE_STORE_HASHES
    assert(counted_hash_calls == 0);
#else
    assert(counted_hash_calls > 0);
#endif

    dshmap_destroy(&st);
}

static void
test_dense_remove_backshifts_cluster(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold < 8) {
        return;
    }

    dshmap st;
    dshmap_init(&st, hashed_entry_hash);

    struct hashed_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 8 + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }

    assert(st.dense);

    dshmap_remove(&st, &entries[0], entries[0].hash);
    assert(dshmap_find(&st, entries[0].hash) == NULL);
    for (size_t i = 1; i < 8; i++) {
        assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
    }

    struct hashed_entry extra = { .key = 99, .hash = (dshmap_hash_t)(99 * 8 + 1) };
    dshmap_insert(&st, &extra, extra.hash);
    assert(dshmap_find(&st, extra.hash) == &extra);
    assert(dshmap_size(&st) == 8);

    dshmap_destroy(&st);
}

static void
test_dense_promotes_to_swiss_at_threshold(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap st;
    dshmap_init(&st, hashed_entry_hash);

    size_t n = threshold + 1;
    struct hashed_entry *entries = malloc(n * sizeof(*entries));
    assert(entries != NULL);

    for (size_t i = 0; i < threshold; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 2654435761u + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
        assert(st.dense);
    }

    entries[threshold].key = (int)threshold;
    entries[threshold].hash = (dshmap_hash_t)(threshold * 2654435761u + 1);
    dshmap_insert(&st, &entries[threshold], entries[threshold].hash);

    assert(!st.dense);
#if DSHMAP_SWISS_STORE_HASHES
    assert(st.hashes != NULL);
#else
    assert(st.hashes == NULL);
#endif
    assert(dshmap_size(&st) == n);
    for (size_t i = 0; i < n; i++) {
        assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
    }

    free(entries);
    dshmap_destroy(&st);
}

static void
test_reserve_above_dense_threshold_uses_swiss(void)
{
    size_t threshold = DSHMAP_DENSE_THRESHOLD;
    if (threshold == 0) {
        return;
    }

    dshmap st;
    dshmap_init(&st, hashed_entry_hash);

    dshmap_reserve(&st, threshold + 1);
    assert(st.slots != NULL);
    assert(!st.dense);
#if DSHMAP_SWISS_STORE_HASHES
    assert(st.hashes != NULL);
#else
    assert(st.hashes == NULL);
#endif

    struct hashed_entry entries[16];
    for (size_t i = 0; i < 16; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }

    for (size_t i = 0; i < 16; i++) {
        assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
    }

    dshmap_destroy(&st);
}

static void
test_swiss_hash_storage_policy(void)
{
    dshmap st;
    dshmap_init(&st, counted_entry_hash);

    size_t reserve_n = (size_t)DSHMAP_DENSE_THRESHOLD + 1;
    if (reserve_n < 16) {
        reserve_n = 16;
    }
    dshmap_reserve(&st, reserve_n);

    assert(st.slots != NULL);
    assert(!st.dense);
#if DSHMAP_SWISS_STORE_HASHES
    assert(st.hashes != NULL);
#else
    assert(st.hashes == NULL);
#endif

    struct counted_entry entries[8];
    for (size_t i = 0; i < 8; i++) {
        entries[i].key = (int)i;
        entries[i].hash = (dshmap_hash_t)(i * 17 + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }

    counted_hash_calls = 0;
    for (size_t i = 0; i < 8; i++) {
        assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
    }
#if DSHMAP_SWISS_STORE_HASHES
    assert(counted_hash_calls == 0);
#else
    assert(counted_hash_calls > 0);
#endif

    dshmap_destroy(&st);
}

static void
test_remove(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    void *c = (void *)30;
    dshmap_insert(&st, a, dummy_hash(a));
    dshmap_insert(&st, b, dummy_hash(b));
    dshmap_insert(&st, c, dummy_hash(c));

    dshmap_remove(&st, b, dummy_hash(b));
    assert(dshmap_size(&st) == 2);
    assert(dshmap_find(&st, dummy_hash(b)) == NULL);
    assert(dshmap_find(&st, dummy_hash(a)) == a);
    assert(dshmap_find(&st, dummy_hash(c)) == c);

    dshmap_remove(&st, a, dummy_hash(a));
    assert(dshmap_size(&st) == 1);
    assert(dshmap_find(&st, dummy_hash(a)) == NULL);
    assert(dshmap_find(&st, dummy_hash(c)) == c);

    dshmap_remove(&st, c, dummy_hash(c));
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));

    dshmap_destroy(&st);
}

static void
test_clear(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    for (size_t i = 1; i <= 10; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&st) == 10);

    dshmap_clear(&st);
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));
    for (size_t i = 1; i <= 10; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == NULL);
    }

    for (size_t i = 100; i <= 105; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&st) == 6);
    for (size_t i = 100; i <= 105; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&st);
}

static void
test_clear_after_tombstone_heavy_table(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);
    dshmap_reserve(&st, 28);

    struct hashed_entry entries[28];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&st) == 28);

    for (size_t i = 0; i < 20; i++) {
        dshmap_remove(&st, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&st) == 8);

    dshmap_clear(&st);
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));

    size_t count = 0;
    DSHMAP_FOR_EACH(entry, &st) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    for (size_t i = 0; i < 28; i++) {
        assert(dshmap_find(&st, entries[i].hash) == NULL);
    }

    struct hashed_entry fresh[5];
    for (size_t i = 0; i < 5; i++) {
        fresh[i].key = (int)i + 100;
        fresh[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (40 + i);
        dshmap_insert(&st, &fresh[i], fresh[i].hash);
    }
    assert(dshmap_size(&st) == 5);

    for (size_t i = 0; i < 5; i++) {
        assert(dshmap_find(&st, fresh[i].hash) == &fresh[i]);
    }

    dshmap_destroy(&st);
}

static void
test_reserve(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    dshmap_reserve(&st, 100);
    size_t mask_after_reserve = st.group_mask;
    assert(mask_after_reserve > 0);

    for (size_t i = 1; i <= 100; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(st.group_mask == mask_after_reserve);
    assert(dshmap_size(&st) == 100);

    for (size_t i = 1; i <= 100; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&st);
}

static void
test_reserve_noop(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    dshmap_reserve(&st, 5);
    size_t mask = st.group_mask;

    for (size_t i = 1; i <= 5; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }

    dshmap_reserve(&st, 3);
    assert(st.group_mask == mask);

    dshmap_destroy(&st);
}

static void
test_reserve_after_tombstone_churn(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);
    dshmap_reserve(&st, 28);

    struct hashed_entry entries[28];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((dshmap_hash_t)(i * 4) << 7) | (i + 1);
        dshmap_insert(&st, &entries[i], entries[i].hash);
    }
    assert(dshmap_size(&st) == 28);

    size_t removed = 0;
    for (size_t i = 0; i < 28; i += 3) {
        dshmap_remove(&st, &entries[i], entries[i].hash);
        removed++;
    }
    assert(dshmap_size(&st) == 28 - removed);

    size_t old_mask = st.group_mask;
    dshmap_reserve(&st, 100);
    assert(st.group_mask > old_mask);
    assert(dshmap_size(&st) == 28 - removed);

    for (size_t i = 0; i < 28; i++) {
        if (i % 3 == 0) {
            assert(dshmap_find(&st, entries[i].hash) == NULL);
        } else {
            assert(dshmap_find(&st, entries[i].hash) == &entries[i]);
        }
    }

    dshmap_destroy(&st);
}

static void
test_growth(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    size_t n = 500;
    size_t prev_mask = 0;
    int resizes = 0;

    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
        if (st.group_mask != prev_mask) {
            resizes++;
            prev_mask = st.group_mask;

            for (size_t j = 1; j <= i; j++) {
                assert(dshmap_find(&st, dummy_hash((void *)j)) == (void *)j);
            }
        }
    }

    assert(resizes >= 3);
    assert(dshmap_size(&st) == n);

    for (size_t i = 1; i <= n; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&st);
}

static void
test_large_table(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    size_t n = 50000;
    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&st) == n);

    for (size_t i = 1; i <= n; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    assert(dshmap_find(&st, dummy_hash((void *)(n + 1))) == NULL);
    assert(dshmap_find(&st, dummy_hash((void *)(n + 1000))) == NULL);

    dshmap_destroy(&st);
}

static void
test_iteration(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    size_t n = 200;
    for (size_t i = 1; i <= n; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }

    bool seen[201] = {0};
    size_t count = 0;
    DSHMAP_FOR_EACH(entry, &st) {
        size_t val = (size_t)entry;
        assert(val >= 1 && val <= n);
        assert(!seen[val]);
        seen[val] = true;
        count++;
    }
    assert(count == n);

    dshmap_destroy(&st);
}

static void
test_iteration_empty(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    size_t count = 0;
    DSHMAP_FOR_EACH(entry, &st) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    dshmap_destroy(&st);
}

static void
test_find_empty(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);
    assert(dshmap_find(&st, dummy_hash((void *)1)) == NULL);
    assert(dshmap_find(&st, 0) == NULL);
    assert(dshmap_find(&st, 999) == NULL);
    dshmap_destroy(&st);
}

static void
test_remove_empty(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);
    dshmap_remove(&st, (void *)1, dummy_hash((void *)1));
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));
    dshmap_destroy(&st);
}

static void
test_remove_nonexistent(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    dshmap_insert(&st, a, dummy_hash(a));
    dshmap_insert(&st, b, dummy_hash(b));

    dshmap_remove(&st, (void *)30, dummy_hash((void *)30));
    assert(dshmap_size(&st) == 2);
    assert(dshmap_find(&st, dummy_hash(a)) == a);
    assert(dshmap_find(&st, dummy_hash(b)) == b);

    dshmap_destroy(&st);
}

static void
test_clear_empty(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);
    dshmap_clear(&st);
    assert(dshmap_size(&st) == 0);
    assert(dshmap_is_empty(&st));
    dshmap_destroy(&st);
}

static void
test_insert_after_remove(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    for (size_t i = 1; i <= 20; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 1; i <= 20; i++) {
        dshmap_remove(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&st) == 0);

    for (size_t i = 100; i <= 119; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&st) == 20);
    for (size_t i = 100; i <= 119; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&st);
}

static void
test_reinsert_same(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    void *entry = (void *)42;
    dshmap_insert(&st, entry, dummy_hash(entry));
    assert(dshmap_find(&st, dummy_hash(entry)) == entry);

    dshmap_remove(&st, entry, dummy_hash(entry));
    assert(dshmap_find(&st, dummy_hash(entry)) == NULL);

    dshmap_insert(&st, entry, dummy_hash(entry));
    assert(dshmap_find(&st, dummy_hash(entry)) == entry);
    assert(dshmap_size(&st) == 1);

    dshmap_destroy(&st);
}

static void
test_churn(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    for (int round = 0; round < 10; round++) {
        size_t base = (size_t)round * 100 + 1;
        for (size_t i = base; i < base + 100; i++) {
            dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
        }
        for (size_t i = base; i < base + 50; i++) {
            dshmap_remove(&st, (void *)i, dummy_hash((void *)i));
        }
    }

    for (int round = 0; round < 10; round++) {
        size_t base = (size_t)round * 100 + 1;
        for (size_t i = base; i < base + 50; i++) {
            assert(dshmap_find(&st, dummy_hash((void *)i)) == NULL);
        }
        for (size_t i = base + 50; i < base + 100; i++) {
            assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
        }
    }
    assert(dshmap_size(&st) == 500);

    dshmap_destroy(&st);
}

static void
test_load_factor_boundary(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    dshmap_insert(&st, (void *)1, dummy_hash((void *)1));
    size_t next = fill_until_growth_left_zero(&st, 2);
    size_t mask = st.group_mask;

    dshmap_insert(&st, (void *)next, dummy_hash((void *)next));
    assert(st.group_mask > mask);

    for (size_t i = 1; i <= next; i++) {
        assert(dshmap_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    dshmap_destroy(&st);
}

static void
test_remove_empty_slot_restores_growth(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    dshmap_insert(&st, (void *)1, dummy_hash((void *)1));
    size_t next = fill_until_growth_left_zero(&st, 2);
    size_t mask = st.group_mask;
    size_t last = next - 1;
    assert(st.growth_left == 0);

    dshmap_remove(&st, (void *)last, dummy_hash((void *)last));
    assert(st.growth_left == 1);

    dshmap_insert(&st, (void *)100, dummy_hash((void *)100));
    assert(st.group_mask == mask);
    assert(st.growth_left == 0);
    assert(dshmap_size(&st) == last);
    assert(dshmap_find(&st, dummy_hash((void *)100)) == (void *)100);

    dshmap_destroy(&st);
}

static void
test_reuse_tombstone_at_boundary(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    dshmap_reserve(&st, 14);
    if (!st.dense) {
        assert(st.group_mask == 1);
    }

    for (size_t i = 1; i <= 8; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 129; i <= 134; i++) {
        dshmap_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(dshmap_size(&st) == 14);
    if (!st.dense) {
        assert(st.growth_left == 0);
    }

    dshmap_remove(&st, (void *)1, dummy_hash((void *)1));
    if (!st.dense) {
        assert(st.group_mask == 1);
        assert(st.growth_left == 0);
    }

    dshmap_insert(&st, (void *)9, dummy_hash((void *)9));
    if (!st.dense) {
        assert(st.group_mask == 1);
        assert(st.growth_left == 0);
    }
    assert(dshmap_size(&st) == 14);
    assert(dshmap_find(&st, dummy_hash((void *)9)) == (void *)9);

    dshmap_destroy(&st);
}

static void
test_find_next_no_duplicates(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    dshmap_insert(&st, a, dummy_hash(a));
    dshmap_insert(&st, b, dummy_hash(b));

    assert(dshmap_find_next(&st, dummy_hash(a), a) == NULL);
    assert(dshmap_find_next(&st, dummy_hash(b), b) == NULL);

    dshmap_destroy(&st);
}

static void
test_find_next_filters_same_h2(void)
{
    dshmap st;
    dshmap_init(&st, hashed_entry_hash);

    dshmap_hash_t target_hash = 0x2A;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = target_hash };
    struct hashed_entry c = { .key = 3, .hash = target_hash };
    struct hashed_entry noise1 = { .key = 4, .hash = 0xAA };
    struct hashed_entry noise2 = { .key = 5, .hash = 0x12A };
    struct hashed_entry noise3 = { .key = 6, .hash = 0x1AA };

    dshmap_insert(&st, &noise1, noise1.hash);
    dshmap_insert(&st, &a, a.hash);
    dshmap_insert(&st, &noise2, noise2.hash);
    dshmap_insert(&st, &b, b.hash);
    dshmap_insert(&st, &noise3, noise3.hash);
    dshmap_insert(&st, &c, c.hash);

    bool found_a = false, found_b = false, found_c = false;
    size_t count = 0;
    for (void *e = dshmap_find(&st, target_hash);
         e;
         e = dshmap_find_next(&st, target_hash, e)) {
        count++;
        if (e == &a) found_a = true;
        else if (e == &b) found_b = true;
        else if (e == &c) found_c = true;
        else assert(0 && "find_next returned same-H2 noise");
    }

    assert(count == 3);
    assert(found_a && found_b && found_c);

    dshmap_destroy(&st);
}

static void
test_randomized_reference_model(void)
{
    dshmap st;
    struct model_entry entries[MODEL_CAP] = {0};

    dshmap_init(&st, hashed_entry_hash);
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
                dshmap_insert(&st, &entries[idx].entry,
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
                dshmap_remove(&st, &entries[idx].entry,
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
            dshmap_remove(&st, &missing, missing.hash);
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
            model_check_hash(&st, entries, hash);
            break;
        }
        case 8:
            dshmap_reserve(&st, model_rng_next() % (MODEL_CAP * 3));
            break;
        case 9:
            if ((model_rng_next() & 7) == 0) {
                dshmap_clear(&st);
                for (size_t i = 0; i < MODEL_CAP; i++) {
                    entries[i].live = false;
                }
            } else {
                model_check_hash(&st, entries, model_random_hash());
            }
            break;
        }

        model_check_all(&st, entries);
    }

    dshmap_destroy(&st);
}

static void
test_mixed_operations(void)
{
    dshmap st;
    dshmap_init(&st, dummy_hash);

    dshmap_insert(&st, (void *)1, dummy_hash((void *)1));
    dshmap_insert(&st, (void *)2, dummy_hash((void *)2));
    dshmap_insert(&st, (void *)3, dummy_hash((void *)3));
    assert(dshmap_size(&st) == 3);

    dshmap_remove(&st, (void *)2, dummy_hash((void *)2));
    assert(dshmap_size(&st) == 2);

    dshmap_clear(&st);
    assert(dshmap_size(&st) == 0);

    dshmap_insert(&st, (void *)10, dummy_hash((void *)10));
    dshmap_insert(&st, (void *)11, dummy_hash((void *)11));
    assert(dshmap_size(&st) == 2);

    assert(dshmap_find(&st, dummy_hash((void *)1)) == NULL);
    assert(dshmap_find(&st, dummy_hash((void *)10)) == (void *)10);
    assert(dshmap_find(&st, dummy_hash((void *)11)) == (void *)11);

    dshmap_destroy(&st);
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
    RUN_TEST(test_reserve_above_dense_threshold_uses_swiss);
    RUN_TEST(test_swiss_hash_storage_policy);
    RUN_TEST(test_remove);
    RUN_TEST(test_clear);
    RUN_TEST(test_clear_after_tombstone_heavy_table);
    RUN_TEST(test_reserve);
    RUN_TEST(test_reserve_noop);
    RUN_TEST(test_reserve_after_tombstone_churn);
    RUN_TEST(test_growth);
    RUN_TEST(test_large_table);
    RUN_TEST(test_iteration);
    RUN_TEST(test_iteration_empty);
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
    RUN_TEST(test_randomized_reference_model);
    RUN_TEST(test_mixed_operations);

    printf("\nAll tests passed.\n");
    return 0;
}
