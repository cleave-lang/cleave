---
rfc: 0004
title: "Cross-engine state sharing (one state root across WASM and EVM)"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/46
created: 2026-05-25
---

The runtime ships two engines (Wasmtime + REVM, as of v0.3). Each has its own state backend today. The original #19 issue specified "State writes from REVM and the Cleave VM coexist in the same state tree (the chain has one state root)" as an acceptance criterion that was deferred.

This RFC picks the design before the implementation issue starts.

## Status

Draft.

## The shape of the problem

WASM and EVM have different storage semantics:

| | WASM (Cleave) | EVM |
|---|---|---|
| Key | u32 slot index | (Address, 32-byte storage key) |
| Value | u64 (with widening) | 32-byte word |
| Scope | per-module | per-contract |
| Identity | source-declared slot | deterministic deploy address |

A unified state needs an answer for:

1. How do WASM modules and EVM contracts address each other's state?
2. Can a WASM module read an EVM contract's storage and vice versa?
3. Does the state root commit both engines uniformly?

## Design candidates

### (a) Two namespaces, single root

```
state_root = merkle(wasm_state || evm_state)
```

Both engines have their own KV; the chain commits them under a single root for hash purposes but engines don't see each other's data. Simple; "one state root" claim is true; but cross-engine reads still need a bridge layer.

### (b) Unified KV addressed by `(engine, address, key)`

Both engines write through one KV. WASM module identity maps to a synthetic Address. State keys are unified bytes. Engines can read each other's storage with the right `(engine, address, key)` tuple.

### (c) EVM-shaped unified state, WASM is the second-class citizen

Make WASM `state_set(slot)` desugar to `evm_set(THIS_MODULE_ADDR, slot_as_word, value_as_word)`. Universal store; WASM and EVM speak the same storage substrate.

### (d) WASM-shaped unified state, EVM is the second-class citizen

Reverse of (c). Less likely since the EVM storage shape (32-byte words, address-keyed) is more general.

## Questions

1. **Pick a candidate.** Which of (a)-(d), or a fifth, do we want?
2. **Synthetic addresses.** If WASM modules need addresses, how are they assigned? Hash of source? Deploy-order index?
3. **Cross-engine call semantics.** If a WASM module CAN read EVM storage, can it also call an EVM contract directly? Or only via host functions?
4. **State proofs.** Do light clients verify each engine's state separately, or unified?
5. **Migration.** v0.3 chains shipped with split state. Upgrading them to unified state is its own headache.
6. **Performance.** Unified KV adds an indirection. Bench impact?

## Reversibility

Low. Once contracts depend on a specific cross-engine convention, changing it requires a hard fork.

## Related

- Companion implementation issue (separate)
- Memory model RFC (#42) — interacts with state value shapes
- DataAvailabilityProtocol RFC (#16, closed) — state and DA both go through commit, must compose
