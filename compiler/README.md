# Compiler

The Cleave compiler bootstrap, written in C11.

This is `v0.0.1`. The binary builds, prints its version, and exits. There is no lexer, parser, or codegen yet. The directory layout is in place so the pieces can land incrementally without the build going stale.

## Build

```bash
make            # produces build/cleavec
make run        # builds, runs --version
make clean
```

The Makefile uses standard `cc`. Tested with GCC and Clang. No external dependencies.

## Layout

```
compiler/
  include/        Public headers
    cleave.h      Version constant, will grow into the language entry points
  src/            Implementation
    main.c        CLI entry point
  build/          Generated; gitignored
```

When the lexer, parser, AST, and codegen land, they will fit alongside `main.c` in `src/` and their headers in `include/`. We will resist the urge to over-organize before there is real code to organize around.

## Roadmap (this directory)

- **v0.1** — Lexer for chain manifests and module declarations. A `cleavec --parse <file.cv>` that prints a token stream.
- **v0.2** — Parser into AST. `cleavec --ast <file.cv>` that prints the parsed tree.
- **v0.3** — Codegen to WASM. `cleavec <file.cv>` that produces a `.wasm` module.
- **v0.4** — Self-hosting: the compiler compiles itself.

Roadmap above is intent, not promise. Each step is bigger than the last.
