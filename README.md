# swtab

swtab is a header-only C99 Swiss table hash map.

## Features

- **Fast** — SWAR group scanning checks 8 slots per step; benchmarks show 3-5x faster lookups than chained hash maps at scale ([see benchmarks](#benchmarks))
- **Cache-friendly** — flat, contiguous layout minimizes pointer chasing and cache misses
- **Header-only** — single file, no build system integration, no dependencies beyond the C standard library
- **Generic** — stores `void *` pointers; works with any data type
- **Small** — ~9 bytes/entry overhead at typical load factors

## Usage

Copy `swtab.h` into your project and include it:

```c
#include "swtab.h"
```

No build system integration needed — single header, no dependencies beyond the C standard library.

## Quick Example

```c
#include "swtab.h"
#include <stdio.h>
#include <stdlib.h>

static swtab_hash_t entry_hash(const void *entry) {
    return (swtab_hash_t)(uintptr_t)entry;
}

int main(void) {
    swtab st;
    swtab_init(&st, entry_hash);
    swtab_reserve(&st, 1000000);

    for (uintptr_t i = 1; i <= 1000000; i++)
        swtab_insert(&st, (void *)i, i);

    void *found = swtab_find(&st, 500000);
    printf("found key %ld in %zu entries\n",
           (long)(uintptr_t)found, swtab_size(&st));

    swtab_destroy(&st);
}
```

## API

Full documentation is in `swtab.h` above each function prototype.

| Function | Description |
|---|---|
| `swtab_init` | Initialize a table |
| `swtab_destroy` | Free table memory |
| `swtab_size` | Number of entries |
| `swtab_is_empty` | Check if empty |
| `swtab_clear` | Remove all entries, keep capacity |
| `swtab_reserve` | Pre-allocate capacity |
| `swtab_insert` | Insert an entry |
| `swtab_remove` | Remove an entry by pointer |
| `swtab_find` | Look up by hash |
| `swtab_find_next` | Continue a lookup (duplicates) |
| `SWTAB_FOR_EACH` | Iterate all entries |

## Building

Requires GCC or Clang and a C99-compatible toolchain.

Run the test suite:

```
make test
```

Run with AddressSanitizer and UndefinedBehaviorSanitizer:

```
make test-asan
```

Run the comparative benchmark (swtab vs chained hash map):

```
make bench
```

## Benchmarks

All numbers below are from a single machine and will vary by hardware.
Run `make bench` to reproduce on yours — the benchmark includes hardware
performance counters (L1/LLC misses, instructions, branch mispredictions)
when available via `perf_event_open`.

The benchmark compares swtab against a chained hash map (separate chaining
with linked lists, similar to OVS's hmap). Adding your own implementation
is straightforward: write an `impl_foo.h` adapter with the `bench_impl`
vtable and add it to the `impls[]` array in `bench/bench.c`.

### L2-resident (1024 entries)

| Operation | swtab (ns/op) | chained (ns/op) |
|---|---|---|
| insert_seq | 11.2 | 19.4 |
| find_hit | 3.8 | 2.0 |
| find_miss | 3.0 | 2.9 |
| remove | 3.6 | 6.5 |
| iterate | 2.3 | 1.9 |

### Beyond L3 (1M entries)

| Operation | swtab (ns/op) | chained (ns/op) |
|---|---|---|
| insert_seq | 28.9 | 46.0 |
| find_hit | 24.0 | 45.8 |
| find_miss | 7.2 | 55.5 |
| remove | 22.3 | 96.4 |
| iterate | 2.8 | 19.7 |

### Memory

| | swtab | chained |
|---|---|---|
| bytes/entry | 18.0 | 32.0 |

## Requirements

- C99 compiler (GCC or Clang)
- 64-bit platform (SWAR operations assume 64-bit `uint64_t`)

The benchmark (`make bench`) uses Linux `perf_event_open` for hardware
counters but falls back to wall-clock timing on other platforms.

## License

MIT
