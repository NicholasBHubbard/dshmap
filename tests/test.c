#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../swtab.h"

#define RUN_TEST(fn) do { printf("  %-40s", #fn); fn(); printf("ok\n"); } while (0)

static swtab_hash_t
dummy_hash(const void *entry)
{
    return (swtab_hash_t)entry;
}

static void
test_lifecycle(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));
    swtab_destroy(&st);

    swtab_init(&st, dummy_hash);
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));
    swtab_destroy(&st);
}

static void
test_insert_find(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    void *entry = (void *)42;
    swtab_hash_t hash = dummy_hash(entry);
    swtab_insert(&st, entry, hash);

    assert(swtab_size(&st) == 1);
    assert(!swtab_is_empty(&st));
    assert(swtab_find(&st, hash) == entry);
    assert(swtab_find(&st, dummy_hash((void *)99)) == NULL);

    swtab_destroy(&st);
}

static void
test_insert_multiple(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    void *entries[] = {
        (void *)10, (void *)20, (void *)30, (void *)40, (void *)50,
    };
    size_t n = sizeof(entries) / sizeof(entries[0]);

    for (size_t i = 0; i < n; i++) {
        swtab_insert(&st, entries[i], dummy_hash(entries[i]));
    }

    assert(swtab_size(&st) == n);
    for (size_t i = 0; i < n; i++) {
        assert(swtab_find(&st, dummy_hash(entries[i])) == entries[i]);
    }

    swtab_destroy(&st);
}

static swtab_hash_t
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
    swtab_hash_t hash;
};

struct hashed_entry {
    int key;
    swtab_hash_t hash;
};

static size_t counted_hash_calls;

static bool
keyed_entry_eq(const void *entry, const void *key)
{
    const struct keyed_entry *e = entry;
    const int *k = key;
    return e->key == *k;
}

static swtab_hash_t
counted_entry_hash(const void *entry)
{
    const struct counted_entry *e = entry;
    counted_hash_calls++;
    return e->hash;
}

static swtab_hash_t
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
    swtab st;
    swtab_init(&st, colliding_hash);

    void *a = (void *)1;
    void *b = (void *)2;
    void *c = (void *)3;
    swtab_hash_t hash = 42;

    swtab_insert(&st, a, hash);
    swtab_insert(&st, b, hash);
    swtab_insert(&st, c, hash);
    assert(swtab_size(&st) == 3);

    bool found_a = false, found_b = false, found_c = false;
    size_t found_count = 0;
    void *last = NULL;
    void *e = swtab_find(&st, hash);
    while (e) {
        found_count++;
        last = e;
        if (e == a) found_a = true;
        else if (e == b) found_b = true;
        else if (e == c) found_c = true;
        e = swtab_find_next(&st, hash, e);
    }
    assert(found_a && found_b && found_c);
    assert(found_count == 3);
    assert(last != NULL);
    assert(swtab_find_next(&st, hash, last) == NULL);

    swtab_destroy(&st);
}

static void
test_same_h2_different_full_hashes(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);

    struct hashed_entry a = { .key = 1, .hash = 0x05 };
    struct hashed_entry b = { .key = 2, .hash = 0x85 };
    struct hashed_entry c = { .key = 3, .hash = 0x105 };

    swtab_insert(&st, &a, a.hash);
    swtab_insert(&st, &b, b.hash);
    swtab_insert(&st, &c, c.hash);

    assert(swtab_find(&st, a.hash) == &a);
    assert(swtab_find(&st, b.hash) == &b);
    assert(swtab_find(&st, c.hash) == &c);
    assert(swtab_find(&st, 0x185) == NULL);

    swtab_destroy(&st);
}

static void
test_tombstone_preserves_probe_chain(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);
    swtab_reserve(&st, 14);
    assert(st.group_mask == 1);

    struct hashed_entry entries[9];
    for (size_t i = 0; i < 9; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((swtab_hash_t)(i * 2) << 7) | (i + 1);
        swtab_insert(&st, &entries[i], entries[i].hash);
    }
    assert(swtab_size(&st) == 9);

    swtab_remove(&st, &entries[3], entries[3].hash);
    assert(swtab_find(&st, entries[3].hash) == NULL);
    assert(swtab_find(&st, entries[8].hash) == &entries[8]);

    for (size_t i = 0; i < 9; i++) {
        if (i != 3) {
            assert(swtab_find(&st, entries[i].hash) == &entries[i]);
        }
    }

    swtab_destroy(&st);
}

