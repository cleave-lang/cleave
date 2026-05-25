---
rfc: 0002
title: "extern host ABI for native function declarations"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/55
created: 2026-05-25
---

Promised in the memory-model RFC (#42) as the escape hatch for raw-performance primitives (crypto, hashing, signature verification) that should not run inside the WASM gas meter.

## Status

Draft. Open for discussion.

## Context

Smart-contract platforms universally need a host-function mechanism for expensive primitives. Substrate calls them "host functions," Solana calls them "syscalls," EVM has "precompiles," Cosmos SDK has "keepers." Each ecosystem has reinvented this with subtly different semantics.

Cleave needs one too. The memory-model RFC argues it should be the ONLY escape from the safe-language envelope (no `unsafe` block, no raw pointer ops). That puts more weight on getting the ABI right.

## Strawman

```cleave
extern host fn blake3(input: bytes) -> [u8; 32]
extern host fn bls12_381_aggregate(sigs: Vec<Signature>) -> Signature
extern host fn ec_recover(message: [u8; 32], v: u8, r: [u8; 32], s: [u8; 32]) -> Address
```

- `extern host fn` is a declaration, not a definition. The function body lives in the host (Rust / C / etc.).
- Codegen lowers calls to WASM imports under a `host` namespace (vs the `env` namespace used by `state_get` / `state_set`).
- The runtime crate exposes a registration API: `runtime.register_host_fn("blake3", impl)`.
- Per-chain manifest declares which host functions are allowed: `host_functions: [blake3, bls12_381_aggregate]`.

## Questions

1. **Who declares the allow-list?** Per-chain (chain manifest) or per-module (module declaration)?
2. **Versioning.** If `blake3`'s implementation changes, how does the runtime know it's the new version?
3. **Determinism.** Host functions must be deterministic across nodes. How do we enforce that the registered implementation actually is?
4. **Gas accounting.** Host functions run outside the WASM gas meter. Do they have their own gas surface? Charged per-call? Per byte of input?
5. **Type marshaling.** WASM linear memory vs Cleave values. Vec<T>, String, bytes need ABI conventions.
6. **Trust model.** Host functions can do anything the host can do. Who audits them? Is there a "stdlib host function set" that's blessed?
7. **Composition.** Can a host function call back into the runtime to read state? (Probably no, but worth being explicit.)

## Reversibility

Medium. Once contracts call `host::blake3(...)`, the ABI for `blake3` is frozen. Adding new host functions is easy; changing existing ones requires a chain hard fork.

## Related work

- Substrate `pallet-contracts` runtime API
- Solana SBPF syscalls (`sol_log`, `sol_keccak256`, `sol_invoke_signed`)
- EVM precompiles (the 0x01..0x0a address space)
- WebAssembly Component Model (the longer-term industry direction)

Discussion: comment thread. RFC stays draft for at least two weeks before any binding decision.

