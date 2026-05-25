---
rfc: 0002
title: "extern host ABI for native function declarations"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/55
created: 2026-05-25
updated: 2026-05-25
---

# Summary

**`extern host fn` declares a function whose body lives in the host runtime, not in Cleave source.** Compiles to a WASM import under a versioned namespace (`host_v1.<name>`). Chain manifests allow-list which host functions modules may import; out-of-list imports are rejected at module load. Default allow-list is the cryptographic stdlib (`blake3`, `sha256`, `keccak256`, `ec_recover`, `bls12_381_*`). Gas accounting per-call + per-byte-of-input, charged to the cpu dimension. Host functions run outside the WASM fuel meter; they get their own dedicated gas charge instead. Host functions are forbidden from calling back into the runtime (no reentrancy into `state_get` / `state_set`); they are pure computation only.

Promised by RFC 0001 (memory model) as the sole escape hatch from Cleave's safe-language envelope. Replaces `unsafe` blocks, inline assembly, raw pointer ops, and every other failure mode that produced the smart-contract exploit history of the last decade.

# Motivation

Smart-contract platforms need a way to call expensive cryptographic primitives from contract code without running them as interpreted WASM. Hash functions, signature verification, pairing operations, big-integer arithmetic: these are 100x to 1000x faster as native host code than as WASM. Every chain has a story here:

| Chain | Mechanism | Surface |
|---|---|---|
| Ethereum / EVM | Precompiles at addresses `0x01`..`0x0a` | Hard-coded into the EVM spec |
| Substrate | Host functions on the runtime | Per-runtime, versioned |
| Solana | Syscalls (`sol_log`, `sol_keccak256`, `sol_invoke_signed`) | Defined in `solana-program` |
| Cosmos SDK | "Keepers" modules called from CosmWasm contracts | Per-chain, per-module |
| Move (Aptos / Sui) | Framework "Native functions" | Curated stdlib |

Cleave needs one too. RFC 0001 argues this is the *only* way Cleave should expose native code, since `unsafe` blocks are the source of approximately every published smart-contract exploit since 2019. The shape of that one mechanism is this RFC's subject.

# Hard constraints

Same as RFC 0001:

1. **Deterministic execution.** A host function must produce byte-identical output for byte-identical input on any node. No clocks, no random, no nondeterministic floating point, no thread scheduling.
2. **Gas metering.** Calls into host functions consume gas. The chain rejects a tx that would exceed the budget.
3. **Bounded execution.** No host function can spin forever; every host function has a documented per-call worst-case time complexity.
4. **No state mutation.** Host functions cannot call `state_get` / `state_set` or `event_emit`. They are pure functions of their inputs. The contract calls host functions, then mutates state itself with the result.
5. **No security footgun at the chain layer.** Adding a host function to a chain is a load-bearing decision; the trust model around it matches the chain's trust model around its consensus + state implementations.

# Recommended approach

## Declaration syntax

```cleave
extern host fn blake3(input: &[u8]) -> [u8; 32]

extern host fn ec_recover(
    msg_hash: [u8; 32],
    v: u8,
    r: [u8; 32],
    s: [u8; 32],
) -> Result<Address, EcRecoverError>

extern host fn bls12_381_aggregate(sigs: &[Signature]) -> Signature

extern host fn keccak256(input: &[u8]) -> [u8; 32]
```

Parses as a top-level declaration, not inside a module. Looks like a `fn` declaration with `extern host` prefix and no body.

The compiler:
- Records the declaration's signature in a per-module `extern host` table.
- Lowers each call site to a WASM import under the namespace `host_v1.<name>`.
- Refuses to emit a module that references an undeclared `extern host fn`.

The runtime:
- Registers the host function under the same WASM import name.
- Validates at module load that every `host_v1.*` import the module wants exists in the registered set.
- Validates the module's chain manifest allow-list includes every `host_v1.*` the module imports.

## Calling convention

```cleave
// Use site looks identical to a regular fn call:
fn verify_signature(msg: &[u8], sig: &[u8; 65]) -> bool {
    let hash = keccak256(msg)
    match ec_recover(hash, sig[64], slice32(sig, 0), slice32(sig, 32)) {
        Ok(addr) => addr == expected_signer,
        Err(_)  => false,
    }
}
```

