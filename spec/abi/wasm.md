# WASM hostcall ABI

This document defines the interface between Cleave-compiled WASM modules and the runtime that executes them. The compiler (#17) emits modules that import these hostcalls; the runtime (#18) provides implementations.

**Status:** stable contract for the v0.3 runtime. Future versions of Cleave can extend this ABI; backward-incompatible changes ship behind a major-version bump on the module's binary header.

## Module shape

A Cleave-compiled WASM module:

- Imports its hostcalls from the `env` namespace.
- Exports every public `fn` declared in the source module under its source name.
- Uses `i64` as the canonical value type for integer state and arithmetic. Narrower primitives (`u32`, `bool`, etc.) are sign- or zero-extended to `i64` at hostcall boundaries.
- Holds no global mutable state. All durable state goes through `state_*` hostcalls so the runtime owns the storage substrate.

The runtime is expected to AOT-compile every module on load. Modules that cannot be AOT-compiled by Wasmtime are rejected.

## Hostcalls

### `env.state_get(slot: i32) -> i64`

Reads the value of the state slot identified by `slot`. Slots are allocated by the compiler in source order, starting at `0`, one per `state` declaration in the module.

- `slot`: the compile-time-assigned slot index.
- Return: the current value of the slot, zero-extended to `i64` for narrower types. Reads of an unset slot return `0` (state is zero-initialized at module instantiation).

### `env.state_set(slot: i32, value: i64) -> ()`

Writes `value` to the state slot identified by `slot`. The runtime is responsible for atomicity within a single transaction's execution. Persistence across transactions is the runtime's contract with the consensus layer.

- `slot`: the compile-time-assigned slot index.
- `value`: the new value, truncated to the slot's declared width on store. Truncation is silent; the type checker is responsible for catching the static cases.

### `env.gas_consume(dimension: i32, amount: i64) -> ()`

Charges `amount` units of gas to the given gas dimension. Dimensions are compile-time-assigned indices into the module's `gas` declaration record. The runtime aborts the current transaction with an out-of-gas trap if `amount` would exceed the dimension's remaining budget.

- `dimension`: index of the gas dimension (`cpu = 0`, `storage = 1`, etc., in declaration order).
- `amount`: units to deduct.

Codegen emits `gas_consume` calls inline; the compiler is responsible for accumulating per-block gas costs. v0.3 emits a single `gas_consume` per function entry that charges the function's declared cost; per-instruction metering is a later iteration.

### `env.event_emit(id: i32, payload_ptr: i32, payload_len: i32) -> ()`

Emits an event. The payload is a contiguous byte buffer in the module's linear memory; the runtime is responsible for copying it before returning.

- `id`: compile-time-assigned event index.
- `payload_ptr`: offset into module memory.
- `payload_len`: byte length of the payload.

v0.3 codegen does NOT yet emit `event_emit` calls; the import is declared but unused until the event-serialization layer lands. The signature is reserved here so future modules can be loaded by today's runtime without ABI churn.

## Slot allocation

The compiler assigns slot indices statically:

| Source construct | Slot space | Allocation |
|---|---|---|
| `state` declaration | state slots | `0..N` in source order |
| Gas dimension (key in the `gas` record) | gas slots | `0..N` in source order |
| `event` declaration | event slots | `0..N` in source order |

Slot indices are stable across recompilations of identical source. They are NOT stable across source edits: adding a new `state` decl in the middle of a module reshuffles the slot space. The runtime is responsible for migrating durable state across schema versions; the ABI deliberately does not encode source names into hostcall parameters because that would force every load to do a name lookup.

## Linear memory

v0.3 codegen does NOT yet allocate or use linear memory. Modules declare a memory section only if a future codegen pass needs it (e.g. for `event_emit` payloads). Until then, the runtime instantiates modules with a single page (64 KB) of zero-initialized memory that goes unused.

## Function signatures

All functions in the v0.3 codegen surface have the shape:

```
(func (param ...i64) (result i64))
```

Integer primitives narrower than `i64` are widened at parameter / return boundaries; the typed AST already records the source widths so future codegens can choose tighter representations. Functions that the source declares with no return type are emitted with a synthetic `i64` return of `0` to keep the codegen uniform; this is invisible to callers since the compiler refuses to use the value of a `()`-returning function.

## What this ABI does NOT yet cover

Each of these is its own follow-up issue:

- Sum types (`Result<T>`, `Option<T>`, enums): need a tag + payload memory layout
- Struct field types: need offset metadata and `struct_get`/`struct_set` hostcalls or in-module memory ops
- String allocation and ownership rules
- Effect handlers from RFC #9: effects compile to specially-named hostcalls today
- Multi-VM interop: calling EVM/SVM contracts from a Cleave module
- Cross-module calls within the same chain
