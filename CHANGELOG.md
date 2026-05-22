# Changelog

All notable changes to Cleave will be documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project uses [Semantic Versioning](https://semver.org/).

Prose references a version as `v0.X.Y`; headings stay bare `[0.X.Y]`.

## [Unreleased]

## [0.1.0] - 2026-05-22

First tagged release. The compiler can lex and parse a chain manifest into an AST. No codegen or runtime yet; those land in subsequent milestones.

### Added

- Repository scaffolding: README, LICENSE (Apache-2.0), DESIGN.md, CONTRIBUTING.md, CODE_OF_CONDUCT.md, SECURITY.md, issue and PR templates, CHANGELOG.
- Brand assets in `.github/`: mark + wordmark lockup, light and dark variants.
- Example `.cv` files in `examples/`: minimal chain manifest, multi-VM chain, counter module, token module, custom consensus protocol.
- Compiler bootstrap in C11 (`compiler/`): Makefile, `cleavec` binary with `--version`, `--help`, `--tokens <file.cv>`, and `--ast <file.cv>` commands.
- Lexer covering keywords, identifiers, integer literals (decimal, hex, binary, with `_` separators), string and character literals, single and multi-character punctuation, `//` line and `/* */` block comments, source line and column tracking, error recovery on unknown characters. Lexes all `examples/*.cv` with zero ERROR tokens.
- AST node definitions (`compiler/include/ast.h`): tagged-union `AstNode` covering 28 kinds across declarations, types, expressions, and statements. Source spans on every node. Memory is malloc-and-leak by design for v0; documented in `compiler/src/ast.c`.
- `ast_dump` pretty-printer producing readable indented output.
- Recursive-descent parser (`compiler/src/parser.c`) for the `chain { }` block: subsystem assignments, named and generic types, keyword and positional type parameters, and anonymous set literals like `{cpu, storage, witness}`. Error reporting with source line and column on missing tokens.
- Hand-rolled testing harness (`compiler/tests/test.h`, ~60 lines). Three test binaries: `lexer_test.c` (224 assertions), `ast_test.c` (58 assertions), `parser_test.c` (47 assertions). 329 total assertions across 60 tests.
- Microbenchmark harness (`compiler/bench/bench.h`). Lexer and parser benches with baseline numbers documented in `compiler/bench/README.md`.
- CI workflow building the compiler, running the test suite, and running benchmarks on every push and PR. Bench results visible in workflow logs.
- Branch protection ruleset on `main`: required PR review, required CI check, linear history, signed commits, no force push, no deletion. OrganizationAdmin bypass for emergency direct pushes.
- GitHub Discussions enabled. Milestones (v0.1, v0.2, v0.3, v0.4). Labels (`compiler`, `stdlib`, `tooling`, `design`). 14 tracking issues across milestones.

### Notes

- The compiler does not yet produce executable output. `cleavec --tokens` prints token streams; `cleavec --ast` prints parsed AST. Codegen lands in v0.3.
- Standard library protocol designs (consensus, gas, state, effects, DA) are open RFCs in the v0.2 milestone.
- `.cv` is the canonical file extension. Switched from `.cleave` during early bootstrap; no public history of the older extension.
