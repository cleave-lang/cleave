//! Cleave runtime: Wasmtime-backed execution for Cleave-compiled WASM modules.
//!
//! This is the first piece of Cleave that lives outside the C compiler.
//! It loads `.wasm` binaries produced by `cleavec --emit-wasm`, links
//! the hostcalls defined in `spec/abi/wasm.md` (`env.state_get`,
//! `env.state_set`, `env.gas_consume`, `env.event_emit`), and exposes a
//! tiny call API for invoking module exports.
//!
//! # Determinism
//!
//! Cleave-compiled modules use only `i64`/`i32` integer instructions
//! and the host imports above. They do not touch SIMD, threads, system
//! time, or random sources. Wasmtime is configured with the minimal
//! feature set that this surface needs, so two runs over identical
//! inputs against identical state produce byte-identical outputs and a
//! byte-identical post-state.
//!
//! # State persistence
//!
//! State lives on the host side, keyed by the compile-time slot index
//! the codegen assigned. An [`Instance`] owns its own state map; the
//! runtime makes no assumptions about durability. Chain-level
//! persistence (commit, rollback, state-root computation) is the
//! caller's responsibility.

use std::collections::HashMap;

use anyhow::{anyhow, Context, Result};
use wasmtime::{Caller, Config, Engine, Linker, Module, Store, Val};

pub mod evm;
pub use evm::Evm;

/// Per-instance host-side state. Owned by Wasmtime's `Store`, accessed
/// by hostcalls via [`Caller::data_mut`].
#[derive(Default)]
pub struct HostState {
    /// Slot index -> last written value. Unset slots read as 0.
    pub storage: HashMap<u32, i64>,
    /// Recorded events (id + payload bytes). v0.3 codegen never emits
    /// these; the field is here so the runtime can record them once
    /// codegen does.
    pub events: Vec<EventRecord>,
    /// Per-dimension gas consumed (dimension index -> units).
    pub gas_used: HashMap<u32, u64>,
    /// Per-dimension gas budget (dimension index -> max units). Dimensions
    /// without an entry are unmetered. When `gas_used + amount` would
    /// exceed `gas_budgets[dim]`, the `gas_consume` hostcall returns an
    /// error and Wasmtime traps the current call.
    pub gas_budgets: HashMap<u32, u64>,
}

/// A single recorded event. Cleave-compiled v0.3 modules never emit
/// these; reserved for the next codegen pass.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EventRecord {
    pub id: u32,
    pub payload: Vec<u8>,
}

/// The shared, lock-free Wasmtime engine. One per process; clone freely
/// and use to instantiate multiple modules.
pub struct Runtime {
    engine: Engine,
}

impl Runtime {
    /// Construct a runtime with a determinism-friendly Wasmtime config.
    /// The current Cleave codegen produces modules that never need
    /// SIMD, threads, or other nondeterministic features; we configure
    /// the engine to match.
    pub fn new() -> Self {
        let mut config = Config::new();
        // No SIMD or relaxed-SIMD. Codegen never emits SIMD
        // instructions; disabling these makes loading an out-of-spec
        // module fail fast instead of silently executing
        // nondeterministic code.
        //
        // We leave bulk-memory, reference-types, and multi-value at
        // their Wasmtime defaults: codegen never emits them, but
        // disabling them invalidates other defaults that Wasmtime
        // expects to coexist. Defense-in-depth here would require
        // bigger surgery on the engine config than v0 warrants.
        config.wasm_simd(false);
        config.wasm_relaxed_simd(false);
        // Enable fuel metering so we can bound total execution per
        // call.  Fuel ticks at the instruction level which catches
        // infinite loops in pure WASM (no hostcall in the loop body
        // would otherwise let a malicious module starve the node).
        config.consume_fuel(true);
        Self {
            engine: Engine::new(&config).expect("wasmtime config is always valid"),
        }
    }

    /// Load a WASM module from raw bytes, link the env-namespace
    /// hostcalls per `spec/abi/wasm.md`, and instantiate.
    ///
    /// The new instance starts with `DEFAULT_FUEL` units of Wasmtime
    /// fuel and no per-dimension gas budget set.  Callers tune both
    /// via [`Instance::set_fuel`] and [`Instance::set_gas_budget`]
    /// before the first call.
    pub fn load(&self, wasm: &[u8]) -> Result<Instance> {
        let module = Module::new(&self.engine, wasm)
            .context("loading wasm bytes into a Module")?;

        let mut store = Store::new(&self.engine, HostState::default());
        // Generous default so existing callers that do not opt into
        // metering still see fully-executed modules. Tests and chain
        // integrations should set their own budget via
        // `Instance::set_fuel` immediately after `load`.
        store
            .set_fuel(DEFAULT_FUEL)
            .expect("fuel is enabled on the engine config");

        let mut linker: Linker<HostState> = Linker::new(&self.engine);
        link_hostcalls(&mut linker)?;

        let instance = linker
            .instantiate(&mut store, &module)
            .context("instantiating module with linked hostcalls")?;

        Ok(Instance { store, instance })
    }
}

