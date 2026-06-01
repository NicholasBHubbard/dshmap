# dshmap

dshmap is a header only C99 swiss-style hash map with a dense small-table fast path.

## Features

- **Hybrid layout**: small tables use a dense open-addressed layout, then promote to the Swiss-table layout after `DSHMAP_DENSE_THRESHOLD` entries
- **Scale-oriented**: flat layouts reduce pointer chasing and cache misses; the largest Swiss-table wins show up on large tables, especially misses, removes, iteration, and memory use ([see benchmarks](#benchmarks))
- **Cache-friendly**: contiguous control bytes and slots improve locality compared with pointer-heavy tables
- **Configurable hash storage**: store full hashes per slot when hash functions are expensive, or omit them to reduce memory use
- **Header-only**: single file, no build system integration, no dependencies beyond the C standard library
- **Generic**: stores caller-owned non-`NULL` `void *` entries; each entry pointer may be present at most once
- **Small**: ~9 bytes/entry overhead in Swiss mode at typical load factors

## Usage

Copy `dshmap.h` into your project and include it:

```c
#include "dshmap.h"
```

No build system integration needed: single header, no dependencies beyond the C standard library.

## Quick Example

```c
#include "dshmap.h"
#include <stdio.h>
#include <stdlib.h>

static dshmap_hash_t entry_hash(const void *entry) {
    return (dshmap_hash_t)(uintptr_t)entry;
}

int main(void) {
    dshmap map;
    dshmap_init(&map, entry_hash);
    dshmap_reserve(&map, 1000000);

    for (uintptr_t i = 1; i <= 1000000; i++)
        dshmap_insert(&map, (void *)i, i);

    void *found = dshmap_find(&map, 500000);
    printf("found key %ld in %zu entries\n",
           (long)(uintptr_t)found, dshmap_size(&map));

    dshmap_destroy(&map);
}
```

## API

Full documentation is in `dshmap.h`.

| Function | Description |
|---|---|
| `dshmap_init` | Initialize a table |
| `dshmap_destroy` | Free table memory |
| `dshmap_clear` | Remove all entries, keep capacity |
| `dshmap_reserve` | Pre-allocate capacity |
| `dshmap_size` | Number of entries |
| `dshmap_is_empty` | Check if empty |
| `dshmap_insert` | Insert a non-`NULL` entry; inserting the same entry pointer twice without removing it first is unsupported |
| `dshmap_find` | Look up by hash; use `dshmap_find_key` when key equality matters |
| `dshmap_find_next` | Continue a lookup through distinct entries with the same hash |
| `dshmap_find_key` | Look up by hash and key equality; key-aware form of `dshmap_find` |
| `dshmap_find_key_next` | Continue a key-aware lookup through duplicate logical keys |
| `dshmap_remove` | Remove an entry by pointer |
| `DSHMAP_FOR_EACH` | Iterate all entries |
| `DSHMAP_FOR_EACH_WITH_HASH` | Iterate entries with a matching full hash |

`NULL` entries are not supported. `dshmap` uses `NULL` as the lookup miss result
and as an iteration sentinel.

Define `DSHMAP_DENSE_THRESHOLD` before including `dshmap.h` to tune the hybrid
cutover. The default is `2048`. Define it as `0` to disable dense mode and use
the Swiss layout from the first insertion.

Hash storage is independently configurable for dense and Swiss layouts. Define
these macros before including `dshmap.h`:

```c
#define DSHMAP_DENSE_STORE_HASHES 1
#define DSHMAP_SWISS_STORE_HASHES 0
#include "dshmap.h"
```

Those are the defaults: dense mode stores full hashes because small tables are
sensitive to repeated hash recomputation, while Swiss mode omits them to keep
large tables compact. Disabling hash storage saves one `dshmap_hash_t` per slot
but recomputes hashes during some lookups, removals, and resizes. Enabling hash
storage can help when hash functions are expensive or collision-heavy lookups
are common.

## Allocation Failure

Allocation failure is fatal by default. If allocation fails, or if size
arithmetic overflows while growing/reserving, `dshmap` calls `DSHMAP_OOM()`.
The default hook calls `abort()`.

Define `DSHMAP_OOM`, `DSHMAP_MALLOC`, and `DSHMAP_FREE` before including
`dshmap.h` to customize allocation behavior. `DSHMAP_OOM()` should not return
normally; if it does, `dshmap` aborts.

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

Run the comparative benchmark (dshmap vs chained hash map):

```
make bench
```

Pass benchmark arguments through `BENCH_ARGS`:

```
make bench BENCH_ARGS="--help"
make bench BENCH_ARGS="--compare --geometric 1:65536:2 --ops find_hit,find_miss,mixed"
```

## Performance Profile

`dshmap` is designed for large, cache-sensitive tables. Its flat layout tends
to pay off once pointer chasing and cache misses dominate.

Dense mode reduces small-table overhead, especially for inserts, removes, and
mixed workloads. It still is not guaranteed to beat simpler hash maps: direct
hit lookups and iteration can remain faster in a simple chained table or
linear structure at small sizes.

Benchmark with your workload if small-table latency matters.

## Benchmarks

All numbers below are from a single machine and will vary by hardware.
Run `make bench` to reproduce on yours: the benchmark includes hardware
performance counters (L1/LLC misses, instructions, branch mispredictions)
when available via `perf_event_open`.

The benchmark can also scan arbitrary table sizes. Use `--linear A:B[:STEP]`
for every size in a range, `--geometric A:B[:MUL]` for powers, `--sizes`
for explicit lists, `--keys ptr|string|expensive` to switch key/hash
workloads, `--ops` to limit operations, `--compare` for a compact
dshmap/chained crossover table, or `--csv` for machine-readable output.
The `ptr` workload is the original integer-as-pointer benchmark; `string`
uses fixed string entries; `expensive` uses the same entries with a
deliberately expensive hash function.

The benchmark compares dshmap against a chained hash map (separate chaining
with linked lists). Adding your own implementation
is straightforward: write an `impl_foo.h` adapter with the `bench_impl`
vtable and add it to the `impls[]` array in `bench/bench.c`.

The table below uses the `ptr` workload, with hardware counters disabled.
Values are median wall-clock timings from 7 runs. Ratio is
`dshmap / chained`, so lower is better.

```
make bench BENCH_ARGS="--csv --sizes 64,2048,4096,65536,1048576 --keys ptr --ops insert_seq,find_hit,find_miss,remove,iterate,mixed --min-ops 500000 --no-perf"
```

`mixed` starts from a half-full table and runs a randomized workload of about
70% find, 20% insert, and 10% remove.

| Size | Operation | dshmap (ns/op) | chained (ns/op) | Ratio | Winner |
|---:|---|---:|---:|---:|---|
| 64 | `insert_seq` | 8.0 | 7.4 | 1.08 | chained |
| 64 | `find_hit` | 3.0 | 2.1 | 1.45 | chained |
| 64 | `find_miss` | 2.9 | 2.8 | 1.03 | chained |
| 64 | `remove` | 4.9 | 5.4 | 0.91 | dshmap |
| 64 | `iterate` | 1.4 | 1.6 | 0.88 | dshmap |
| 64 | `mixed` | 3.2 | 3.4 | 0.94 | dshmap |
| 2048 | `insert_seq` | 9.3 | 20.5 | 0.46 | dshmap |
| 2048 | `find_hit` | 3.2 | 2.2 | 1.45 | chained |
| 2048 | `find_miss` | 2.8 | 3.2 | 0.88 | dshmap |
| 2048 | `remove` | 5.1 | 7.9 | 0.64 | dshmap |
| 2048 | `iterate` | 2.5 | 1.8 | 1.38 | chained |
| 2048 | `mixed` | 3.2 | 5.4 | 0.60 | dshmap |
| 4096 | `insert_seq` | 15.1 | 18.2 | 0.83 | dshmap |
| 4096 | `find_hit` | 3.9 | 6.9 | 0.57 | dshmap |
| 4096 | `find_miss` | 3.7 | 8.4 | 0.44 | dshmap |
| 4096 | `remove` | 4.6 | 10.7 | 0.43 | dshmap |
| 4096 | `iterate` | 2.5 | 2.7 | 0.93 | dshmap |
| 4096 | `mixed` | 4.7 | 9.8 | 0.48 | dshmap |
| 65536 | `insert_seq` | 18.0 | 16.7 | 1.08 | chained |
| 65536 | `find_hit` | 4.7 | 14.6 | 0.32 | dshmap |
| 65536 | `find_miss` | 4.4 | 16.4 | 0.27 | dshmap |
| 65536 | `remove` | 6.1 | 17.5 | 0.35 | dshmap |
| 65536 | `iterate` | 2.6 | 7.4 | 0.35 | dshmap |
| 65536 | `mixed` | 7.8 | 16.1 | 0.49 | dshmap |
| 1048576 | `insert_seq` | 23.8 | 36.8 | 0.65 | dshmap |
| 1048576 | `find_hit` | 25.4 | 41.9 | 0.61 | dshmap |
| 1048576 | `find_miss` | 7.6 | 48.6 | 0.16 | dshmap |
| 1048576 | `remove` | 26.2 | 92.3 | 0.28 | dshmap |
| 1048576 | `iterate` | 2.8 | 20.0 | 0.14 | dshmap |
| 1048576 | `mixed` | 36.5 | 57.8 | 0.63 | dshmap |

### Memory

| Size | dshmap bytes/entry | chained bytes/entry |
|---:|---:|---:|
| 64 | 34.0 | 32.0 |
| 2048 | 34.0 | 32.0 |
| 4096 | 18.0 | 32.0 |
| 65536 | 18.0 | 32.0 |
| 1048576 | 18.0 | 32.0 |

## Requirements

- C99 compiler (GCC or Clang)
- Little-endian 64-bit platform (SWAR operations assume 64-bit `uint64_t`
  and little-endian control-byte decoding)

The benchmark (`make bench`) uses Linux `perf_event_open` for hardware
counters but falls back to wall-clock timing on other platforms.

## License

MIT
