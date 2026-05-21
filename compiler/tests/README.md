# Compiler tests

Hand-rolled test framework, no external dependencies. The whole framework is `tests/test.h` (~60 lines).

## Run

```bash
make -C compiler test        # build + run everything
```

CI runs the same target on every push and PR.

## Add a test

1. Create `compiler/tests/<thing>_test.c`. The filename must end in `_test.c` for the Makefile to pick it up.
2. Include the framework and whatever you're testing:

   ```c
   #include "test.h"
   #include "lexer.h"   /* or whatever module */
   ```

3. Write test functions:

   ```c
   TEST(test_some_behavior) {
       Lexer lex;
       lexer_init(&lex, "input");
       Token t = lexer_next(&lex);
       ASSERT_EQ_INT(t.kind, TK_KW_CHAIN);
       ASSERT_EQ_STR_LEN(t.start, t.length, "chain");
   }
   ```

4. Call them from `main()` and end with `REPORT()`:

   ```c
   int main(void) {
       RUN(test_some_behavior);
       /* ... more RUN(...) ... */
       REPORT();
   }
   ```

The `_test.c` file produces a standalone binary (linked against the compiler library objects, minus `main.o`). `make test` runs every test binary and exits non-zero if any binary returns non-zero.

## Assertion macros

| Macro | Use |
|---|---|
| `ASSERT(cond)` | Generic boolean. Reports the failing expression text. |
| `ASSERT_EQ_INT(actual, expected)` | Integer equality. Reports both values on failure. |
| `ASSERT_EQ_STR_LEN(actual, len, expected)` | Compare a `(ptr, len)` slice against a C string. Useful for `Token.start` + `Token.length`. |
| `FAIL_AT(fmt, ...)` | Bail without an assertion. Always provide at least one varadic arg (`-Wpedantic` requires it). |

All assertions report file, line, test function, and a useful message on failure.

## What we test

| File | Covers |
|---|---|
| `lexer_test.c` | Every keyword and punctuation token, identifiers, comments, whitespace, line/column tracking, error recovery, a smoke test of a small chain manifest |

When parser, AST, and codegen land, they get their own `*_test.c` files alongside.

## Constraints

- Test binaries are built with the same flags as the compiler (`-std=c11 -Wall -Wextra -Werror -Wpedantic -O2`). New tests have to compile under those.
- No global state across test functions. Set up fresh per test.
- No filesystem access in tests if avoidable. Use string literals as fake source inputs.
- Resist adding a test framework dependency. The whole thing is 60 lines for a reason.
