# Changelog

All notable changes to Cleave will be documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project uses [Semantic Versioning](https://semver.org/).

Prose references a version as `v0.X.Y`; headings stay bare `[0.X.Y]`.

## [Unreleased]

### Added

- Solidity toolchain integration. Real `solc`-compiled Solidity contracts now run end-to-end on the EVM engine. `runtime/tests/solidity.rs` shells out to `solc --bin --optimize` to compile `runtime/tests/fixtures/MiniERC20.sol`, deploys via `Evm::deploy` with a 32-byte ABI-encoded constructor arg (`initial supply`), then exercises `balanceOf` and `transfer` via hand-rolled ABI encoding of the standard selectors (`0x70a08231 balanceOf(address)`, `0xa9059cbb transfer(address,uint256)`, `0x18160ddd totalSupply()`). Closes #52.
- 2 new integration tests:
  - `erc20_deploys_and_transfers`: deploys MiniERC20 with 1M supply to alice, asserts `totalSupply == 1M`, asserts `balanceOf(alice) == 1M` and `balanceOf(bob) == 0`, transfers 100 from alice to bob, asserts both balances post-transfer
  - `erc20_transfer_reverts_when_insufficient_balance`: bob (zero balance) attempting to transfer 1 reverts with the expected revert message
- Both tests skip cleanly if `solc` is not on PATH (same pattern as the `cleavec`-dependent end-to-end test).
- CI installs `solc` in the runtime job so the Solidity tests exercise the real toolchain on every PR.
- Runtime README updated: full Solidity workflow documented; the "ERC-20 / standard Solidity contract deployment via solc toolchain" caveat removed from the "what this runtime does not yet do" list.
- Real gas budget enforcement on the WASM engine (`runtime/src/lib.rs`). The v0.3 runtime tracked `gas_used` per dimension but never aborted on overflow; that caveat is gone. Two layers of metering now compose:
  - **Per-dimension gas budgets**: `Instance::set_gas_budget(dim, units)` sets a budget; `env.gas_consume(dim, amount)` traps the current call when `gas_used + amount > budget`. Dimensions without a budget set are unmetered. Lets chains express the multi-dimensional gas model from RFC #7 (cpu / storage / witness / etc.).
  - **Wasmtime fuel**: enabled in `Config::consume_fuel(true)` so every WASM instruction consumes fuel. Catches infinite loops in pure WASM that never call into a hostcall. `Instance::set_fuel(units)` sets the budget; `Instance::fuel_remaining()` reports how much is left. Default is 10 billion units (`DEFAULT_FUEL`), generous enough that existing callers see no behavior change.
- New `gas_budgets: HashMap<u32, u64>` on `HostState` and the new `Instance` setters / readers (`set_gas_budget`, `set_fuel`, `fuel_remaining`).
- 7 new unit tests: unmetered call, under-budget call, over-budget traps with a clear "out of gas" message, orthogonal-dimension budget ignored, default fuel sufficient for 100 increment calls, fuel actually decreases across a call, zero-fuel traps. Plus a hand-crafted `GAS_TEST_WASM` constant (73 bytes) that exercises `env.gas_consume` since the v0.3 codegen does not yet emit gas_consume calls.
- Closes #51.
- Codegen for `match` statements (`compiler/src/codegen.c`). The v0.3 codegen rejected `match`; this lifts the restriction. v0 lowering: save scrutinee to a shared scratch local, then chain `i64.eq` comparisons inside nested `if 0x40 ... else ... end`. Supported patterns: integer literal, bool literal, identifier (treated as wildcard for v0 since binding patterns need locals + sum types). Wildcard arms terminate the chain as the innermost else branch. Closes #43.
- Match scratch local. The fn-body pre-scan now detects any `match` statement and allocates one shared scratch local for the scrutinee value. Matches are sequential within a fn (the next match runs only after the previous arm body completes), so one scratch slot covers all matches in a fn.
- `count_local_slots` helper replaces `count_let_bindings`. Returns both let-binding count and a match-presence flag, which `emit_fn_body` translates into a single locals declaration group.
- `leaves_value_on_stack` extended to recurse into block result. Lets match arm bodies (which can themselves be blocks) correctly trigger the post-arm drop only when their trailing expression actually leaves a value.
- 5 codegen tests covering: extra scratch local in the locals declaration, scrutinee-save-then-compare byte sequence, wildcard-terminates-chain (2 ifs for 3 arms with `_`), no-wildcard omits final else, wildcard-only match emits no `if` byte.
- `codegen_match_heavy` bench: 6 literal arms + wildcard.
- Codegen for `if` / `else` / `else if` chains (`compiler/src/codegen.c`). The v0.3 codegen rejected `if` statements; this lifts the restriction. Cleave-compiled WASM modules can now do branching. New `emit_if` lowers to WASM's native `if 0x40 ... else ... end` instruction sequence with an empty block type (if-as-expression remains gated on the parser supporting it). `else if` chains nest naturally as `if ... else (if ...)`. Closes #49.
- Cond coercion to i32. WASM's `if` instruction consumes an i32, but Cleave-compiled expressions usually leave i64 on the stack (literals, identifier reads, calls, arithmetic). Codegen now emits `i32.wrap_i64` (0xA7) before the `if` byte when needed. Comparisons (`==`, `!=`, `<`, `>`, `<=`, `>=`) already produce i32 per the WASM spec, so no wrap is emitted in that case; a new `leaves_i32_on_stack` helper distinguishes the two.
- New `leaves_value_on_stack` helper that returns false for assignment expressions. Used in both the if-branch and fn-body code paths so a block whose trailing expression is `x = expr` does not get a spurious `drop` or default-zero. Fixes a latent bug that existed before this PR: a fn whose body ended in an assignment produced invalid WASM. The pre-existing tests did not catch it because every example always had a non-assignment trailing expression.
- 6 codegen tests covering: if without else, if with else, else-if chain, comparison-cond (no wrap), bool-literal cond (wrap), and the assignment-branch-no-drop regression that surfaced during this PR's development.
- `codegen_if_heavy` bench case: 5-way else-if chain.
- New opcodes registered in `wasm.h`: `WASM_OP_BLOCK 0x02`, `WASM_OP_LOOP 0x03`, `WASM_OP_IF 0x04`, `WASM_OP_ELSE 0x05`, `WASM_OP_I32_WRAP_I64 0xA7`. New constant `WASM_BLOCKTYPE_EMPTY 0x40`.
- CI gains four lower-cost gates:
  - **`wasm-validate` on emitted modules.** The compiler job installs wabt, compiles every example marked "yes" in `examples/README.md`, and runs `wasm-validate` on the output. Catches structurally invalid modules that pass our byte-level codegen unit tests.
  - **Determinism check.** The compiler job compiles `examples/counter-mvp.cv` twice and `cmp -s` the outputs; non-zero exit on any difference. Guards against nondeterminism from HashMap iteration order, pointer addresses, build timestamps.
  - **`cargo clippy --all-targets --release -- -D warnings`** as the first runtime step. Runs before the full Wasmtime + Cranelift compile so style and correctness lints fail fast.
  - **MSRV pin** in `runtime/Cargo.toml`: `rust-version = "1.95"`. Locks the floor after the v0.3 bump (revm 40 + ruint 1.18 need rustc >= 1.91).
- One existing clippy lint fixed inline (`runtime/src/main.rs::hex_decode` now uses `s.len().is_multiple_of(2)` instead of `s.len() % 2 != 0`).
- Codegen for `let` bindings (`compiler/src/codegen.c`). The v0.3 codegen rejected `let` with a diagnostic; this lifts the restriction. The fn-body emitter pre-scans the top-level block to count let statements, allocates one WASM local per binding at index `n_params + let_index`, emits a single locals declaration group of N i64s, and lowers each `let name = expr` to `<expr>; local.set <idx>`. Reads of let-bound names go through the existing `local.get` path; assignments to let names route to `local.set`. `find_local` now scans most-recent-first so shadowing matches user expectation. Closes #44.
- 7 codegen tests (`codegen_test.c`): locals declaration byte sequence, `local.set 0` / `local.get 0` opcodes, post-param local index, three-let single-group declaration, let referencing earlier let, reassignment lowering, regression guard that fn with no lets still emits a zero local-groups byte.
- `codegen_let_heavy` bench case: 8 let bindings + arithmetic chain.
- `codegen.c` header doc updated: "let bindings deliberately rejected" moved out, control flow + sum types now reference the issues that track them (#43, #48, #49).

## [0.3.0] - 2026-05-24

The "Multi-VM execution" release. Cleave is end-to-end now: write a `.cv` file, compile to WASM with `cleavec`, run it on the Wasmtime-backed runtime with state persisting across calls. The same runtime also embeds REVM, so Solidity-compiled EVM bytecode runs alongside Cleave-compiled WASM modules. Both engines clear the project's 10 K TPS minimum target by orders of magnitude on hot-path microbenches.

### Added

- EVM execution engine in the runtime crate (`runtime/src/evm.rs`). Embeds REVM 40 alongside the existing Wasmtime engine so a chain manifest can declare `exec: EVM<runtime=REVM>` and execute Solidity-compiled bytecode without changing application code. Configured for the Cancun hardfork over REVM's in-memory `CacheDB`. Public API: `Evm::new`, `Evm::fund`, `Evm::install` (write bytecode directly to an address), `Evm::deploy` (standard CREATE), `Evm::call`, `Evm::storage`. Re-exports `Address`, `Bytes`, `U256`, `StorageKey`, `StorageValue` from REVM so downstream callers do not need a direct REVM dependency. Lands the core of issue #19.
- `cleave-run --evm <bytecode.hex> [--calls N]` CLI mode for executing EVM bytecode. Hex input accepted inline (`0x...`) or as a file path. State persists across `--calls` and prints `storage[0]` at the end.
- 5 EVM unit tests in `runtime/src/evm.rs` covering install + call, multi-call state persistence, determinism, the CREATE deploy path (with a hand-crafted constructor that copies runtime bytecode into memory and RETURNs it), and the EVM precompile-vs-installed-contract distinction.
- Hand-crafted EVM "counter" runtime bytecode embedded in tests and bench: 18 bytes that increment storage slot 0 and return the new value as a 32-byte big-endian word. No Solidity toolchain required.
- EVM bench cases (`evm_load_counter`, `evm_call_increment_hot`) in `cleave-runtime-bench`. On Apple M3 Pro: ~1.18 M EVM ops/sec on the hot path, vs ~10.6 M WASM ops/sec on the same semantic operation. Both engines clear the 10 K TPS minimum target by orders of magnitude.
- Cleave runtime (`runtime/`). First piece of Cleave outside the C compiler. Wasmtime-backed (Rust crate `cleave-runtime`) execution engine that loads `.wasm` binaries produced by `cleavec --emit-wasm` and runs their exported functions. Implements the four `env`-namespace hostcalls documented in `spec/abi/wasm.md`: `state_get`, `state_set`, `gas_consume`, `event_emit`. Wasmtime configured for deterministic execution (no SIMD, no relaxed-SIMD). Backs hostcalls with per-instance in-memory state owned by the Wasmtime `Store`. Lands the core of issue #18.
- `cleave-run` CLI binary: `cleave-run <module.wasm> <fn> [args...]` for single calls or `--calls fn1 fn2 fn3` for sequenced calls sharing state within one invocation.
- Runtime tests: 5 unit tests in `runtime/src/lib.rs` against a precompiled WASM snapshot (no C toolchain needed) and 2 integration tests in `runtime/tests/end_to_end.rs` that shell out to `cleavec` to compile `examples/counter-mvp.cv` and exercise the full pipeline.
- Runtime bench (`cleave-runtime-bench`): tight-loop microbench reports `BENCH name=X iters=N ops_per_sec=N` lines matching the C-side format. On Apple M3 Pro: ~7.9 M increment calls/sec hot-path, ~9.3 M read calls/sec hot-path, ~2.9 K cold module loads/sec. Well above the 10 K TPS minimum target.
- CI gains a `runtime` job that installs Rust, builds `cleavec` (the integration test depends on it), then `cargo build --release && cargo test --release && cargo run --release --bin cleave-runtime-bench` against the runtime crate. Caches `~/.cargo/registry`, `~/.cargo/git`, and `runtime/target` keyed on `runtime/Cargo.toml`.
- WebAssembly code generator (`compiler/src/codegen.c`, `compiler/include/codegen.h`). Compiles a Cleave module into a valid WASM 1.0 binary that `wasm-validate` accepts and Wasmtime can AOT-compile. Lowers integer literals, state reads and writes (via `env.state_get` / `env.state_set` hostcall imports), binary arithmetic and comparison operators on `i64`, function calls between module functions, and block trailing expressions used as function return values. Modules export every `fn` under its source name. Lands the core of issue #17.
- WASM binary emitter primitives (`compiler/src/wasm.c`, `compiler/include/wasm.h`): byte buffer, LEB128 unsigned and signed encoders, section helpers, header / opcode / type-byte constants. Self-contained, no AST dependency. Reusable by future codegens (e.g. a JIT inspector).
- Hostcall ABI documented in `spec/abi/wasm.md`: `env.state_get(slot) -> i64`, `env.state_set(slot, value)`, `env.gas_consume(dimension, amount)`, `env.event_emit(id, ptr, len)`. Slot allocation, type widths, linear-memory expectations, and the v0 scope are all spelled out so the runtime (#18) and codegen agree on the contract.
- `cleavec --emit-wasm <file.cv> [-o <out.wasm>]` runs lex + parse + typecheck + codegen and writes the binary to the given path (or stdout if `-o` is omitted). The output is byte-identical across runs and validates clean with wabt's `wasm-validate`.
- `examples/counter-mvp.cv`: the canonical v0.3 codegen target. Identical in spirit to the aspirational `counter.cv` but drops the `Result<u64>` wrapper since sum types are not yet in the type system or codegen.
- `codegen_test.c` (32 assertions): LEB128 encoding correctness, WASM header bytes, import-name byte sequences, export-name encoding, opcode emission for integer literals / `i64.add` / `state_get` / `state_set` / function calls, and a negative case rejecting chain-only programs.
- `codegen_bench.c` reports throughput for codegen on counter and arithmetic-heavy modules.
- Type checker (`compiler/src/typecheck.c`, `compiler/include/typecheck.h`) covering the minimum surface needed to unblock codegen: primitive integer types and bool, str, char, unit, function types, opaque generic-head types like `Result<u64>`. Validates `let` bindings (annotated and inferred), `state` declarations, assignments, binary operator operand types, comparison and logical operator shapes, function return-type compatibility with both trailing expressions and explicit `return` statements, call-site arity, and call-site argument types. Continues past the first error so multiple issues surface in one run. Lands issue #34.
- `cleavec --check <file.cv>` runs lex + parse + typecheck and exits 0 on success, non-zero with diagnostics on failure. `--ast` and `--tokens` unchanged.
- `AstNode` carries a `resolved_type` slot populated by the type checker for every expression node. NULL on non-expression nodes and on expressions whose type the checker could not resolve.
- `Type` interning for primitives and unit; equality of two primitive `Type*` is pointer equality.
- Hand-rolled `typecheck_test.c` (34 assertions: positive cases for counter module, let inference, comparisons, logical ops, chain manifests; negative cases for let / assignment / return / logical-op / if-condition / comparison / call-arity / call-arg mismatches; multi-error continuation).
- `typecheck_bench.c` reports type-checker throughput on counter, arithmetic-heavy, and protocol-shaped source.
- Parser coverage extended from chain manifests only to the full grammar surface that ships in `tree-sitter-cleave`: module declarations, protocol declarations with `implements` and `slash_on`, function declarations with optional `pure`/`view` modifier and `with [effects]` clause, event and effect declarations (including trailing `deferred`), state and gas declarations, record literals `{ key: value, ... }`, full expression grammar with precedence climbing (binary, unary, call, index, member, path, percent, parenthesized), block expressions with optional trailing expression, statements (`let` with optional type annotation, `return`, `if`/`else` chains, `match` with arms, expression statements). Lands the productions issue #35 tracks.
- `parser_parse_program` as the new top-level entry; `parser_parse_expression` exposed for tests. `cleavec --ast` now accepts files with `chain`, `module`, and `protocol` declarations at the top level.
- New keywords recognized by the lexer: `pure`, `view`, `with`, `deferred`, `event`.
- AST node kinds added: `AST_EVENT_DECL`, `AST_EXPR_RECORD`, `AST_RECORD_FIELD`, `AST_STMT_MATCH`, `AST_MATCH_ARM`. `FnDecl` extended with `modifier` and `with_effects`; `EffectDecl` extended with `is_deferred`.
- Parser test suite grew from 47 to 149 assertions covering the new productions.
- Parser bench gains `parse_counter_module` and `parse_protocol_body` cases on realistic source.
- `spec/grammar.ebnf` rewritten to document the full surface; remaining planned items (closures, unit type, struct-literal shorthand, nested subsystem blocks) listed at the bottom for future issues.

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
