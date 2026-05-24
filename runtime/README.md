# Cleave runtime

Wasmtime-backed execution engine for Cleave-compiled WASM modules. First piece of Cleave that lives outside the C compiler.

## Architecture

```
+-------------+      +---------------+      +-----------------+
|  .cv source | ---> |    cleavec    | ---> |    .wasm bytes  |
+-------------+      | (C compiler)  |      +-----------------+
                     +---------------+              |
                                                    v
+-----------+                                +------+------+
| Solidity  | ---> compile ---> .evm bytes  |  cleave-    |
| / hex     |                               |  runtime    |
+-----------+                               | (Wasmtime + |
                                            |    REVM)    |
                                            +-------------+
```

The runtime hosts two execution engines side by side:

- **Wasmtime** for Cleave-compiled WASM modules. Configured with a determinism-friendly feature set (no SIMD, no relaxed-SIMD). Links the four `env`-namespace hostcalls documented in [`spec/abi/wasm.md`](../spec/abi/wasm.md): `state_get`, `state_set`, `gas_consume`, `event_emit`. State is owned by Wasmtime's `Store`.
- **REVM** for Solidity / EVM bytecode. Configured for the Cancun hardfork over an in-memory `CacheDB`. Lets a chain manifest declare `exec: EVM<runtime=REVM>` and execute Solidity contracts on the same runtime layer. v0.3 issue #19 lands the engine; cross-VM state sharing with the WASM engine is a separate future issue.

Both engines expose a small Rust API and share the `cleave-run` CLI.

## Why these two engines

Wasmtime: mature, fast AOT, used in production by Fastly, Cosmonic. REVM: the Rust EVM used by Reth, fastest production EVM implementation. Cleave's runtime layer stays a thin wrapper over each; swapping either later is straightforward, but these are the right starting bets.

## Layout

```
runtime/
  Cargo.toml
  src/
    lib.rs              # Runtime, Instance, hostcall linker
    main.rs             # cleave-run CLI
  tests/
    end_to_end.rs       # compile + load + execute against the real cleavec
  benches/
    runtime_bench.rs    # tight-loop microbench (BENCH lines on stdout)
  README.md
```

## Building

```
cargo build --release
```

Wasmtime + Cranelift pull in a fair amount of code (≈170 crates on first build). Subsequent builds reuse the cached registry.

## Running

WASM (Cleave-compiled):

```
# Compile a Cleave source file first
make -C ../compiler
../compiler/build/cleavec --emit-wasm ../examples/counter-mvp.cv -o /tmp/counter.wasm

# Single call
cargo run --release --bin cleave-run -- /tmp/counter.wasm increment
# prints: 1

# Multiple calls sharing state
cargo run --release --bin cleave-run -- /tmp/counter.wasm --calls increment increment increment read
# prints:
#   increment = 1
#   increment = 2
#   increment = 3
#   read = 3
```

EVM (Solidity / hand-crafted bytecode):

```
# Hand-crafted counter contract: increments slot 0, returns the new value
cargo run --release --bin cleave-run -- --evm 0x60005460010180600055600052602060 00f3 --calls 3
# prints:
#   call 1 = 0x0000000000000000000000000000000000000000000000000000000000000001
#   call 2 = 0x0000000000000000000000000000000000000000000000000000000000000002
#   call 3 = 0x0000000000000000000000000000000000000000000000000000000000000003
#   storage[0] = 3
```

## Testing

```
cargo test
```

Five unit tests use a precompiled WASM snapshot embedded in `lib.rs` (no C toolchain needed). Two integration tests in `tests/end_to_end.rs` shell out to `../compiler/build/cleavec` to compile `examples/counter-mvp.cv` and exercise the full pipeline. The integration tests print a clear skip notice if `cleavec` is not built yet rather than failing.

## Benchmarks

```
cargo run --release --bin cleave-runtime-bench
```

Output (Apple M3 Pro, indicative):

```
BENCH name=load_counter_module     iters=2368    ops_per_sec=4667
BENCH name=call_increment_hot      iters=5308928 ops_per_sec=10617849
BENCH name=call_read_hot           iters=5097280 ops_per_sec=10194480
BENCH name=evm_load_counter        iters=295040  ops_per_sec=589991
BENCH name=evm_call_increment_hot  iters=591744  ops_per_sec=1183398
```

`call_increment_hot` is the WASM hot-path number: Wasmtime dispatching one exported function that does a state read, an add, and a state write. **~10.6 million ops/sec.** `evm_call_increment_hot` is the same semantic operation on the EVM engine: **~1.18 million ops/sec**, ~9x slower than WASM but still ~100x above the 10k TPS minimum target.

Module load is asymmetric because Wasmtime AOT-compiles on `Runtime::load` (slow once, fast forever), whereas REVM keeps an interpreter so install is cheap but per-instruction dispatch is more expensive.

These numbers reflect raw VM dispatch only. Real chain throughput will be bound by consensus latency, gas metering granularity, and storage commit cost, none of which exist yet. The point is that the runtime layer is not the bottleneck.

## What this runtime does not yet do

- Persistence across processes (state is in-memory only)
- Real gas budgets (`gas_consume` records usage but never aborts; EVM gas is metered by REVM but no budget enforcement at the cross-engine level)
- Event payloads serialized from module memory beyond raw byte copy
- Multiple modules sharing state
- Cross-module calls
- **Cross-engine state sharing**: WASM modules and EVM contracts each have their own state today. A Cleave module calling a Solidity contract on the same chain (or vice versa) requires a shared state backend; that's its own future issue.
- ERC-20 / standard Solidity contract deployment via `solc` toolchain (the EVM engine accepts raw bytecode today; the toolchain layer is the next step)
- JSON-RPC layer for `eth_sendRawTransaction` and friends
- Integration with a consensus layer

Each of these is its own future issue. The current runtime is the minimum that makes a Cleave-compiled module AND a Solidity-compiled EVM module both actually run.
