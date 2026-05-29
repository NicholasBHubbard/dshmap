CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Werror -std=c99
ASANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: test test-asan bench coverage clean

test: tests/test.c swtab.h
	$(CC) $(CFLAGS) -o tests/test tests/test.c
	./tests/test

test-asan: tests/test.c swtab.h
	$(CC) $(CFLAGS) $(ASANFLAGS) -o tests/test-asan tests/test.c
	./tests/test-asan

bench: bench/bench.c swtab.h
	$(CC) $(CFLAGS) -o bench/bench bench/bench.c
	./bench/bench

coverage: tests/test.c swtab.h
	$(CC) -O0 -Wall -Wextra -Werror -std=c99 --coverage -o tests/test-cov tests/test.c
	./tests/test-cov
	gcov -r tests/test-cov-test.gcno
	@echo "Coverage report: swtab.h.gcov"

clean:
	rm -f tests/test tests/test-asan tests/test-cov bench/bench
	rm -f tests/*.gcno tests/*.gcda *.gcov
