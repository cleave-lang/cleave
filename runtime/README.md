# Cleave runtime

Wasmtime-backed execution engine for Cleave-compiled WASM modules. First piece of Cleave that lives outside the C compiler.

## Architecture

```
+-------------+      +---------------+      +-----------------+
|  .cv source | ---> |    cleavec    | ---> |    .wasm bytes  |
+-------------+      | (C compiler)  |      +-----------------+
                     +---------------+              |
                                                    v
                                             +------+------+
                                             | cleave-     |
                                             | runtime     |
                                             | (Wasmtime)  |
                                             +-------------+
```

The runtime:

- Configures Wasmtime with a determinism-friendly feature set (no SIMD, no relaxed-SIMD).
- Links the four `env`-namespace hostcalls documented in [`spec/abi/wasm.md`](../spec/abi/wasm.md): `state_get`, `state_set`, `gas_consume`, `event_emit`.
- Backs hostcalls with per-instance state owned by Wasmtime's `Store`.
- Exposes a small Rust API (`Runtime`, `Instance`) and a thin CLI binary (`cleave-run`).

## Why Wasmtime

Mature, fast AOT, used in production by Fastly, Cosmonic, others. Cleave's runtime layer is small (~250 lines today); swapping engines later if needed is straightforward, but Wasmtime is the right starting bet.

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
BENCH name=load_counter_module iters=1472 ops_per_sec=2902
BENCH name=call_increment_hot iters=3959680 ops_per_sec=7919280
BENCH name=call_read_hot iters=4650944 ops_per_sec=9301840
```

`call_increment_hot` is the hot-path execution number: Wasmtime dispatching one exported function that does a state read, an add, and a state write. **~7.9 million ops/sec** on this hardware. Well above the 10k TPS minimum target documented in the repo's CLAUDE.md.

These numbers reflect raw VM dispatch only. Real chain throughput will be bound by consensus latency, gas metering granularity, and storage commit cost, none of which exist yet. The point is that the runtime layer is not the bottleneck.

## What this runtime does not yet do

- Persistence across processes (state is in-memory only)
- Real gas budgets (`gas_consume` records usage but never aborts)
- Event payloads serialized from module memory beyond raw byte copy
- Multiple modules sharing state
- Cross-module calls
- Integration with a consensus layer

Each of these is its own future issue. The current runtime is the minimum that makes a Cleave-compiled module actually run.
