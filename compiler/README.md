# Compiler

The Cleave compiler bootstrap, written in C11.

Current state: the **lexer** is wired up. `cleavec --tokens <file.cv>` reads a source file and prints its token stream. No parser, AST, or codegen yet.

## Build

```bash
make             # produces build/cleavec
make run         # builds, runs --version
make clean
```

The Makefile uses standard `cc`. Tested with GCC and Clang. No external dependencies.

## Try it

```bash
./build/cleavec --tokens ../examples/minimal-chain.cv
```

Output is one token per line: `<line>:<col>  <kind>  '<lexeme>'`. EOF is the last entry.

## Layout

```
compiler/
  include/        Public headers
    cleave.h      Version constant
    lexer.h       Token kinds, Token struct, Lexer struct, public API
  src/            Implementation
    main.c        CLI entry point (--version, --help, --tokens)
    lexer.c       Tokenizer
  build/          Generated; gitignored
```

When the parser, AST, and codegen land, they will fit alongside `lexer.c` in `src/` and their headers in `include/`. No premature abstraction.

## Roadmap (this directory)

- **v0.1**: Lexer + parser for chain manifests and module declarations. `cleavec --ast <file.cv>` prints the parsed AST.
- **v0.2**: Type checker. Worked end-to-end example.
- **v0.3**: Codegen to WASM. `cleavec <file.cv>` produces a `.wasm` module.
- **v0.4**: Self-hosting: the compiler compiles itself.

Roadmap above is intent, not promise. Each step is bigger than the last.
