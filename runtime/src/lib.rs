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
    /// Per-dimension gas consumed (dimension index -> units). Total is
    /// uncapped in v0; an out-of-gas trap will land with the next
    /// runtime iteration.
    pub gas_used: HashMap<u32, u64>,
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
        Self {
            engine: Engine::new(&config).expect("wasmtime config is always valid"),
        }
    }

    /// Load a WASM module from raw bytes, link the env-namespace
    /// hostcalls per `spec/abi/wasm.md`, and instantiate.
    pub fn load(&self, wasm: &[u8]) -> Result<Instance> {
        let module = Module::new(&self.engine, wasm)
            .context("loading wasm bytes into a Module")?;

        let mut store = Store::new(&self.engine, HostState::default());

        let mut linker: Linker<HostState> = Linker::new(&self.engine);
        link_hostcalls(&mut linker)?;

        let instance = linker
            .instantiate(&mut store, &module)
            .context("instantiating module with linked hostcalls")?;

        Ok(Instance { store, instance })
    }
}

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
            |mut caller: Caller<'_, HostState>, dimension: i32, amount: i64| {
                let dim = dimension as u32;
                let amount = amount.max(0) as u64;
                let used = caller.data_mut().gas_used.entry(dim).or_insert(0);
                *used = used.saturating_add(amount);
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
}