static void
test_probe_wraparound(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);
    swtab_reserve(&st, 28);
    assert(st.group_mask == 3);

    struct hashed_entry entries[9];
    for (size_t i = 0; i < 9; i++) {
        swtab_hash_t h1 = 3 + i * 4;
        entries[i].key = (int)i;
        entries[i].hash = (h1 << 7) | (20 + i);
        swtab_insert(&st, &entries[i], entries[i].hash);
    }
    assert(swtab_size(&st) == 9);

    for (size_t i = 0; i < 9; i++) {
        assert(swtab_find(&st, entries[i].hash) == &entries[i]);
    }
    assert(swtab_find(&st, ((swtab_hash_t)43 << 7) | 60) == NULL);

    swtab_remove(&st, &entries[0], entries[0].hash);
    assert(swtab_find(&st, entries[0].hash) == NULL);
    assert(swtab_find(&st, entries[8].hash) == &entries[8]);

    swtab_remove(&st, &entries[8], entries[8].hash);
    assert(swtab_find(&st, entries[8].hash) == NULL);
    assert(swtab_size(&st) == 7);

    swtab_destroy(&st);
}

static void
test_reuse_deep_tombstone_before_grow(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);
    swtab_reserve(&st, 28);
    assert(st.group_mask == 3);

    struct hashed_entry entries[29];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((swtab_hash_t)(i * 4) << 7) | (i + 1);
        swtab_insert(&st, &entries[i], entries[i].hash);
    }
    assert(swtab_size(&st) == 28);
    assert(st.growth_left == 0);

    swtab_remove(&st, &entries[10], entries[10].hash);
    assert(swtab_find(&st, entries[10].hash) == NULL);
    assert(st.growth_left == 0);

    size_t mask = st.group_mask;
    entries[28].key = 28;
    entries[28].hash = ((swtab_hash_t)(100 * 4) << 7) | 90;
    swtab_insert(&st, &entries[28], entries[28].hash);

    assert(st.group_mask == mask);
    assert(swtab_size(&st) == 28);
    assert(swtab_find(&st, entries[28].hash) == &entries[28]);

    for (size_t i = 0; i < 28; i++) {
        if (i != 10) {
            assert(swtab_find(&st, entries[i].hash) == &entries[i]);
        }
    }

    swtab_destroy(&st);
}

static void
test_find_key_resolves_hash_collision(void)
{
    swtab st;
    swtab_init(&st, colliding_hash);

    struct keyed_entry a = { .key = 1 };
    struct keyed_entry b = { .key = 2 };
    struct keyed_entry c = { .key = 3 };
    swtab_hash_t hash = 42;

    swtab_insert(&st, &a, hash);
    swtab_insert(&st, &b, hash);
    swtab_insert(&st, &c, hash);

    int key = 2;
    assert(swtab_find_key(&st, hash, &key, keyed_entry_eq) == &b);

    key = 99;
    assert(swtab_find_key(&st, hash, &key, keyed_entry_eq) == NULL);

    swtab_destroy(&st);
}

static void
test_find_key_next(void)
{
    swtab st;
    swtab_init(&st, colliding_hash);

    struct keyed_entry a = { .key = 7 };
    struct keyed_entry b = { .key = 8 };
    struct keyed_entry c = { .key = 7 };
    swtab_hash_t hash = 42;

    swtab_insert(&st, &a, hash);
    swtab_insert(&st, &b, hash);
    swtab_insert(&st, &c, hash);

    int key = 7;
    void *first = swtab_find_key(&st, hash, &key, keyed_entry_eq);
    void *second = swtab_find_key_next(&st, hash, &key, keyed_entry_eq, first);
    void *third = swtab_find_key_next(&st, hash, &key, keyed_entry_eq, second);

    assert((first == &a && second == &c) ||
           (first == &c && second == &a));
    assert(third == NULL);

    swtab_destroy(&st);
}

