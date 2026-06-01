CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Werror -std=c99
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: test test-asan bench coverage clean

test: tests/test.c tests/test_oom.c bench/bench.c swtab.h
	$(CC) $(CFLAGS) -o tests/test tests/test.c
	./tests/test
	$(CC) $(CFLAGS) -o tests/test-oom tests/test_oom.c
	./tests/test-oom
	$(CC) $(CFLAGS) -o bench/bench bench/bench.c
	./bench/bench --compare --sizes 1,2 --ops find_hit,mixed --min-ops 1 --no-perf >/dev/null
	./bench/bench --csv --linear 1:3 --ops find_miss --min-ops 1 --no-perf >/dev/null

test-asan: tests/test.c tests/test_oom.c swtab.h
	$(CC) $(CFLAGS) $(ASANFLAGS) -o tests/test-asan tests/test.c
	./tests/test-asan
	$(CC) $(CFLAGS) $(ASANFLAGS) -o tests/test-oom-asan tests/test_oom.c
	./tests/test-oom-asan

bench: bench/bench.c swtab.h
	$(CC) $(CFLAGS) -o bench/bench bench/bench.c
	./bench/bench $(BENCH_ARGS)

coverage: tests/test.c swtab.h
	$(CC) -O0 -Wall -Wextra -Werror -std=c99 --coverage -o tests/test-cov tests/test.c
	./tests/test-cov
	gcov -r tests/test-cov-test.gcno
	@echo "Coverage report: swtab.h.gcov"

clean:
	rm -f tests/test tests/test-asan tests/test-oom tests/test-oom-asan tests/test-cov bench/bench
	rm -f tests/*.gcno tests/*.gcda *.gcov