/// Default fuel budget assigned to every freshly loaded instance.
/// Generous enough for normal calls; a node operating a real chain
/// will replace this with a per-transaction budget derived from gas.
pub const DEFAULT_FUEL: u64 = 10_000_000_000;

impl Default for Runtime {
    fn default() -> Self {
        Self::new()
    }
}

/// A loaded, instantiated module ready to call.
pub struct Instance {
    store: Store<HostState>,
    instance: wasmtime::Instance,
}

impl Instance {
    /// Call an exported function by name, passing `args` as i64 values.
    /// Returns the function's single i64 return value (v0.3 codegen
    /// always emits functions with one i64 return).
    pub fn call(&mut self, name: &str, args: &[i64]) -> Result<i64> {
        let func = self
            .instance
            .get_func(&mut self.store, name)
            .ok_or_else(|| anyhow!("module has no exported function '{name}'"))?;

        let args_vec: Vec<Val> = args.iter().copied().map(Val::I64).collect();
        let mut results = vec![Val::I64(0); 1];
        func.call(&mut self.store, &args_vec, &mut results)
            .with_context(|| format!("calling '{name}'"))?;

        match results[0] {
            Val::I64(v) => Ok(v),
            other => Err(anyhow!(
                "expected i64 return from '{name}', got {other:?}"
            )),
        }
    }

    /// Read the current value of a state slot. Returns 0 for unset slots.
    pub fn state(&self, slot: u32) -> i64 {
        self.store.data().storage.get(&slot).copied().unwrap_or(0)
    }

    /// All events recorded since instantiation, in emission order.
    pub fn events(&self) -> &[EventRecord] {
        &self.store.data().events
    }

    /// Total gas consumed per dimension. Returns 0 for dimensions that
    /// have not been charged.
    pub fn gas_used(&self, dimension: u32) -> u64 {
        self.store
            .data()
            .gas_used
            .get(&dimension)
            .copied()
            .unwrap_or(0)
    }

    /// Set the per-dimension gas budget. When `gas_used + amount` for
    /// a dimension would exceed its budget, `env.gas_consume` traps
    /// the current call.  Dimensions without a budget set are
    /// unmetered.
    pub fn set_gas_budget(&mut self, dimension: u32, budget: u64) {
        self.store.data_mut().gas_budgets.insert(dimension, budget);
    }

    /// Replace the current Wasmtime fuel budget. Lower values bound
    /// total per-call execution more tightly; ~10 units per WASM
    /// instruction is a reasonable rule of thumb.  Callers should set
    /// this before the first `call` on an instance; setting it after
    /// a partially-consumed call is supported but does not refund
    /// already-spent fuel.
    pub fn set_fuel(&mut self, units: u64) -> Result<()> {
        self.store
            .set_fuel(units)
            .map_err(|e| anyhow!("set_fuel: {e}"))
    }

    /// Read the remaining Wasmtime fuel.  Useful as a coarse "how
    /// much execution did the last call consume" measurement.
    pub fn fuel_remaining(&self) -> u64 {
        self.store.get_fuel().unwrap_or(0)
    }
}

