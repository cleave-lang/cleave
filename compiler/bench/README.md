# Compiler benchmarks

Microbenchmarks for the lexer and parser. Numbers here exist so we can tell when something gets slower; the throughput targets that actually matter live in `runtime/` and the chain-execution layer (post-v0.3).

## Run

```bash
make -C compiler bench
```

Output format (one line per bench, machine-parseable):

```
BENCH name=<name> iters=<N> ops_per_sec=<N>
```

CI prints the same lines to the Actions log. No gate yet; this is measurement infrastructure, not a regression alarm.

## Adding a benchmark

1. Create `compiler/bench/<thing>_bench.c`. Filename must end in `_bench.c` for the Makefile to pick it up.
2. Include the harness and whatever you measure: `#include "bench.h"`, `#include "lexer.h"`, etc.
3. Call `BENCH("name", { ... statements ... })` from `main()`. Each statement block should be self-contained (re-create state inside).
4. Return 0 from main.

The harness runs 100 warmup iterations, then iterates in batches of 100 until at least 0.5 seconds of wall time has accumulated, and reports `iters / seconds`.

## Baseline (as of v0.1, 2026-05-22)

Hardware: MacBook Pro, Apple Silicon. Single-threaded. `cc -std=c11 -O2`.

| Bench | iters | ops/sec | notes |
|---|---|---|---|
| `lex_minimal_chain` | 752,000 | 1,503,868 | full `examples/minimal-chain.cv`, lex to EOF |
| `lex_keyword_soup` | 986,900 | 1,973,646 | 100-byte string of keywords + punctuation |
| `lex_synthetic_1k_chains` | 900 | 1,634 | 1000 repeated chain blocks (~200 KB source), lex to EOF |
| `parse_empty_chain` | 6,227,100 | 12,454,150 | `chain Empty { }`, baseline parser overhead |
| `parse_minimal_chain` | 381,300 | 762,586 | full minimal manifest, 4 subsystems, type args + set literal |
| `parse_subsystem_heavy` | 242,600 | 485,184 | 5 subsystems with multi-arg generics each |

Rough takeaway: the lexer sustains ~325 MB/s on the synthetic input, and parsing a 5-line chain manifest takes ~1.3 microseconds. These numbers are NOT what matters for the language's stated throughput goals (chain execution at 10k+ TPS); they're a sanity check that the compiler dev-loop is not the bottleneck.

## When to update this file

Whenever a PR meaningfully changes any of the bench numbers, re-run locally and update the table above in the same PR. We want the table to be approximately current, not historically accurate.

## Future benches

- Codegen throughput (when codegen lands; issue #17)
- End-to-end compile time (`.cv` source -> WASM module)
- Per-VM execution micro-benchmarks (issue #18 and #19)
