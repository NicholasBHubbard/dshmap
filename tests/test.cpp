#include <cassert>
#include <cstddef>
#include <cstdio>

#include "../dshmap.h"

struct Entry {
    int key;
    dshmap_hash_t hash;
};

static dshmap_hash_t
entry_hash(const void *entry)
{
    const Entry *e = static_cast<const Entry *>(entry);
    return e->hash;
}

static bool
entry_eq(const void *entry, const void *key)
{
    const Entry *e = static_cast<const Entry *>(entry);
    const int *k = static_cast<const int *>(key);
    return e->key == *k;
}

int
main()
{
    dshmap map;
    dshmap_init(&map, entry_hash);
    assert(dshmap_capacity(&map) == 0);

    Entry entries[] = {
        {1, 0x101},
        {2, 0x181},
        {3, 0x202},
        {4, 0x282},
    };

    dshmap_reserve(&map, 32);
    assert(dshmap_capacity(&map) >= 32);

    for (std::size_t i = 0; i < sizeof entries / sizeof entries[0]; i++) {
        dshmap_insert(&map, &entries[i], entries[i].hash);
    }

    assert(dshmap_size(&map) == sizeof entries / sizeof entries[0]);
    assert(dshmap_find(&map, entries[0].hash) == &entries[0]);
    assert(dshmap_find(&map, entries[1].hash) == &entries[1]);

    int key = 3;
    assert(dshmap_find_key(&map, entries[2].hash, &key, entry_eq) ==
           &entries[2]);

    std::size_t count = 0;
    DSHMAP_FOR_EACH(entry, &map) {
        Entry *e = static_cast<Entry *>(entry);
        assert(e->key >= 1 && e->key <= 4);
        count++;
    }
    assert(count == sizeof entries / sizeof entries[0]);

    dshmap_remove(&map, &entries[1], entries[1].hash);
    assert(dshmap_find(&map, entries[1].hash) == nullptr);
    assert(dshmap_size(&map) == 3);

    dshmap_shrink(&map);
    assert(dshmap_capacity(&map) >= dshmap_size(&map));

    dshmap_clear(&map);
    assert(dshmap_is_empty(&map));
    dshmap_destroy(&map);

    std::puts("dshmap C++ smoke test: ok");
    return 0;
}