/// Register the env-namespace hostcalls onto a Wasmtime linker. The
/// signatures here are the source of truth that codegen targets; if
/// they change, `spec/abi/wasm.md` and `compiler/src/codegen.c` must
/// change in lockstep.
fn link_hostcalls(linker: &mut Linker<HostState>) -> Result<()> {
    linker
        .func_wrap(
            "env",
            "state_get",
            |caller: Caller<'_, HostState>, slot: i32| -> i64 {
                let slot = slot as u32;
                caller.data().storage.get(&slot).copied().unwrap_or(0)
            },
        )
        .context("linking env.state_get")?;

    linker
        .func_wrap(
            "env",
            "state_set",
            |mut caller: Caller<'_, HostState>, slot: i32, value: i64| {
                caller.data_mut().storage.insert(slot as u32, value);
            },
        )
        .context("linking env.state_set")?;

    linker
        .func_wrap(
            "env",
            "gas_consume",
            |mut caller: Caller<'_, HostState>, dimension: i32, amount: i64| -> Result<()> {
                let dim = dimension as u32;
                let amount = amount.max(0) as u64;
                let state = caller.data_mut();
                let prev = state.gas_used.get(&dim).copied().unwrap_or(0);
                let new_total = prev.saturating_add(amount);
                if let Some(&budget) = state.gas_budgets.get(&dim) {
                    if new_total > budget {
                        // Returning Err here causes Wasmtime to trap
                        // the current call.  The error propagates out
                        // through `Instance::call` as an Err result,
                        // which the chain layer can surface to the
                        // transaction submitter as an "out of gas".
                        return Err(anyhow!(
                            "out of gas: dimension {dim} budget {budget} \
                             exceeded ({new_total} > {budget})"
                        ));
                    }
                }
                state.gas_used.insert(dim, new_total);
                Ok(())
            },
        )
        .context("linking env.gas_consume")?;

    // event_emit signature reserved by the ABI but not yet emitted by
    // codegen. Register a no-op so future modules link cleanly without
    // a runtime version bump.
    linker
        .func_wrap(
            "env",
            "event_emit",
            |mut caller: Caller<'_, HostState>,
             id: i32,
             payload_ptr: i32,
             payload_len: i32| {
                let memory = match caller.get_export("memory") {
                    Some(wasmtime::Extern::Memory(m)) => m,
                    _ => {
                        // Module exports no memory; record the event id
                        // with an empty payload so callers can see that
                        // an emit was attempted.
                        caller.data_mut().events.push(EventRecord {
                            id: id as u32,
                            payload: Vec::new(),
                        });
                        return;
                    }
                };
                let mut buf = vec![0u8; payload_len.max(0) as usize];
                let _ = memory.read(&caller, payload_ptr as usize, &mut buf);
                caller.data_mut().events.push(EventRecord {
                    id: id as u32,
                    payload: buf,
                });
            },
        )
        .context("linking env.event_emit")?;

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A precompiled WASM module equivalent to:
    ///
    ///     module Counter {
    ///         state count: u64
    ///         fn increment() -> u64 { count = count + 1; count }
    ///         fn read() -> u64 { count }
    ///     }
    ///
    /// Captured from `cleavec --emit-wasm examples/counter-mvp.cv`.
    /// Embedding the bytes lets unit tests run without invoking the C
    /// compiler; the integration test (tests/end_to_end.rs) exercises
    /// the live cleavec output.
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

    #[test]
    fn runtime_loads_counter_module() {
        let rt = Runtime::new();
        let instance = rt.load(COUNTER_WASM).expect("counter loads");
        // Initial state is zero (unset slot reads as 0).
        assert_eq!(instance.state(0), 0);
    }

    #[test]
    fn increment_then_read_returns_one() {
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        let after_increment = instance.call("increment", &[]).unwrap();
        assert_eq!(after_increment, 1);
        let read_value = instance.call("read", &[]).unwrap();
        assert_eq!(read_value, 1);
    }

    #[test]
    fn state_persists_across_calls() {
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        for expected in 1..=5i64 {
            let v = instance.call("increment", &[]).unwrap();
            assert_eq!(v, expected, "increment call {expected} returned wrong value");
        }
        assert_eq!(instance.state(0), 5);
        assert_eq!(instance.call("read", &[]).unwrap(), 5);
    }

    #[test]
    fn deterministic_across_two_runs() {
        let rt = Runtime::new();
        let run_once = || -> Vec<i64> {
            let mut instance = rt.load(COUNTER_WASM).unwrap();
            (0..10)
                .map(|_| instance.call("increment", &[]).unwrap())
                .collect()
        };
        let a = run_once();
        let b = run_once();
        assert_eq!(a, b, "same inputs must produce same outputs");
    }

    #[test]
    fn missing_export_errors_clearly() {
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        let err = instance.call("does_not_exist", &[]).unwrap_err();
        let msg = format!("{err:#}");
        assert!(msg.contains("does_not_exist"), "error should name missing fn: {msg}");
    }

    // ====== gas-budget tests (issue #51) ======

    /// Hand-crafted WASM module: imports env.gas_consume, exports
    /// `do_work` which calls `gas_consume(0, 100)` then returns 1.
    /// Verifying budget enforcement requires actually executing a
    /// module that calls into the hostcall; the standard counter
    /// module never does.
    ///
    /// Disassembled:
    ///
    ///     (module
    ///       (type (func (param i32 i64)))
    ///       (type (func (result i64)))
    ///       (import "env" "gas_consume" (func (type 0)))
    ///       (func (export "do_work") (result i64)
    ///         i32.const 0    ;; dimension 0
    ///         i64.const 100  ;; charge 100 units
    ///         call 0         ;; gas_consume
    ///         i64.const 1    ;; return 1
    ///       ))
    #[rustfmt::skip]
    const GAS_TEST_WASM: &[u8] = &[
        // magic + version
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        // type section: 2 types
        0x01, 0x0A, 0x02,
        0x60, 0x02, 0x7F, 0x7E, 0x00,   // (i32, i64) -> ()
        0x60, 0x00, 0x01, 0x7E,         // () -> i64
        // import section: env.gas_consume : type 0
        0x02, 0x13, 0x01,
        0x03, b'e', b'n', b'v',
        0x0B, b'g', b'a', b's', b'_', b'c', b'o', b'n', b's', b'u', b'm', b'e',
        0x00, 0x00,
        // function section: 1 fn of type 1
        0x03, 0x02, 0x01, 0x01,
        // export section: "do_work" func 1 (after import 0)
        0x07, 0x0B, 0x01,
        0x07, b'd', b'o', b'_', b'w', b'o', b'r', b'k',
        0x00, 0x01,
        // code section: 1 fn body, 11 bytes
        0x0A, 0x0D, 0x01, 0x0B,
        0x00,                      // 0 locals
        0x41, 0x00,                // i32.const 0
        0x42, 0xE4, 0x00,          // i64.const 100 (signed LEB)
        0x10, 0x00,                // call 0 (gas_consume)
        0x42, 0x01,                // i64.const 1
        0x0B,                      // end
    ];

    #[test]
    fn gas_consume_without_budget_runs_freely() {
        let rt = Runtime::new();
        let mut instance = rt.load(GAS_TEST_WASM).expect("gas test wasm loads");
        let result = instance.call("do_work", &[]).expect("unmetered call succeeds");
        assert_eq!(result, 1);
        assert_eq!(instance.gas_used(0), 100);
    }

    #[test]
    fn gas_consume_under_budget_runs_and_records_usage() {
        let rt = Runtime::new();
        let mut instance = rt.load(GAS_TEST_WASM).expect("gas test wasm loads");
        instance.set_gas_budget(0, 1_000);
        let result = instance.call("do_work", &[]).expect("under-budget call succeeds");
        assert_eq!(result, 1);
        assert_eq!(instance.gas_used(0), 100);
    }

    #[test]
    fn gas_consume_over_budget_traps_with_clear_error() {
        let rt = Runtime::new();
        let mut instance = rt.load(GAS_TEST_WASM).expect("gas test wasm loads");
        instance.set_gas_budget(0, 50);
        let err = instance.call("do_work", &[]).expect_err("should trap");
        let msg = format!("{err:#}");
        assert!(
            msg.contains("out of gas"),
            "error should report out of gas: {msg}"
        );
        // gas_used stays at the pre-call value since the consume was
        // rejected before the accumulator updated.
        assert_eq!(instance.gas_used(0), 0);
    }

    #[test]
    fn gas_budget_only_applies_to_set_dimension() {
        let rt = Runtime::new();
        let mut instance = rt.load(GAS_TEST_WASM).expect("gas test wasm loads");
        // Tighten dimension 1; the wasm charges dimension 0 only, so
        // the call should still succeed.
        instance.set_gas_budget(1, 1);
        let result = instance.call("do_work", &[]).expect("orthogonal dim budget");
        assert_eq!(result, 1);
        assert_eq!(instance.gas_used(0), 100);
        assert_eq!(instance.gas_used(1), 0);
    }

    #[test]
    fn fuel_default_is_sufficient_for_counter_module() {
        // Regression guard on DEFAULT_FUEL: existing counter calls
        // must not exhaust the default fuel allocation.
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        for _ in 0..100 {
            instance.call("increment", &[]).expect("under default fuel");
        }
        assert_eq!(instance.state(0), 100);
    }

    #[test]
    fn fuel_is_actually_consumed() {
        // First confirm fuel metering is wired up at all. One
        // `increment` call should consume a non-trivial amount of
        // fuel from the default budget.
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        let before = instance.fuel_remaining();
        instance.call("increment", &[]).unwrap();
        let after = instance.fuel_remaining();
        assert!(
            after < before,
            "fuel must decrease across a call (before={before}, after={after})"
        );
    }

    #[test]
    fn fuel_exhaustion_traps_the_call() {
        let rt = Runtime::new();
        let mut instance = rt.load(COUNTER_WASM).expect("counter loads");
        // Zero fuel means even a single instruction traps. Wasmtime's
        // exact fuel cost per WASM instruction varies by backend and
        // version; zero is the one budget guaranteed to trap.
        instance.set_fuel(0).unwrap();
        let err = instance.call("increment", &[]).expect_err("should trap on fuel");
        let msg = format!("{err:#}");
        assert!(
            msg.to_lowercase().contains("fuel") || msg.to_lowercase().contains("trap"),
            "error should mention fuel exhaustion: {msg}"
        );
    }
}