No special prefix at call sites. The type checker resolves `keccak256` and `ec_recover` to extern-host signatures.

## ABI: how host functions cross the WASM boundary

WASM imports use the multi-value extension only for return types up to 2 values. For wider return values (a `[u8; 32]` is 4 i64 values), we pass an out-pointer into linear memory.

Marshaling conventions:

| Cleave type | WASM ABI |
|---|---|
| `u8`, `u16`, `u32` | `i32` (zero-extended) |
| `u64`, `u128 (low)` | `i64` |
| `u128` | two `i64` (low then high) |
| `u256` | four `i64` (LSB to MSB) |
| `bool` | `i32` (0 or 1) |
| `&[u8]` | `i32 ptr, i32 len` pair |
| `&str` | `i32 ptr, i32 len` (UTF-8, validated by caller) |
| `[u8; N]` (param) | `i32 ptr` (32-byte chunks live in linear memory) |
| `[u8; N]` (return) | caller passes `i32 out_ptr`, host writes there, fn signature returns `()` |
| `Result<T, E>` | tag (i32) + payload via out-pointer or stacked values |
| `Address` (= `[u8; 20]`) | `i32 ptr` (in), `i32 out_ptr` (out) |

Detailed encoding rules become a sub-document (`spec/abi/extern-host.md`) once Phase 5 implementation begins. This RFC commits to the shape: small primitives as i32/i64, slices as ptr+len pairs, fixed arrays via memory pointers.

## Chain manifest allow-list

```cleave
chain MyChain {
    consensus: Tendermint
    gas:       Multidim
    state:     SparseMerkle
    exec:      WasmVM
    da:        NativeDA

    host_extensions: [
        Blake3,
        Keccak256,
        EcRecover,
        Bls12_381,
    ]
}
```