static void
test_find_key_does_not_rehash_candidates(void)
{
    swtab st;
    swtab_init(&st, counted_entry_hash);

    struct counted_entry a = { .key = 7, .hash = 42 };
    struct counted_entry b = { .key = 8, .hash = 42 };
    struct counted_entry c = { .key = 7, .hash = 42 };
    swtab_hash_t hash = 42;

    swtab_insert(&st, &a, hash);
    swtab_insert(&st, &b, hash);
    swtab_insert(&st, &c, hash);

    counted_hash_calls = 0;

    int key = 7;
    void *first = swtab_find_key(&st, hash, &key, counted_entry_eq);
    void *second = swtab_find_key_next(&st, hash, &key, counted_entry_eq, first);
    void *third = swtab_find_key_next(&st, hash, &key, counted_entry_eq, second);

    assert((first == &a && second == &c) ||
           (first == &c && second == &a));
    assert(third == NULL);
    assert(counted_hash_calls == 0);

    swtab_destroy(&st);
}

static void
test_find_key_skips_same_h2_noise(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);

    swtab_hash_t target_hash = 0x55;
    struct hashed_entry noise1 = { .key = 1, .hash = 0xD5 };
    struct hashed_entry a = { .key = 7, .hash = target_hash };
    struct hashed_entry noise2 = { .key = 2, .hash = 0x155 };
    struct hashed_entry b = { .key = 7, .hash = target_hash };
    struct hashed_entry noise3 = { .key = 3, .hash = 0x1D5 };

    swtab_insert(&st, &noise1, noise1.hash);
    swtab_insert(&st, &a, a.hash);
    swtab_insert(&st, &noise2, noise2.hash);
    swtab_insert(&st, &b, b.hash);
    swtab_insert(&st, &noise3, noise3.hash);

    int key = 7;
    void *first = swtab_find_key(&st, target_hash, &key, hashed_entry_eq);
    void *second = swtab_find_key_next(&st, target_hash, &key,
                                       hashed_entry_eq, first);
    void *third = swtab_find_key_next(&st, target_hash, &key,
                                      hashed_entry_eq, second);

    assert((first == &a && second == &b) ||
           (first == &b && second == &a));
    assert(third == NULL);

    key = 99;
    assert(swtab_find_key(&st, target_hash, &key, hashed_entry_eq) == NULL);

    swtab_destroy(&st);
}

static void
test_remove(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    void *c = (void *)30;
    swtab_insert(&st, a, dummy_hash(a));
    swtab_insert(&st, b, dummy_hash(b));
    swtab_insert(&st, c, dummy_hash(c));

    swtab_remove(&st, b, dummy_hash(b));
    assert(swtab_size(&st) == 2);
    assert(swtab_find(&st, dummy_hash(b)) == NULL);
    assert(swtab_find(&st, dummy_hash(a)) == a);
    assert(swtab_find(&st, dummy_hash(c)) == c);

    swtab_remove(&st, a, dummy_hash(a));
    assert(swtab_size(&st) == 1);
    assert(swtab_find(&st, dummy_hash(a)) == NULL);
    assert(swtab_find(&st, dummy_hash(c)) == c);

    swtab_remove(&st, c, dummy_hash(c));
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));

    swtab_destroy(&st);
}

