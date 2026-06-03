# dshmap

dshmap is a header only C99 Swiss-style hash map with a small-table fast path.

## Features

- **Hybrid layout**: small tables use pooled separate chaining, then promote to the Swiss-table layout after `DSHMAP_SMALL_THRESHOLD` entries
- **Scale-oriented**: flat layouts reduce pointer chasing and cache misses; the largest Swiss-table wins show up on large tables, especially misses, iteration, and memory use ([see benchmarks](#benchmarks))
- **Cache-friendly**: small mode keeps bucket heads and nodes in one allocation; Swiss mode uses contiguous control bytes and slots
- **SIMD/SWAR control matching**: uses x86 SIMD on AVX2 targets and Clang SSE2 targets, NEON on ARM, and SWAR otherwise
- **Configurable Swiss hash storage**: store full hashes per Swiss slot when hash functions are expensive, or omit them to reduce memory use
- **Public iterators**: cursor-based iteration plus safe remove-current iteration
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
| `DSHMAP_INITIALIZER` | Static initializer for an empty table |
| `dshmap_init` | Initialize a table |
| `dshmap_destroy` | Free table memory |
| `dshmap_clear` | Remove all entries, keep capacity |
| `dshmap_shrink` | Reduce capacity to fit the current size |
| `dshmap_reserve` | Pre-allocate capacity |
| `dshmap_size` | Number of entries |
| `dshmap_is_empty` | Check if empty |
| `dshmap_insert` | Insert a non-`NULL` entry; inserting the same entry pointer twice without removing it first is unsupported |
| `dshmap_find` | Look up by hash; use `dshmap_find_key` when key equality matters |
| `dshmap_find_next` | Continue a lookup through distinct entries with the same hash |
| `dshmap_find_key` | Look up by hash and key equality; key-aware form of `dshmap_find` |
| `dshmap_find_key_next` | Continue a key-aware lookup through duplicate logical keys |
| `dshmap_remove` | Remove an entry by pointer |
| `dshmap_iter_init` | Initialize an all-entry iterator |
| `dshmap_iter_next` | Return the next entry from an iterator |
| `dshmap_iter_next_after` | Return the entry after a currently present entry |
| `DSHMAP_FOR_EACH` | Iterate all entries |
| `DSHMAP_FOR_EACH_SAFE` | Iterate all entries while removing the current entry |
| `DSHMAP_FOR_EACH_WITH_HASH` | Iterate entries with a matching full hash |

Use `DSHMAP_INITIALIZER(hash_fn)` for static storage or aggregate
initialization. Tables initialized this way are ready to use and still need
`dshmap_destroy()` when done.

`NULL` entries are not supported. `dshmap` uses `NULL` as the lookup miss result
and as an iteration sentinel.

Define `DSHMAP_SMALL_THRESHOLD` before including `dshmap.h` to tune the hybrid
cutover. The default is `2048`. Define it as `0` to disable small mode and use
the Swiss layout from the first insertion.

Small mode always stores the full hash in each pooled node. Swiss hash storage
is configurable. Define this macro before including `dshmap.h`:

```c
#define DSHMAP_SWISS_STORE_HASHES 0
#include "dshmap.h"
```

That is the default: small mode stores full hashes because it needs exact hash
checks inside each chain, while Swiss mode omits them to keep large tables
compact. Enabling Swiss hash storage costs one `dshmap_hash_t` per Swiss slot
but avoids recomputing hashes during some lookups, removals, and resizes. It
can help when hash functions are expensive or collision-heavy lookups are
common.

`DSHMAP_DENSE_THRESHOLD` is still accepted as an old name for
`DSHMAP_SMALL_THRESHOLD`.

Swiss control-byte matching uses the fastest simple backend found in local
benchmarks:

- x86 SIMD on AVX2 compiler targets
- x86 SIMD on Clang SSE2 targets
- NEON on ARM targets
- SWAR otherwise

GCC x86 builds without AVX2 use SWAR by default because the generic SSE2 path
was slower for successful key lookups in profiling. Compile GCC with `-mavx2`
or `-march=native` if you want the x86 SIMD backend where supported. To force
the fallback, define `DSHMAP_DISABLE_SIMD` before including `dshmap.h`:

```c
#define DSHMAP_DISABLE_SIMD 1
#include "dshmap.h"
```

SIMD backends use 16-slot Swiss groups. The SWAR fallback uses 8-slot groups
because that is faster for scalar control matching. This is normally an
internal detail, but it affects very low custom load factors: configs below
`1/8` can compile on SIMD targets and fail on the SWAR fallback. Use a load
factor of at least `1/8` if the same config must compile everywhere.

## Iteration

Use `DSHMAP_FOR_EACH` for normal all-entry iteration:

```c
DSHMAP_FOR_EACH(entry, &map) {
    process(entry);
}
```

Use `DSHMAP_FOR_EACH_SAFE` when the loop removes or frees the current entry:

```c
DSHMAP_FOR_EACH_SAFE(entry, next, &map) {
    dshmap_remove(&map, entry, entry_hash(entry));
    free(entry);
}
```

The safe macro is only for removing the current entry. Do not insert entries,
clear the table, or remove other entries during that loop.

For manual control, use `dshmap_iter`:

```c
dshmap_iter iter;
dshmap_iter_init(&iter, &map);
for (void *entry = dshmap_iter_next(&map, &iter);
     entry;
     entry = dshmap_iter_next(&map, &iter)) {
    process(entry);
}
```

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

Run the compile-time configuration matrix:

```
make test-config-matrix
```

Run with AddressSanitizer and UndefinedBehaviorSanitizer:

```
make test-asan
```

Run the comparative benchmark (dshmap vs every registered implementation):

```
make bench
```

Pass benchmark arguments through `BENCH_ARGS`:

```
make bench BENCH_ARGS="--help"
make bench BENCH_ARGS="--compare --geometric 1:65536:2 --ops find_hit,find_miss,mixed"
```

## Performance Profile

`dshmap` is designed to use a simple pooled chained table at small sizes, then
switch to Swiss layout once flat probing starts to pay off.

Small mode avoids paying Swiss-table probing overhead before the table is large
enough to benefit from it. The Swiss layout still does the heavy lifting for
larger tables, especially misses and iteration.

Benchmark with your workload if small-table latency matters.

## Benchmarks

All numbers below are from a single machine and will vary by hardware.
Run `make bench` to reproduce on yours: the benchmark includes hardware
performance counters (L1/LLC misses, instructions, branch mispredictions)
when available via `perf_event_open`.

The benchmark can also scan arbitrary table sizes. Use `--linear A:B[:STEP]`
for every size in a range, `--geometric A:B[:MUL]` for powers, `--sizes`
for explicit lists, `--keys ptr|string|expensive` to switch key/hash
workloads, `--impls dshmap,chained,...` to choose implementations, `--ops` to
limit operations, `--samples N` to choose the number of timed samples,
`--compare` for compact dshmap-vs-each-implementation rows, or `--csv` for
machine-readable output.
The `ptr` workload is the original integer-as-pointer benchmark; `string`
uses fixed string entries; `expensive` uses the same entries with a
deliberately expensive hash function.

The `mixed` workload is configurable. Use `--mixed-ratio F,I,R` to set
find/insert/remove percentages, `--mixed-initial PCT` to set the starting fill
percentage, `--mixed-iter-scans N` to set the target number of full-table
scans, and `--mixed-seed N` to choose a repeatable operation sequence. The
default is `--mixed-ratio 70,20,10 --mixed-initial 50 --mixed-iter-scans 8`.
`--mixed-iter-scans 0` disables iteration inside `mixed`.

The benchmark compares dshmap against every implementation registered in
`impls[]` in `bench/bench.c`. The current set includes a chained hash map,
a packed linear table, and a flat open-addressed table. The chained baseline
uses a reusable node pool after `reserve`, so reserved workloads do not charge
it a malloc/free per insert or remove. Adding your own implementation is
straightforward: write an `impl_foo.h` adapter with the `bench_impl` vtable
and add it to the `impls[]` array.

The table below uses the `ptr` workload, with hardware counters disabled.
Values are median wall-clock timings from 7 samples. Ratio is
`dshmap / candidate`, so lower is better. Layout is the dshmap layout used for
that size with the default `DSHMAP_SMALL_THRESHOLD=2048`. These numbers were
collected on x86_64 with the default GCC backend selection.

```
make bench BENCH_ARGS="--compare --sizes 64,2048,4096 --keys ptr --ops insert_seq,find_hit,find_miss,remove,iterate,mixed --min-ops 500000 --no-perf"
```

`find_hit` and `find_miss` perform keyed lookup, not hash-only lookup.
`insert_seq` reserves the target size before timing. `mixed` starts from a
half-full table, reserves the expected workload size, and runs a randomized
live-key workload of about 70% find, 20% insert, and 10% remove. It also does
periodic full-table iteration. Iteration work is counted as one operation per
entry visited.

| Size | Layout | Operation | Candidate | dshmap (ns/op) | candidate (ns/op) | Ratio | Winner |
|---:|---|---|---|---:|---:|---:|---|
| 64 | small | `insert_seq` | chained | 3.3 | 2.3 | 1.417 | chained |
| 64 | small | `insert_seq` | packed | 3.3 | 1.5 | 2.241 | packed |
| 64 | small | `insert_seq` | flat | 3.3 | 2.2 | 1.481 | flat |
| 64 | small | `find_hit` | chained | 2.9 | 2.7 | 1.080 | chained |
| 64 | small | `find_hit` | packed | 2.9 | 16.3 | 0.178 | dshmap |
| 64 | small | `find_hit` | flat | 2.9 | 3.0 | 0.965 | dshmap |
| 64 | small | `find_miss` | chained | 2.6 | 2.3 | 1.123 | chained |
| 64 | small | `find_miss` | packed | 2.6 | 35.2 | 0.075 | dshmap |
| 64 | small | `find_miss` | flat | 2.6 | 3.1 | 0.856 | dshmap |
| 64 | small | `remove` | chained | 3.3 | 1.9 | 1.731 | chained |
| 64 | small | `remove` | packed | 3.3 | 4.9 | 0.668 | dshmap |
| 64 | small | `remove` | flat | 3.3 | 3.3 | 0.996 | dshmap |
| 64 | small | `iterate` | chained | 2.0 | 1.7 | 1.180 | chained |
| 64 | small | `iterate` | packed | 2.0 | 1.0 | 1.953 | packed |
| 64 | small | `iterate` | flat | 2.0 | 2.2 | 0.884 | dshmap |
| 64 | small | `mixed` | chained | 4.3 | 3.8 | 1.141 | chained |
| 64 | small | `mixed` | packed | 4.3 | 3.2 | 1.351 | packed |
| 64 | small | `mixed` | flat | 4.3 | 5.4 | 0.811 | dshmap |
| 2048 | small | `insert_seq` | chained | 2.9 | 2.6 | 1.131 | chained |
| 2048 | small | `insert_seq` | packed | 2.9 | 1.6 | 1.828 | packed |
| 2048 | small | `insert_seq` | flat | 2.9 | 2.2 | 1.318 | flat |
| 2048 | small | `find_hit` | chained | 3.5 | 3.2 | 1.104 | chained |
| 2048 | small | `find_hit` | packed | 3.5 | 460.4 | 0.008 | dshmap |
| 2048 | small | `find_hit` | flat | 3.5 | 3.3 | 1.057 | flat |
| 2048 | small | `find_miss` | chained | 2.6 | 2.4 | 1.059 | chained |
| 2048 | small | `find_miss` | packed | 2.6 | 908.1 | 0.003 | dshmap |
| 2048 | small | `find_miss` | flat | 2.6 | 2.8 | 0.902 | dshmap |
| 2048 | small | `remove` | chained | 4.0 | 2.4 | 1.675 | chained |
| 2048 | small | `remove` | packed | 4.0 | 163.8 | 0.025 | dshmap |
| 2048 | small | `remove` | flat | 4.0 | 3.6 | 1.112 | flat |
| 2048 | small | `iterate` | chained | 1.8 | 1.5 | 1.154 | chained |
| 2048 | small | `iterate` | packed | 1.8 | 0.9 | 2.055 | packed |
| 2048 | small | `iterate` | flat | 1.8 | 2.1 | 0.841 | dshmap |
| 2048 | small | `mixed` | chained | 3.5 | 7.4 | 0.479 | dshmap |
| 2048 | small | `mixed` | packed | 3.5 | 60.0 | 0.059 | dshmap |
| 2048 | small | `mixed` | flat | 3.5 | 10.5 | 0.335 | dshmap |
| 4096 | Swiss | `insert_seq` | chained | 3.8 | 2.7 | 1.405 | chained |
| 4096 | Swiss | `insert_seq` | packed | 3.8 | 1.6 | 2.354 | packed |
| 4096 | Swiss | `insert_seq` | flat | 3.8 | 2.5 | 1.535 | flat |
| 4096 | Swiss | `find_hit` | chained | 4.4 | 4.9 | 0.897 | dshmap |
| 4096 | Swiss | `find_hit` | packed | 4.4 | 932.7 | 0.005 | dshmap |
| 4096 | Swiss | `find_hit` | flat | 4.4 | 4.7 | 0.923 | dshmap |
| 4096 | Swiss | `find_miss` | chained | 4.0 | 2.6 | 1.536 | chained |
| 4096 | Swiss | `find_miss` | packed | 4.0 | 1898.5 | 0.002 | dshmap |
| 4096 | Swiss | `find_miss` | flat | 4.0 | 3.1 | 1.303 | flat |
| 4096 | Swiss | `remove` | chained | 4.7 | 3.0 | 1.582 | chained |
| 4096 | Swiss | `remove` | packed | 4.7 | 326.6 | 0.014 | dshmap |
| 4096 | Swiss | `remove` | flat | 4.7 | 6.0 | 0.784 | dshmap |
| 4096 | Swiss | `iterate` | chained | 2.1 | 1.6 | 1.293 | chained |
| 4096 | Swiss | `iterate` | packed | 2.1 | 0.9 | 2.361 | packed |
| 4096 | Swiss | `iterate` | flat | 2.1 | 4.0 | 0.532 | dshmap |
| 4096 | Swiss | `mixed` | chained | 6.1 | 10.6 | 0.571 | dshmap |
| 4096 | Swiss | `mixed` | packed | 6.1 | 120.5 | 0.050 | dshmap |
| 4096 | Swiss | `mixed` | flat | 6.1 | 14.6 | 0.416 | dshmap |

For large tables, the packed linear table is not useful for lookup-heavy
workloads. The table below compares dshmap with the chained baseline only:

```
make bench BENCH_ARGS="--compare --impls dshmap,chained --sizes 65536,1048576,10000000 --keys ptr --ops insert_seq,find_hit,find_miss,remove,iterate,mixed --min-ops 500000 --no-perf"
```

| Size | Layout | Operation | dshmap (ns/op) | chained (ns/op) | Ratio | Winner |
|---:|---|---|---:|---:|---:|---|
| 65536 | Swiss | `insert_seq` | 6.3 | 3.0 | 2.11 | chained |
| 65536 | Swiss | `find_hit` | 5.2 | 10.6 | 0.49 | dshmap |
| 65536 | Swiss | `find_miss` | 4.3 | 11.1 | 0.39 | dshmap |
| 65536 | Swiss | `remove` | 5.7 | 6.5 | 0.88 | dshmap |
| 65536 | Swiss | `iterate` | 3.1 | 7.5 | 0.42 | dshmap |
| 65536 | Swiss | `mixed` | 9.3 | 16.3 | 0.57 | dshmap |
| 1048576 | Swiss | `insert_seq` | 14.9 | 10.9 | 1.37 | chained |
| 1048576 | Swiss | `find_hit` | 27.7 | 41.7 | 0.67 | dshmap |
| 1048576 | Swiss | `find_miss` | 8.1 | 33.8 | 0.24 | dshmap |
| 1048576 | Swiss | `remove` | 25.3 | 32.1 | 0.79 | dshmap |
| 1048576 | Swiss | `iterate` | 3.5 | 16.7 | 0.21 | dshmap |
| 1048576 | Swiss | `mixed` | 22.4 | 38.7 | 0.58 | dshmap |
| 10000000 | Swiss | `insert_seq` | 31.3 | 28.0 | 1.12 | chained |
| 10000000 | Swiss | `find_hit` | 56.0 | 50.5 | 1.11 | chained |
| 10000000 | Swiss | `find_miss` | 22.9 | 34.8 | 0.66 | dshmap |
| 10000000 | Swiss | `remove` | 50.4 | 42.7 | 1.18 | chained |
| 10000000 | Swiss | `iterate` | 3.1 | 21.4 | 0.14 | dshmap |
| 10000000 | Swiss | `mixed` | 28.3 | 47.9 | 0.59 | dshmap |

### Memory

Values are bytes per entry.

| Size | dshmap | chained | packed | flat |
|---:|---:|---:|---:|---:|
| 64 | 32.0 | 32.0 | 16.0 | 32.0 |
| 2048 | 32.0 | 32.0 | 16.0 | 32.0 |
| 4096 | 18.0 | 32.0 | 16.0 | 32.0 |
| 65536 | 18.0 | 32.0 | - | - |
| 1048576 | 18.0 | 32.0 | - | - |
| 10000000 | 15.1 | 37.4 | - | - |

## Requirements

- C99 compiler (GCC or Clang)
- Little-endian platform
- `uint64_t` support for the SWAR fallback

The benchmark (`make bench`) uses Linux `perf_event_open` for hardware
counters but falls back to wall-clock timing on other platforms.