`host_extensions` is a new chain-manifest key (the grammar already accepts arbitrary subsystem keys per #64, so this needs no new parser work). Its value is a set-literal of stdlib extension names.

A chain that does not list an extension cannot load modules that import from it. At module load:

```
module imports: [host_v1.blake3, host_v1.keccak256]
chain manifest: host_extensions = [Blake3]
=> load fails: module imports keccak256 but chain does not list Keccak256
```

The runtime returns a clear `"module imports host_v1.keccak256 but chain manifest does not enable Keccak256"` error.

## Versioning

Each host function lives under a versioned namespace:

- `host_v1.blake3` — the v1 implementation
- `host_v2.blake3` — a future v2 with different semantics or signature

A chain pins to specific versions in its manifest. The runtime maintains separate implementations per version. Modules compiled against v1 keep working when v2 ships; chains opt in by editing the manifest and re-running modules against the new version.

Within v1, **the implementation cannot change semantics**. Bug fixes that preserve output for all valid inputs are OK; anything observable (different return for the same input) requires a v2.

## Gas accounting

Host functions get their own gas cost, charged to the `cpu` dimension before the host call runs:

- **Fixed per-call cost** based on the function's worst-case complexity. Keccak256 is faster than ec_recover is faster than bls12_381_aggregate.
- **Per-byte cost** for variable-input functions (`blake3(&[u8])`). Linear in input length.
- **No fuel consumed inside the host function.** Once the per-call + per-byte gas is paid, the host implementation runs to completion. This matches Substrate / EVM precompiles.

Concrete pricing (initial proposal; tunable):

| Function | Fixed cost | Per-byte cost |
|---|---:|---:|
| `blake3` | 100 | 12 |
| `keccak256` | 100 | 6 |
| `sha256` | 250 | 50 |
| `ec_recover` | 3,000 | 0 |
| `bls12_381_pairing` | 80,000 | 0 |
| `bls12_381_aggregate` (per sig) | 12,000 | 0 |

These numbers are placeholders. Real numbers come from benchmarking. The RFC commits to the shape (fixed + per-byte), not the values.

## Trust model

The default `host_extensions` set (Blake3, Keccak256, EcRecover, Bls12_381, etc.) is shipped with the stdlib runtime, implemented in audited Rust (likely vendored from existing crates: `blake3`, `tiny-keccak`, `k256`, `bls12_381`). Auditing happens once at the runtime level.

Custom host functions (not in the stdlib set) require a chain operator to:

1. Provide the Rust implementation
2. Build a custom runtime that registers it
3. Add the name to the chain's `host_extensions`

This is a meaningful barrier and a deliberate one. A chain that registers a malicious host function controls the chain anyway (consensus, state, etc.); the trust model is the same as for any chain operator. The RFC does NOT propose a community registry of unblessed custom host functions; that's a different (and harder) problem.

# Resolutions to open questions

## 1. Allow-list scope — DECIDED: per-chain

**Per-chain** via the `host_extensions` manifest key, not per-module. Reasoning:

- A module that imports `host_v1.blake3` does not need to know *which* chain it deploys to.
- The chain is the security boundary; the operator decides which native primitives their chain provides.
- Modules can be shared across chains; if module-level allow-lists were the contract, every module would need a chain-specific manifest, which kills reusability.

Per-module declarations exist as a syntax artifact (`extern host fn blake3(...) -> ...`) so the type checker knows the signature. The allow-list / authorization layer sits one level up at the chain manifest.

## 2. Versioning — DECIDED: namespace-prefix versioning

`host_v1.blake3`, `host_v2.blake3`. Chains pin to specific versions; modules compiled against v1 keep working until the chain explicitly migrates. Within a version, semantics are frozen.

Alternative considered: SHA-of-implementation hash as the import name. Rejected: brittle (implementation changes that preserve behavior would force module recompiles), and prone to operator error (typo in the hash silently changes which function runs).

## 3. Determinism enforcement — DECIDED: stdlib implementations are audited; custom is operator's problem

The stdlib host function set ships with reference implementations vetted for determinism (no clocks, no random, no platform-specific floating-point). Custom extensions are the chain operator's responsibility; the RFC does not propose runtime enforcement (e.g., wrapping the host function in a determinism sandbox).

This is the same trust contract that today applies to consensus + state implementations. A chain operator who registers a nondeterministic host function has bigger problems than this RFC can fix.

## 4. Gas accounting — DECIDED: fixed per-call + per-byte, charged before execution

Detailed above. Charged to the `cpu` gas dimension by default. Functions that hit storage (none in the stdlib initial set) would charge `storage` as well.

## 5. Type marshaling — DECIDED: detailed table above, full spec in `spec/abi/extern-host.md` once Phase 5 begins

Small primitives as i32/i64; slices as `(ptr, len)` pairs; fixed-size arrays via in-linear-memory pointers. `Result<T, E>` via tag + payload. Detailed encoding lives in a sub-document; this RFC commits to the framework.

## 6. Trust model — DECIDED: stdlib is blessed, custom is operator's responsibility

No community registry. No third-party host functions in v0. Chains that want custom primitives build a custom runtime. RFC 0005 (third-party protocols) covers the longer-term ecosystem question.

## 7. Composition / reentrancy — DECIDED: forbidden

Host functions cannot call `state_get`, `state_set`, `event_emit`, or any other env-namespace import. The runtime enforces this by giving host functions a `Caller<HostState>` that exposes no methods for state access.

Rationale:

- Host functions are pure computation. State access is the contract's responsibility.
- Reentrancy bugs are the source of most cross-contract exploits; banning the easy reentrancy path closes a whole class.
- A host function that genuinely needs state should be redesigned as a regular contract function with the state access in Cleave code, calling a smaller host function for the pure-compute part.

# Concrete syntax: full lifecycle of a host function

## 1. Stdlib declares the signature

In `spec/stdlib/extern-host.cv` (notional location):

```cleave
extern host fn blake3(input: &[u8]) -> [u8; 32]
```

## 2. A module uses it

```cleave
module Token {
    state balances: Map<Address, u128>

    fn verify_signed_transfer(
        signer: Address,
        to: Address,
        amount: u128,
        sig: &[u8; 65],
    ) -> bool {
        let mut buf = Vec::with_capacity(72)
        buf.extend_from_slice(&signer.as_bytes())
        buf.extend_from_slice(&to.as_bytes())
        buf.extend_from_slice(&amount.to_le_bytes())

        let hash = blake3(&buf)
        let recovered = match ec_recover(hash, sig[64], slice32(sig, 0), slice32(sig, 32)) {
            Ok(a) => a,
            Err(_) => return false,
        }
        recovered == signer
    }
}
```

The compiler sees `blake3` and `ec_recover` as `extern host fn`, lowers each call to a WASM import.

## 3. The chain manifest authorizes them

```cleave
chain MyToken {
    consensus: Tendermint
    exec:      WasmVM
    state:     SparseMerkle
    gas:       Multidim
    da:        NativeDA
    host_extensions: [Blake3, EcRecover]
}
```

## 4. At module load the runtime validates

```
module imports host_v1.blake3:       Blake3 in chain.host_extensions -> OK
module imports host_v1.ec_recover:   EcRecover in chain.host_extensions -> OK
```

Module loads. Subsequent calls into `blake3` / `ec_recover` cross the WASM boundary into native Rust code, return values are marshaled per the ABI table, and gas is charged before each call.

## 5. A different chain rejects the same module

```cleave
chain AnotherChain {
    consensus: Tendermint
    exec:      WasmVM
    ...
    host_extensions: [Blake3]   // missing EcRecover
}
```

The runtime rejects loading the Token module: `"module imports host_v1.ec_recover but chain manifest does not enable EcRecover"`. The chain operator either adds EcRecover or refuses the module.

# Initial stdlib host function set

Proposed v1 stdlib:

| Function | Purpose | Source crate (Rust) |
|---|---|---|
| `blake3(input: &[u8]) -> [u8; 32]` | Modern fast hash | `blake3` |
| `sha256(input: &[u8]) -> [u8; 32]` | Bitcoin / TLS compatibility | `sha2` |
| `keccak256(input: &[u8]) -> [u8; 32]` | EVM compatibility | `tiny-keccak` |
| `ec_recover(hash, v, r, s) -> Result<Address, _>` | Ethereum sig recovery | `k256` |
| `bls12_381_pairing(g1: &[u8; 96], g2: &[u8; 192]) -> bool` | Pairing check | `bls12_381` |
| `bls12_381_aggregate(sigs: &[Signature]) -> Signature` | BLS sig aggregation | `bls12_381` |
| `ed25519_verify(msg: &[u8], pubkey: &[u8; 32], sig: &[u8; 64]) -> bool` | Modern signature verification | `ed25519-dalek` |

Excluded from v1 (worth their own RFC):

- Generic BigInteger ops (need careful gas accounting for variable-size inputs)
- ZK proof verification (Groth16, Plonk, etc.) — heavyweight; chain-specific
- Random source — non-trivial to make deterministic; out of scope until VRF infrastructure exists

# Counterarguments

## "Just compile crypto to WASM and run it under fuel."

Empirically too slow. Keccak256 in pure WASM is ~50x slower than native. For real chain throughput, native crypto is necessary, not optional. EVM ships precompiles at fixed addresses for exactly this reason; Substrate ships host functions; every production chain has solved this problem the same way.

## "Allow arbitrary user code as host functions via WASM imports."

Defeats the purpose. The whole point of `extern host` is "this code is audited, deterministic, gas-priced, and runs outside the safe envelope." Letting users register arbitrary native code is just `unsafe` with extra steps. The chain operator can opt into custom host functions (they control the runtime build) but ordinary developers cannot.

## "Why not WebAssembly Component Model?"

Component Model is the right long-term answer for typed cross-language interop. Today's reality: tooling is immature, not all WASM runtimes support it (Wasmtime's support is recent and partial), and the design is still moving. Cleave commits to a simpler, narrower interface now and migrates to Component Model when it stabilizes. The host function namespace versioning (`host_v1.*`) gives us a clean upgrade path.

