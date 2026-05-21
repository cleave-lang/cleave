CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Wpedantic -O2 -Iinclude
LDFLAGS :=

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/cleavec

.PHONY: all clean test run

all: $(BIN)

$(BIN): $(OBJ) | build
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	@mkdir -p build

run: $(BIN)
	@$(BIN) --version

test:
	@echo "no tests yet"

clean:
	rm -rf build
