# swtab

swtab is a header-only C99 swiss table hash map.

## Features

- **Scale-oriented**: flat Swiss-table layout reduces pointer chasing and cache misses; the largest wins show up on large tables, especially misses, removes, iteration, and memory use ([see benchmarks](#benchmarks))
- **Cache-friendly**: contiguous control bytes and slots improve locality compared with pointer-heavy tables
- **Header-only**: single file, no build system integration, no dependencies beyond the C standard library
- **Generic**: stores caller-owned non-`NULL` `void *` entries; each entry pointer may be present at most once
- **Small**: ~9 bytes/entry overhead at typical load factors

## Usage

Copy `swtab.h` into your project and include it:

```c
#include "swtab.h"
```

No build system integration needed: single header, no dependencies beyond the C standard library.

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

Full documentation is in `swtab.h`.

| Function | Description |
|---|---|
| `swtab_init` | Initialize a table |
| `swtab_destroy` | Free table memory |
| `swtab_size` | Number of entries |
| `swtab_is_empty` | Check if empty |
| `swtab_clear` | Remove all entries, keep capacity |
| `swtab_reserve` | Pre-allocate capacity |
| `swtab_insert` | Insert a non-`NULL` entry; inserting the same entry pointer twice without removing it first is unsupported |
| `swtab_remove` | Remove an entry by pointer |
| `swtab_find` | Look up by hash; use `swtab_find_key` when key equality matters |
| `swtab_find_key` | Look up by hash and key equality; key-aware form of `swtab_find` |
| `swtab_find_key_next` | Continue a key-aware lookup through duplicate logical keys |
| `swtab_find_next` | Continue a lookup through distinct entries with the same hash |
| `SWTAB_FOR_EACH` | Iterate all entries |

`NULL` entries are not supported. `swtab` uses `NULL` as the lookup miss result
and as an iteration sentinel.

## Allocation Failure

Allocation failure is fatal by default. If allocation fails, or if size
arithmetic overflows while growing/reserving, `swtab` calls `SWTAB_OOM()`.
The default hook calls `abort()`.

Define `SWTAB_OOM`, `SWTAB_MALLOC`, and `SWTAB_FREE` before including
`swtab.h` to customize allocation behavior. `SWTAB_OOM()` should not return
normally; if it does, `swtab` aborts.

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

Pass benchmark arguments through `BENCH_ARGS`:

```
make bench BENCH_ARGS="--compare --geometric 1:65536:2 --ops find_hit,find_miss,mixed"
```

## Performance Profile

`swtab` is designed for large, cache-sensitive tables. Its flat layout tends
to pay off once pointer chasing and cache misses dominate.

For small tables, it is not guaranteed to beat simpler hash maps. Function
pointer hashing, control-byte probing, and Swiss-table bookkeeping can cost
more than a simple chained table or linear structure at small sizes.

Benchmark with your workload if small-table latency matters.

## Benchmarks

All numbers below are from a single machine and will vary by hardware.
Run `make bench` to reproduce on yours: the benchmark includes hardware
performance counters (L1/LLC misses, instructions, branch mispredictions)
when available via `perf_event_open`.

The benchmark can also scan arbitrary table sizes. Use `--linear A:B[:STEP]`
for every size in a range, `--geometric A:B[:MUL]` for powers, `--sizes`
for explicit lists, `--ops` to limit operations, `--compare` for a compact
swtab/chained crossover table, or `--csv` for machine-readable output.

The benchmark compares swtab against a chained hash map (separate chaining
with linked lists). Adding your own implementation
is straightforward: write an `impl_foo.h` adapter with the `bench_impl`
vtable and add it to the `impls[]` array in `bench/bench.c`.

### Small/L2-Resident: Mixed Results

| Operation | swtab (ns/op) | chained (ns/op) |
|---|---|---|
| insert_seq | 11.2 | 19.4 |
| find_hit | 3.8 | 2.0 |
| find_miss | 3.0 | 2.9 |
| remove | 3.6 | 6.5 |
| iterate | 2.3 | 1.9 |

### Large/Beyond L3: Strong Wins

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
- Little-endian 64-bit platform (SWAR operations assume 64-bit `uint64_t`
  and little-endian control-byte decoding)

The benchmark (`make bench`) uses Linux `perf_event_open` for hardware
counters but falls back to wall-clock timing on other platforms.

## License

MIT