## "Reentrancy ban kills useful patterns. What about a host function that needs to read storage?"

Acknowledged constraint. Real cases this rules out:

- "Read this state slot, then hash it" — easy workaround: read in Cleave, pass to `blake3(...)` in Cleave
- "Crypto-verify a state-derived key" — same workaround
- A genuinely state-aware host function (e.g., a precompile that reads storage to validate ZK proofs) — out of scope for v1; a future "stateful host function" RFC can lift the ban if real cases demand it

The ban is reversible (a future RFC can extend the env-passed-to-host-fn surface); the inverse direction (allowing reentrancy then trying to take it back) is not.

# Migration path

- **Existing examples**: no impact. None of them use `extern host fn`.
- **The compiler**: adds extern-host parsing, signature recording, WASM import lowering. ~250 LoC.
- **The runtime**: adds the v1 host function registration, per-chain manifest allow-list parsing, module-load validation, per-call gas charging. ~400 LoC plus the implementations.
- **`spec/abi/`**: gains `extern-host.md` documenting the byte-level ABI.
- **`spec/grammar.ebnf`**: gains `extern host fn` as a top-level declaration form.

No deployed chains to migrate (we have none). The first chain shipped after Phase 5 lands declares its `host_extensions` upfront; subsequent contracts target whatever's in the manifest.