static void
test_clear(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    for (size_t i = 1; i <= 10; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(swtab_size(&st) == 10);

    swtab_clear(&st);
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));
    for (size_t i = 1; i <= 10; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == NULL);
    }

    for (size_t i = 100; i <= 105; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(swtab_size(&st) == 6);
    for (size_t i = 100; i <= 105; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    swtab_destroy(&st);
}

static void
test_clear_after_tombstone_heavy_table(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);
    swtab_reserve(&st, 28);

    struct hashed_entry entries[28];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((swtab_hash_t)(i * 4) << 7) | (i + 1);
        swtab_insert(&st, &entries[i], entries[i].hash);
    }
    assert(swtab_size(&st) == 28);

    for (size_t i = 0; i < 20; i++) {
        swtab_remove(&st, &entries[i], entries[i].hash);
    }
    assert(swtab_size(&st) == 8);

    swtab_clear(&st);
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));

    size_t count = 0;
    SWTAB_FOR_EACH(entry, &st) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    for (size_t i = 0; i < 28; i++) {
        assert(swtab_find(&st, entries[i].hash) == NULL);
    }

    struct hashed_entry fresh[5];
    for (size_t i = 0; i < 5; i++) {
        fresh[i].key = (int)i + 100;
        fresh[i].hash = ((swtab_hash_t)(i * 4) << 7) | (40 + i);
        swtab_insert(&st, &fresh[i], fresh[i].hash);
    }
    assert(swtab_size(&st) == 5);

    for (size_t i = 0; i < 5; i++) {
        assert(swtab_find(&st, fresh[i].hash) == &fresh[i]);
    }

    swtab_destroy(&st);
}

static void
test_reserve(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    swtab_reserve(&st, 100);
    size_t mask_after_reserve = st.group_mask;
    assert(mask_after_reserve > 0);

    for (size_t i = 1; i <= 100; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(st.group_mask == mask_after_reserve);
    assert(swtab_size(&st) == 100);

    for (size_t i = 1; i <= 100; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    swtab_destroy(&st);
}

static void
test_reserve_noop(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    swtab_reserve(&st, 5);
    size_t mask = st.group_mask;

    for (size_t i = 1; i <= 5; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }

    swtab_reserve(&st, 3);
    assert(st.group_mask == mask);

    swtab_destroy(&st);
}

static void
test_reserve_after_tombstone_churn(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);
    swtab_reserve(&st, 28);

    struct hashed_entry entries[28];
    for (size_t i = 0; i < 28; i++) {
        entries[i].key = (int)i;
        entries[i].hash = ((swtab_hash_t)(i * 4) << 7) | (i + 1);
        swtab_insert(&st, &entries[i], entries[i].hash);
    }
    assert(swtab_size(&st) == 28);

    size_t removed = 0;
    for (size_t i = 0; i < 28; i += 3) {
        swtab_remove(&st, &entries[i], entries[i].hash);
        removed++;
    }
    assert(swtab_size(&st) == 28 - removed);

    size_t old_mask = st.group_mask;
    swtab_reserve(&st, 100);
    assert(st.group_mask > old_mask);
    assert(swtab_size(&st) == 28 - removed);

    for (size_t i = 0; i < 28; i++) {
        if (i % 3 == 0) {
            assert(swtab_find(&st, entries[i].hash) == NULL);
        } else {
            assert(swtab_find(&st, entries[i].hash) == &entries[i]);
        }
    }

    swtab_destroy(&st);
}

static void
test_growth(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    size_t n = 500;
    size_t prev_mask = 0;
    int resizes = 0;

    for (size_t i = 1; i <= n; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
        if (st.group_mask != prev_mask) {
            resizes++;
            prev_mask = st.group_mask;

            for (size_t j = 1; j <= i; j++) {
                assert(swtab_find(&st, dummy_hash((void *)j)) == (void *)j);
            }
        }
    }

    assert(resizes >= 3);
    assert(swtab_size(&st) == n);

    for (size_t i = 1; i <= n; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    swtab_destroy(&st);
}

static void
test_large_table(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    size_t n = 50000;
    for (size_t i = 1; i <= n; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(swtab_size(&st) == n);

    for (size_t i = 1; i <= n; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    assert(swtab_find(&st, dummy_hash((void *)(n + 1))) == NULL);
    assert(swtab_find(&st, dummy_hash((void *)(n + 1000))) == NULL);

    swtab_destroy(&st);
}

static void
test_iteration(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    size_t n = 200;
    for (size_t i = 1; i <= n; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }

    bool seen[201] = {0};
    size_t count = 0;
    SWTAB_FOR_EACH(entry, &st) {
        size_t val = (size_t)entry;
        assert(val >= 1 && val <= n);
        assert(!seen[val]);
        seen[val] = true;
        count++;
    }
    assert(count == n);

    swtab_destroy(&st);
}

static void
test_iteration_empty(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    size_t count = 0;
    SWTAB_FOR_EACH(entry, &st) {
        (void)entry;
        count++;
    }
    assert(count == 0);

    swtab_destroy(&st);
}

static void
test_find_empty(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);
    assert(swtab_find(&st, dummy_hash((void *)1)) == NULL);
    assert(swtab_find(&st, 0) == NULL);
    assert(swtab_find(&st, 999) == NULL);
    swtab_destroy(&st);
}

static void
test_remove_empty(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);
    swtab_remove(&st, (void *)1, dummy_hash((void *)1));
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));
    swtab_destroy(&st);
}

