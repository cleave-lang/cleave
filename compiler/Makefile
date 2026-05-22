CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Wpedantic -O2 -Iinclude
LDFLAGS :=

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=build/%.o)
BIN     := build/cleavec

# Library objects = everything except main.o (which contains main()).
# Test and bench binaries link against these so they can supply their own main().
LIB_OBJ := $(filter-out build/main.o,$(OBJ))

TEST_SRC  := $(wildcard tests/*_test.c)
TEST_BIN  := $(TEST_SRC:tests/%.c=build/tests/%)

BENCH_SRC := $(wildcard bench/*_bench.c)
BENCH_BIN := $(BENCH_SRC:bench/%.c=build/bench/%)

.PHONY: all clean test run bench

all: $(BIN)

$(BIN): $(OBJ) | build
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/tests/%: tests/%.c $(LIB_OBJ) | build/tests
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LIB_OBJ) $(LDFLAGS)

build/bench/%: bench/%.c $(LIB_OBJ) | build/bench
	$(CC) $(CFLAGS) -Ibench -o $@ $< $(LIB_OBJ) $(LDFLAGS)

build:
	@mkdir -p build

build/tests:
	@mkdir -p build/tests

build/bench:
	@mkdir -p build/bench

run: $(BIN)
	@$(BIN) --version

test: $(TEST_BIN)
	@pass=0; fail=0; \
	for t in $(TEST_BIN); do \
		echo "==> $$t"; \
		if $$t; then pass=$$((pass+1)); else fail=$$((fail+1)); fi; \
	done; \
	echo; echo "test binaries: $$pass passed, $$fail failed"; \
	[ $$fail -eq 0 ]

bench: $(BENCH_BIN)
	@for b in $(BENCH_BIN); do \
		echo "==> $$b"; \
		$$b; \
	done

clean:
	rm -rf build