# Implementation roadmap

Drops into Phase 5 of RFC 0001 (after the memory model phases land):

## Phase 5a: Grammar + parsing (~150 LoC, 1 PR)

- Lexer: `extern` keyword
- Parser: `extern host fn name(params) -> return_type` as a top-level declaration
- AST: `AST_EXTERN_HOST_DECL` node with signature
- Tree-sitter grammar updated in lockstep

## Phase 5b: Type checker integration (~80 LoC, included in same PR)

- Resolve `extern host fn` signatures
- Calls into extern-host functions type-check like regular fn calls
- Type checker tracks "this fn is extern host" for codegen's benefit

## Phase 5c: Codegen import lowering (~200 LoC, 1 PR)

- Emit WASM imports under `host_v1.*` namespace
- Marshal arguments per the ABI table
- Call sites become `call <import_index>` after the imports section

## Phase 5d: Runtime registration + chain manifest (~300 LoC, 1 PR)

- `Runtime::register_host_fn(name, version, impl)` API
- Chain manifest `host_extensions:` parser
- Module-load validation
- Initial stdlib registration: blake3, keccak256, sha256, ec_recover (skip BLS / ed25519 for first ship; add in a follow-up)
- Per-call gas charging via the existing `gas_consume` machinery

## Phase 5e: Gas pricing + bench (~100 LoC, 1 PR)

- Concrete gas costs per stdlib function
- Bench: measure native cost, set gas at ~10x the measured cost (defensive pricing)
- README + spec doc with the final pricing table

## Total

~830 LoC of compiler / runtime + ~300 LoC of stdlib host fn implementations + ~150 LoC of byte-level ABI spec. Phase 5 is the smallest of the five RFC 0001 phases.

# Remaining open questions

Reduced from 7 to 2:

1. **Should the stdlib host function set ship as separate runtime build flags?** (i.e., a "minimal" build that only includes blake3 + keccak256, vs a "full" build with BLS + pairing). Lean towards "full by default, flags for minimal."
2. **VRF / random source**: out of scope for v1, but worth flagging that this is the most likely v2 addition. Needs careful design around determinism + chain-randomness sources.

# Reversibility

**Medium.** Once contracts call `host_v1.blake3`, that import name is frozen. Adding new functions in `host_v1.*` is a strict extension. Changing existing ones requires a `host_v2.*` namespace and chain-by-chain migration.

The chain manifest format (`host_extensions: [...]`) is reversible if we change our minds about allow-listing — the field is just a value, not a structural choice. We could drop the allow-list later (less safe) or extend it to take per-function gas overrides (richer config), without breaking deployed modules.

# Decision criteria

This RFC is ready to graduate from `draft` to `accepted` when:

1. No outstanding maintainer objection
2. Substrate or EVM precompiles experience is reflected (someone who has shipped runtime host functions reviews the spec)
3. The first stdlib host function (blake3) has a working end-to-end prototype, even on a feature branch

Target: 2-3 weeks from RFC 0001 acceptance. RFC 0001 must land first since `extern host` is the escape hatch the memory model relies on.

# Related work

- Substrate `pallet-contracts` runtime API documentation
- Solana SBPF syscalls reference
- Ethereum Yellow Paper precompiled contracts section
- WebAssembly Component Model (the long-run industry direction)
- Move's "Native functions" in the Aptos / Sui framework

# Discussion

Comments on the tracking issue (#55). RFC stays in `draft` until the decision criteria are met.
