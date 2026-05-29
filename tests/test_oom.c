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

#define SWTAB_MALLOC test_malloc
#define SWTAB_FREE test_free
#define SWTAB_OOM() do { oom_called++; longjmp(oom_env, 1); } while (0)

#include "../swtab.h"

static swtab_hash_t
dummy_hash(const void *entry)
{
    return (swtab_hash_t)entry;
}

int
main(void)
{
    swtab st;
    swtab_init(&st, dummy_hash);

    fail_alloc = 1;
    if (setjmp(oom_env) == 0) {
        swtab_insert(&st, (void *)1, dummy_hash((void *)1));
        assert(0 && "swtab_insert did not invoke SWTAB_OOM");
    }

    assert(oom_called == 1);
    assert(swtab_size(&st) == 0);
    assert(swtab_is_empty(&st));
    assert(swtab_find(&st, dummy_hash((void *)1)) == NULL);

    fail_alloc = 0;
    swtab_insert(&st, (void *)1, dummy_hash((void *)1));
    assert(swtab_size(&st) == 1);
    assert(swtab_find(&st, dummy_hash((void *)1)) == (void *)1);

    swtab_destroy(&st);
    return 0;
}
