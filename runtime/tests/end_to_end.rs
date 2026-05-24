//! End-to-end integration test: compile a `.cv` source file with the
//! real `cleavec` binary, load the resulting WASM, and call functions.
//!
//! The test discovers `cleavec` relative to the workspace layout:
//! `runtime/` lives next to `compiler/`, so `../compiler/build/cleavec`
//! is the canonical location after `make` in the compiler directory.
//! If the binary is not present (CI hasn't built it yet, a fresh
//! checkout, etc.) the test prints a clear skip notice instead of
//! failing; this lets `cargo test` from inside `runtime/` work without
//! requiring the C toolchain to be set up first.

use std::path::PathBuf;
use std::process::Command;

use cleave_runtime::Runtime;

fn workspace_root() -> PathBuf {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    // runtime/ -> cleave/
    manifest_dir
        .parent()
        .expect("runtime is nested inside the repo root")
        .to_path_buf()
}

fn cleavec_path() -> PathBuf {
    workspace_root().join("compiler/build/cleavec")
}

fn counter_source() -> PathBuf {
    workspace_root().join("examples/counter-mvp.cv")
}

fn ensure_cleavec_or_skip(test_name: &str) -> Option<PathBuf> {
    let path = cleavec_path();
    if !path.exists() {
        eprintln!(
            "{test_name}: skipping; cleavec binary not found at {}",
            path.display()
        );
        eprintln!("(run `make -C compiler` from the repo root to build it)");
        return None;
    }
    Some(path)
}

fn compile_to_wasm(cleavec: &PathBuf, source: &PathBuf, out: &PathBuf) {
    let status = Command::new(cleavec)
        .arg("--emit-wasm")
        .arg(source)
        .arg("-o")
        .arg(out)
        .status()
        .expect("running cleavec");
    assert!(status.success(), "cleavec failed for {}", source.display());
}

#[test]
fn counter_mvp_full_pipeline() {
    let Some(cleavec) = ensure_cleavec_or_skip("counter_mvp_full_pipeline") else {
        return;
    };
    let tmp = std::env::temp_dir().join("cleave-runtime-counter.wasm");
    compile_to_wasm(&cleavec, &counter_source(), &tmp);

    let wasm = std::fs::read(&tmp).expect("read compiled wasm");
    let rt = Runtime::new();
    let mut instance = rt.load(&wasm).expect("load compiled module");

    for expected in 1..=10i64 {
        let v = instance.call("increment", &[]).unwrap();
        assert_eq!(v, expected, "increment iteration {expected}");
    }
    assert_eq!(instance.call("read", &[]).unwrap(), 10);
    assert_eq!(instance.state(0), 10);
}

#[test]
fn counter_mvp_is_deterministic_across_processes() {
    // Compile twice; the bytes must be identical.
    let Some(cleavec) = ensure_cleavec_or_skip(
        "counter_mvp_is_deterministic_across_processes",
    ) else {
        return;
    };
    let a = std::env::temp_dir().join("cleave-runtime-counter-a.wasm");
    let b = std::env::temp_dir().join("cleave-runtime-counter-b.wasm");
    compile_to_wasm(&cleavec, &counter_source(), &a);
    compile_to_wasm(&cleavec, &counter_source(), &b);
    let bytes_a = std::fs::read(&a).unwrap();
    let bytes_b = std::fs::read(&b).unwrap();
    assert_eq!(
        bytes_a, bytes_b,
        "two compilations of identical source produced different bytes"
    );

    // Run a sequence of calls on each; the outputs must match.
    let rt = Runtime::new();
    let run_sequence = |bytes: &[u8]| -> Vec<i64> {
        let mut inst = rt.load(bytes).unwrap();
        (0..8).map(|_| inst.call("increment", &[]).unwrap()).collect()
    };
    assert_eq!(run_sequence(&bytes_a), run_sequence(&bytes_b));
}
