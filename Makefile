CC      ?= gcc
CXX     ?= c++
CFLAGS  := -O2 -Wall -Wextra -Werror -std=c99
CXXFLAGS := -O2 -Wall -Wextra -Werror -std=c++11
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: test test-asan test-config-matrix bench coverage clean

test: tests/test.c tests/test.cpp tests/test_oom.c bench/bench.c dshmap.h
	$(CC) $(CFLAGS) -o tests/test tests/test.c
	./tests/test
	$(CC) $(CFLAGS) -DDSHMAP_SMALL_THRESHOLD=16 -o tests/test-small-threshold tests/test.c
	./tests/test-small-threshold
	$(CXX) $(CXXFLAGS) -o tests/test-cxx tests/test.cpp
	./tests/test-cxx
	$(CC) $(CFLAGS) -o tests/test-oom tests/test_oom.c
	./tests/test-oom
	$(CC) $(CFLAGS) -o bench/bench bench/bench.c
	./bench/bench --compare --sizes 1,2 --ops find_hit,mixed --min-ops 1 --no-perf >/dev/null
	./bench/bench --compare --sizes 2 --ops find_hit,find_miss,mixed --samples 3 --min-ops 1 --no-perf >/dev/null
	./bench/bench --compare --sizes 2 --ops find_hit --samples 1 --min-ops 1 --no-perf | grep -q ' packed '
	./bench/bench --compare --sizes 2 --ops find_hit --samples 1 --min-ops 1 --no-perf | grep -q ' flat '
	./bench/bench --compare --impls dshmap,chained --sizes 2 --ops find_hit --samples 1 --min-ops 1 --no-perf | grep -q ' chained '
	@if ./bench/bench --compare --impls dshmap,chained --sizes 2 --ops find_hit --samples 1 --min-ops 1 --no-perf | grep -q ' packed '; then exit 1; fi
	./bench/bench --csv --impls ,dshmap --sizes 1 --ops find_hit --samples 1 --min-ops 1 --no-perf 2>&1 | grep -q 'invalid --impls value'
	./bench/bench --csv --linear 1:3 --ops find_miss --min-ops 1 --no-perf >/dev/null
	./bench/bench --csv --sizes 8 --ops insert_seq,find_hit,find_miss,remove,iterate,mixed --samples 3 --min-ops 1 --no-perf >/dev/null
	./bench/bench --csv --sizes 8 --ops mixed --mixed-ratio 80,10,10 --mixed-initial 75 --mixed-iter-scans 0 --mixed-seed 123 --samples 3 --min-ops 1 --no-perf >/dev/null
	./bench/bench --csv --mixed-ratio 80,10,5 --sizes 1 --ops mixed --samples 1 --min-ops 1 --no-perf 2>&1 | grep -q 'invalid --mixed-ratio value'
	./bench/bench --compare --sizes 2 --keys string --ops find_hit --min-ops 1 --no-perf >/dev/null
	./bench/bench --csv --sizes 8 --keys string --ops find_hit,find_miss,mixed --samples 3 --min-ops 1 --no-perf >/dev/null
	./bench/bench --csv --sizes 2 --keys expensive --ops find_miss --min-ops 1 --no-perf >/dev/null

test-asan: tests/test.c tests/test_oom.c dshmap.h
	$(CC) $(CFLAGS) $(ASANFLAGS) -o tests/test-asan tests/test.c
	./tests/test-asan
	$(CC) $(CFLAGS) $(ASANFLAGS) -DDSHMAP_SMALL_THRESHOLD=16 -o tests/test-small-threshold-asan tests/test.c
	./tests/test-small-threshold-asan
	$(CC) $(CFLAGS) $(ASANFLAGS) -o tests/test-oom-asan tests/test_oom.c
	./tests/test-oom-asan

test-config-matrix: tests/test.c dshmap.h
	@set -eu; \
	for cfg in \
	    "swiss_hashes_on:-DDSHMAP_SWISS_STORE_HASHES=1" \
	    "small_disabled:-DDSHMAP_SMALL_THRESHOLD=0" \
	    "tiny_small:-DDSHMAP_SMALL_THRESHOLD=1" \
	    "odd_small:-DDSHMAP_SMALL_THRESHOLD=3" \
	    "swar_fallback:-DDSHMAP_DISABLE_SIMD=1" \
	    "swar_small_disabled:-DDSHMAP_DISABLE_SIMD=1 -DDSHMAP_SMALL_THRESHOLD=0" \
	    "low_load:-DDSHMAP_LOAD_FACTOR_NUM=1 -DDSHMAP_LOAD_FACTOR_DEN=8" \
	    "half_load:-DDSHMAP_LOAD_FACTOR_NUM=1 -DDSHMAP_LOAD_FACTOR_DEN=2"; \
	do \
	    name=$${cfg%%:*}; \
	    defs=$${cfg#*:}; \
	    printf 'config %-20s ' "$$name"; \
	    $(CC) $(CFLAGS) $$defs -o /tmp/dshmap-test-$$name tests/test.c; \
	    /tmp/dshmap-test-$$name >/dev/null; \
	    printf 'ok\n'; \
	done; \
	cast_align_warning=-Wcast-align; \
	if printf 'int dshmap_warning_probe;\n' | \
	    $(CC) $(CFLAGS) -Wcast-align=strict -x c -c -o /tmp/dshmap-cast-align-probe.o - >/dev/null 2>&1; \
	then \
	    cast_align_warning=-Wcast-align=strict; \
	fi; \
	rm -f /tmp/dshmap-cast-align-probe.o; \
	for cfg in \
	    "cast_align_strict:" \
	    "cast_align_strict_hashes:-DDSHMAP_SWISS_STORE_HASHES=1"; \
	do \
	    name=$${cfg%%:*}; \
	    defs=$${cfg#*:}; \
	    printf 'warning %-20s ' "$$name"; \
	    $(CC) $(CFLAGS) $$cast_align_warning $$defs -o /tmp/dshmap-test-$$name tests/test.c; \
	    /tmp/dshmap-test-$$name >/dev/null; \
	    printf 'ok\n'; \
	done

bench: bench/bench.c dshmap.h
	$(CC) $(CFLAGS) -o bench/bench bench/bench.c
	./bench/bench $(BENCH_ARGS)

coverage: tests/test.c dshmap.h
	$(CC) -O0 -Wall -Wextra -Werror -std=c99 --coverage -o tests/test-cov tests/test.c
	./tests/test-cov
	gcov -r tests/test-cov-test.gcno
	@echo "Coverage report: dshmap.h.gcov"

clean:
	rm -f tests/test tests/test-cxx tests/test-asan tests/test-small-threshold tests/test-small-threshold-asan tests/test-oom tests/test-oom-asan tests/test-cov bench/bench
	rm -f tests/*.gcno tests/*.gcda *.gcov