static void
test_remove_nonexistent(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    swtab_insert(&st, a, dummy_hash(a));
    swtab_insert(&st, b, dummy_hash(b));

    swtab_remove(&st, (void *)30, dummy_hash((void *)30));
    assert(swtab_size(&st) == 2);
    assert(swtab_find(&st, dummy_hash(a)) == a);
    assert(swtab_find(&st, dummy_hash(b)) == b);

    swtab_destroy(&st);
}

static void
test_clear_empty(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);
    swtab_clear(&st);
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));
    swtab_destroy(&st);
}

static void
test_insert_after_remove(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    for (size_t i = 1; i <= 20; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 1; i <= 20; i++) {
        swtab_remove(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(swtab_size(&st) == 0);

    for (size_t i = 100; i <= 119; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(swtab_size(&st) == 20);
    for (size_t i = 100; i <= 119; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    swtab_destroy(&st);
}

static void
test_reinsert_same(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    void *entry = (void *)42;
    swtab_insert(&st, entry, dummy_hash(entry));
    assert(swtab_find(&st, dummy_hash(entry)) == entry);

    swtab_remove(&st, entry, dummy_hash(entry));
    assert(swtab_find(&st, dummy_hash(entry)) == NULL);

    swtab_insert(&st, entry, dummy_hash(entry));
    assert(swtab_find(&st, dummy_hash(entry)) == entry);
    assert(swtab_size(&st) == 1);

    swtab_destroy(&st);
}

static void
test_churn(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    for (int round = 0; round < 10; round++) {
        size_t base = (size_t)round * 100 + 1;
        for (size_t i = base; i < base + 100; i++) {
            swtab_insert(&st, (void *)i, dummy_hash((void *)i));
        }
        for (size_t i = base; i < base + 50; i++) {
            swtab_remove(&st, (void *)i, dummy_hash((void *)i));
        }
    }

    for (int round = 0; round < 10; round++) {
        size_t base = (size_t)round * 100 + 1;
        for (size_t i = base; i < base + 50; i++) {
            assert(swtab_find(&st, dummy_hash((void *)i)) == NULL);
        }
        for (size_t i = base + 50; i < base + 100; i++) {
            assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
        }
    }
    assert(swtab_size(&st) == 500);

    swtab_destroy(&st);
}

static void
test_load_factor_boundary(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    swtab_insert(&st, (void *)1, dummy_hash((void *)1));
    assert(st.group_mask == 0);
    size_t cap = (st.group_mask + 1) * 8;
    size_t threshold = cap * SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN;

    for (size_t i = 2; i <= threshold; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(st.group_mask == 0);

    swtab_insert(&st, (void *)(threshold + 1), dummy_hash((void *)(threshold + 1)));
    assert(st.group_mask > 0);

    for (size_t i = 1; i <= threshold + 1; i++) {
        assert(swtab_find(&st, dummy_hash((void *)i)) == (void *)i);
    }

    swtab_destroy(&st);
}

static void
test_remove_empty_slot_restores_growth(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    swtab_insert(&st, (void *)1, dummy_hash((void *)1));
    assert(st.group_mask == 0);
    size_t threshold = ((st.group_mask + 1) * 8) *
                       SWTAB_LOAD_FACTOR_NUM / SWTAB_LOAD_FACTOR_DEN;

    for (size_t i = 2; i <= threshold; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(st.group_mask == 0);
    assert(st.growth_left == 0);

    swtab_remove(&st, (void *)threshold, dummy_hash((void *)threshold));
    assert(st.growth_left == 1);

    swtab_insert(&st, (void *)100, dummy_hash((void *)100));
    assert(st.group_mask == 0);
    assert(st.growth_left == 0);
    assert(swtab_size(&st) == threshold);
    assert(swtab_find(&st, dummy_hash((void *)100)) == (void *)100);

    swtab_destroy(&st);
}

static void
test_reuse_tombstone_at_boundary(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    swtab_reserve(&st, 14);
    assert(st.group_mask == 1);

    for (size_t i = 1; i <= 8; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    for (size_t i = 129; i <= 134; i++) {
        swtab_insert(&st, (void *)i, dummy_hash((void *)i));
    }
    assert(swtab_size(&st) == 14);
    assert(st.growth_left == 0);

    swtab_remove(&st, (void *)1, dummy_hash((void *)1));
    assert(st.group_mask == 1);
    assert(st.growth_left == 0);

    swtab_insert(&st, (void *)9, dummy_hash((void *)9));
    assert(st.group_mask == 1);
    assert(st.growth_left == 0);
    assert(swtab_size(&st) == 14);
    assert(swtab_find(&st, dummy_hash((void *)9)) == (void *)9);

    swtab_destroy(&st);
}

static void
test_find_next_no_duplicates(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    void *a = (void *)10;
    void *b = (void *)20;
    swtab_insert(&st, a, dummy_hash(a));
    swtab_insert(&st, b, dummy_hash(b));

    assert(swtab_find_next(&st, dummy_hash(a), a) == NULL);
    assert(swtab_find_next(&st, dummy_hash(b), b) == NULL);

    swtab_destroy(&st);
}

static void
test_find_next_filters_same_h2(void)
{
    swtab st;
    swtab_init(&st, hashed_entry_hash);

    swtab_hash_t target_hash = 0x2A;
    struct hashed_entry a = { .key = 1, .hash = target_hash };
    struct hashed_entry b = { .key = 2, .hash = target_hash };
    struct hashed_entry c = { .key = 3, .hash = target_hash };
    struct hashed_entry noise1 = { .key = 4, .hash = 0xAA };
    struct hashed_entry noise2 = { .key = 5, .hash = 0x12A };
    struct hashed_entry noise3 = { .key = 6, .hash = 0x1AA };

    swtab_insert(&st, &noise1, noise1.hash);
    swtab_insert(&st, &a, a.hash);
    swtab_insert(&st, &noise2, noise2.hash);
    swtab_insert(&st, &b, b.hash);
    swtab_insert(&st, &noise3, noise3.hash);
    swtab_insert(&st, &c, c.hash);

    bool found_a = false, found_b = false, found_c = false;
    size_t count = 0;
    for (void *e = swtab_find(&st, target_hash);
         e;
         e = swtab_find_next(&st, target_hash, e)) {
        count++;
        if (e == &a) found_a = true;
        else if (e == &b) found_b = true;
        else if (e == &c) found_c = true;
        else assert(0 && "find_next returned same-H2 noise");
    }

    assert(count == 3);
    assert(found_a && found_b && found_c);

    swtab_destroy(&st);
}

static void
test_mixed_operations(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    swtab_insert(&st, (void *)1, dummy_hash((void *)1));
    swtab_insert(&st, (void *)2, dummy_hash((void *)2));
    swtab_insert(&st, (void *)3, dummy_hash((void *)3));
    assert(swtab_size(&st) == 3);

    swtab_remove(&st, (void *)2, dummy_hash((void *)2));
    assert(swtab_size(&st) == 2);

    swtab_clear(&st);
    assert(swtab_size(&st) == 0);

    swtab_insert(&st, (void *)10, dummy_hash((void *)10));
    swtab_insert(&st, (void *)11, dummy_hash((void *)11));
    assert(swtab_size(&st) == 2);

    assert(swtab_find(&st, dummy_hash((void *)1)) == NULL);
    assert(swtab_find(&st, dummy_hash((void *)10)) == (void *)10);
    assert(swtab_find(&st, dummy_hash((void *)11)) == (void *)11);

    swtab_destroy(&st);
}

int
main(void)
{
    printf("swtab tests:\n");
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
    RUN_TEST(test_mixed_operations);

    printf("\nAll tests passed.\n");
    return 0;
}
