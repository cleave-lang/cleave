//! Microbenchmark for the Cleave runtime.
//!
//! Reports `BENCH name=<x> iters=<n> ops_per_sec=<n>` lines on stdout
//! so CI logs match the format the C-side benches use. Throughput
//! numbers here are the first real measurement against the project's
//! 10k TPS minimum target documented in CLAUDE.md.
//!
//! What we measure:
//!   - module load (cold): how long it takes to instantiate from bytes
//!   - increment_call: tight loop calling `increment` on a hot instance
//!   - read_call: tight loop calling `read` on a hot instance
//!
//! Each row is timed against a duration window (default 500 ms) and we
//! divide iteration count by elapsed seconds. This matches the C-side
//! bench.h harness, not criterion's statistical reports; the absolute
//! numbers are not stable across machines but they catch regressions.

use std::time::{Duration, Instant};

use cleave_runtime::Runtime;

/// Counter MVP, byte-identical to the snapshot embedded in lib.rs tests.
/// Regenerate with `cleavec --emit-wasm examples/counter-mvp.cv`.
const COUNTER_WASM: &[u8] = &[
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x0f, 0x03, 0x60, 0x01, 0x7f, 0x01, 0x7e,
    0x60, 0x02, 0x7f, 0x7e, 0x00, 0x60, 0x00, 0x01,
    0x7e, 0x02, 0x21, 0x02, 0x03, 0x65, 0x6e, 0x76,
    0x09, 0x73, 0x74, 0x61, 0x74, 0x65, 0x5f, 0x67,
    0x65, 0x74, 0x00, 0x00, 0x03, 0x65, 0x6e, 0x76,
    0x09, 0x73, 0x74, 0x61, 0x74, 0x65, 0x5f, 0x73,
    0x65, 0x74, 0x00, 0x01, 0x03, 0x03, 0x02, 0x02,
    0x02, 0x07, 0x14, 0x02, 0x09, 0x69, 0x6e, 0x63,
    0x72, 0x65, 0x6d, 0x65, 0x6e, 0x74, 0x00, 0x02,
    0x04, 0x72, 0x65, 0x61, 0x64, 0x00, 0x03, 0x0a,
    0x1a, 0x02, 0x11, 0x00, 0x41, 0x00, 0x41, 0x00,
    0x10, 0x00, 0x42, 0x01, 0x7c, 0x10, 0x01, 0x41,
    0x00, 0x10, 0x00, 0x0b, 0x06, 0x00, 0x41, 0x00,
    0x10, 0x00, 0x0b,
];

fn bench<F: FnMut()>(name: &str, window: Duration, mut body: F) {
    // Warm up briefly so the first iteration doesn't dominate timing
    // for short-running ops.
    let warm_start = Instant::now();
    while warm_start.elapsed() < Duration::from_millis(50) {
        body();
    }

    let start = Instant::now();
    let mut iters: u64 = 0;
    while start.elapsed() < window {
        // Run a small batch between clock reads to amortize the
        // `Instant::now()` cost.
        for _ in 0..64 {
            body();
        }
        iters += 64;
    }
    let elapsed = start.elapsed();
    let secs = elapsed.as_secs_f64();
    let ops_per_sec = (iters as f64 / secs) as u64;
    println!("BENCH name={name} iters={iters} ops_per_sec={ops_per_sec}");
}

fn main() {
    let window = Duration::from_millis(500);

    bench("load_counter_module", window, || {
        let rt = Runtime::new();
        let _ = rt.load(COUNTER_WASM).expect("counter loads");
    });

    {
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        bench("call_increment_hot", window, || {
            let _ = instance.call("increment", &[]).unwrap();
        });
    }

    {
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        // Warm with one write so read isn't always returning zero.
        let _ = instance.call("increment", &[]).unwrap();
        bench("call_read_hot", window, || {
            let _ = instance.call("read", &[]).unwrap();
        });
    }
}
