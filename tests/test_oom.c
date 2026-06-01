#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>

static jmp_buf oom_env;
static int fail_alloc;
static int oom_called;

static void *
test_malloc(size_t size)
{
    if (fail_alloc) {
        return NULL;
    }
    return malloc(size);
}

static void
test_free(void *ptr)
{
    free(ptr);
}

#define DSHMAP_MALLOC test_malloc
#define DSHMAP_FREE test_free
#define DSHMAP_OOM() do { oom_called++; longjmp(oom_env, 1); } while (0)

#include "../dshmap.h"

static dshmap_hash_t
dummy_hash(const void *entry)
{
    return (dshmap_hash_t)entry;
}

int
main(void)
{
    dshmap map;
    dshmap_init(&map, dummy_hash);

    fail_alloc = 1;
    if (setjmp(oom_env) == 0) {
        dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
        assert(0 && "dshmap_insert did not invoke DSHMAP_OOM");
    }

    assert(oom_called == 1);
    assert(dshmap_size(&map) == 0);
    assert(dshmap_is_empty(&map));
    assert(dshmap_find(&map, dummy_hash((void *)1)) == NULL);

    fail_alloc = 0;
    dshmap_insert(&map, (void *)1, dummy_hash((void *)1));
    assert(dshmap_size(&map) == 1);
    assert(dshmap_find(&map, dummy_hash((void *)1)) == (void *)1);

    dshmap_destroy(&map);
    return 0;
}
