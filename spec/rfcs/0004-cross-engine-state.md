---
rfc: 0004
title: "Cross-engine state sharing (one state root across WASM and EVM)"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/46
created: 2026-05-25
updated: 2026-05-25
---

# Summary

**Unified KV backend addressed by `(engine, identity, key)` triples**, with a single Merkle root over the whole space. WASM modules and EVM contracts each speak their native storage surface (slot-indexed for WASM, `(address, word)` for EVM); the runtime translates both to the shared substrate. WASM "module identity" is a synthetic 20-byte address derived from `keccak256(chain_id || module_name || module_version)`, deterministic across nodes. Cross-engine reads work via a host function (RFC 0002), not direct cross-engine fn calls.

Picks option (b) from the four candidates the original RFC laid out. Closes the "one state root" claim from the v0.3 EVM PR (#19) that's been deferred since.

# Motivation

Today the WASM engine and the REVM engine each own their own state. A chain that deploys both a Cleave WASM module and a Solidity contract has two state trees, two roots, and no clean way for them to share data:

```
v0.3 reality:
  Wasmtime HostState.storage:   HashMap<slot_id, i64>
  REVM CacheDB:                  per-address HashMap<storage_key, value>
```

Two consequences:

1. **The multi-VM claim is technically false today.** "A chain manifest declares `exec: WasmVM` + EVM contracts" works at the dispatch layer; "they share state" does not.
2. **Cross-VM call patterns are blocked.** A Cleave module that wants to read an ERC-20 balance has no path. A Solidity contract that wants to call into a Cleave module has no path.

Picking now lets the v0.4 runtime work proceed with confidence. Picking late means either retrofitting (painful) or shipping a v0.4 with the same v0.3 limitation (the multi-VM story stays half-built).

# Hard constraints

1. **Determinism.** Cross-engine state reads must be byte-identical across nodes. No timing-dependent or implementation-dependent traversal.
2. **Single Merkle root.** The state tree's root commitment covers BOTH engines' data. A light client verifies one root, not two.
3. **Compatibility with EVM semantics.** Solidity contracts deployed on Cleave chains must see standard EVM storage behavior. We cannot break the Solidity model.
4. **Performance: bounded indirection.** The shared substrate must be cheap. Expected overhead: less than 5% on the hot-path WASM `state_get` / `state_set` numbers. If real measurements exceed that, the design changes.
5. **No magic state-mutation paths.** State changes go through the documented hostcalls; the shared substrate is not a back door for either engine to silently mutate the other's data.

# The shape of the problem

WASM (Cleave) and EVM have different native storage semantics:

| | WASM (Cleave) | EVM |
|---|---|---|
| Key | `u32` slot index | `(Address, 32-byte storage key)` |
| Value | `u64` (with widening) | 32-byte word |
| Scope | per-module | per-contract |
| Identity | source-declared slot | deterministic deploy address |
| Stored where | `HostState.storage` HashMap (v0.3) | REVM `CacheDB` (v0.3) |

A unified state needs an answer for:

1. How do WASM modules and EVM contracts address each other's state?
2. Can a WASM module read an EVM contract's storage and vice versa?
3. Does the state root commit both engines uniformly?

# Design candidates considered

## (a) Two namespaces, single root

`state_root = merkle(wasm_state || evm_state)`. Both engines have their own KV; the chain commits them under a single root for hash purposes but engines don't see each other's data.

- ✅ Simple, "one state root" claim is true
- ❌ Cross-engine reads still need a bridge layer, which is what this RFC was supposed to define. Ducks the question.

## (b) Unified KV addressed by `(engine, identity, key)` — RECOMMENDED

Both engines write through one KV. WASM module identity maps to a synthetic 20-byte address. State keys are unified bytes. Engines can read each other's storage with the right `(engine, address, key)` tuple.

- ✅ Real cross-engine reads work
- ✅ Single Merkle tree, single root
- ✅ Each engine still sees its native surface
- ⚠️ Adds one level of indirection in the hot-path (acceptable per perf constraint)

## (c) EVM-shaped unified state, WASM is the second-class citizen

Make WASM `state_set(slot)` desugar to `evm_set(THIS_MODULE_ADDR, slot_as_word, value_as_word)`. Universal store; WASM and EVM speak the same storage substrate.

- ✅ Maximum interop, EVM contracts trivially see WASM state
- ❌ Forces WASM to think in 32-byte words and addresses, killing the simpler WASM slot model. WASM modules become harder to write for no gain in WASM-only chains. v0.3 hostcall ABI gets broken.

## (d) WASM-shaped unified state, EVM is the second-class citizen

Reverse of (c). Both engines route through a slot-index model.

- ❌ EVM compatibility breaks. Solidity contracts expect 32-byte words at address-keyed locations. No path back to standard EVM tooling.

# Recommended approach: option (b) in detail

## Unified KV layout

The shared state backend is a flat `Map<Vec<u8>, Vec<u8>>` keyed by canonical-encoded `(engine, identity, key)` triples. Encoding:

```
        1 byte      20 bytes               variable
       +-------+ +-------------------+ +-------------------+
key =  | engine| |   identity        | |   engine-specific key  |
       +-------+ +-------------------+ +-------------------+

engine = 0x00 (WASM) or 0x01 (EVM)
```

Engine-specific key encoding:

| Engine | Identity | Engine-specific key |
|---|---|---|
| WASM | 20-byte synthetic module address | 4-byte big-endian slot index (zero-padded for u32) |
| EVM | 20-byte deployed contract address | 32-byte EVM storage key |

Storage values are byte slices. Length varies per engine:

| Engine | Value |
|---|---|
| WASM | 8 bytes (i64 little-endian) for v0; variable length once memory-model heap types ship per RFC 0001 |
| EVM | 32 bytes (always) |

## Synthetic WASM module addresses

A WASM module's 20-byte address is derived deterministically:

```
module_address = keccak256(
    chain_id        (32 bytes, big-endian)
    || module_name  (UTF-8 bytes)
    || module_version (4 bytes, big-endian u32)
)[0..20]
```

Properties:

- **Deterministic across nodes**: every validator computes the same address for a given chain + module + version.
- **Independent of deploy order**: redeploying a module to the same chain with the same name + version produces the same address.
- **Version-aware**: bumping the version produces a fresh address. Migration is explicit.
- **No collision with EVM addresses**: the keccak256 prefix happens to overlap the EVM address space, so collision is possible (1 in 2^160 if both are uniformly random). The RFC accepts this; it's the same property Ethereum has between EOAs and contract addresses.

Rationale: deploy-order indexing was the alternative, but it ties module identity to who deployed first, which breaks reproducibility across testnets / mainnet / forks. Keccak256 of source data sidesteps all of that.

## Hostcall surface (unchanged for WASM contracts)

WASM contracts continue to call:

```
env.state_get(slot: i32) -> i64
env.state_set(slot: i32, value: i64)
```

The runtime layer expands these to unified-KV operations:

```rust
fn state_get(slot: u32) -> i64 {
    let key = encode_kv_key(WASM_ENGINE, self.module_addr, slot.to_be_bytes());
    let bytes = self.shared.get(&key).unwrap_or_default();
    decode_i64_le(&bytes)
}

fn state_set(slot: u32, value: i64) {
    let key = encode_kv_key(WASM_ENGINE, self.module_addr, slot.to_be_bytes());
    self.shared.set(key, value.to_le_bytes().to_vec());
}
```

Contracts are unaffected; the bytes-on-disk layout becomes the unified one.

## REVM database integration

The EVM engine plugs into the shared substrate via REVM's `Database` trait. The `storage` callback maps each `(contract_addr, storage_key)` to the unified KV:

```rust
impl Database for SharedDb {
    fn storage(&mut self, addr: Address, key: StorageKey) -> Result<StorageValue, _> {
        let kv_key = encode_kv_key(EVM_ENGINE, addr, key.to_be_bytes());
        let bytes = self.shared.get(&kv_key)?;
        Ok(StorageValue::from_be_bytes(...))
    }

    fn commit(&mut self, ...) { ... }
}
```

REVM continues to see standard EVM semantics; the only change is the storage backend.

## Cross-engine reads

A WASM module that wants to read an EVM contract's storage uses an `extern host` function (RFC 0002):

```cleave
extern host fn evm_read(addr: Address, key: [u8; 32]) -> [u8; 32]

fn read_other_balance(holder: Address) -> u256 {
    let key = compute_balance_slot(holder)
    u256::from_be_bytes(evm_read(MY_EVM_TOKEN_ADDR, key))
}
```

A Solidity contract that wants to read a WASM module's state uses an EVM precompile at a reserved address. v0 ships `0xC0..0x..C1` (TBD) with this signature:

```solidity
function readWasmSlot(address moduleAddr, uint32 slot) external view returns (uint64)
```

Cross-engine WRITES are not supported in v0. A contract can read across engines but not write across engines. Rationale:

- Cross-engine writes need transaction-level atomicity guarantees that v0 doesn't have
- The write path makes the state model harder to reason about (who's responsible for the data?)
- Cross-engine reads cover the 80% case (price oracles, balance lookups, registry queries) without the safety tax of writes

The 20% case (cross-engine writes) becomes a v1 RFC if real usage demands it.

## State proofs and light clients

The Merkle tree commits over the unified KV. Light client proofs are uniform: one proof shape regardless of which engine wrote the data being proven. A light-client that wants to verify "balance of address X in EVM token Y" gets a single Merkle inclusion proof against the chain's root.

This is a deliberate departure from the v0.3 fiction where the two engines kept separate trees. Concretely: the v0.5 state-protocol implementation (#54) commits over the unified KV from day one; no migration story needed because v0.3 chains aren't in production.

## Performance bound

The unified KV adds one prefix-encoding step per `state_get` / `state_set`. Quick estimate:

- `state_get` hot path today: ~120 ns (Wasmtime call dispatch + HashMap lookup)
- Add prefix encoding: ~10 ns extra (1 byte engine tag + 20 byte module addr + 4 byte slot, into a fixed-size key buffer)
- Total: ~130 ns, ~8% slowdown

8% is above the 5% constraint; needs optimization or relaxation. Likely optimizations:

- Cache the encoded prefix per-instance (the engine tag + module address don't change for the lifetime of a call)
- Inline the encoding rather than allocating a Vec

With caching, the prefix is just `keybuf[24] = slot.to_be_bytes()` per call, well under 1ns. Real bench numbers go in the implementation issue (#50).

# Resolutions to open questions

Reduced from 6 to 1:

## 1. Pick a candidate — DECIDED: option (b)

Unified KV addressed by `(engine, identity, key)`. Each engine speaks its native surface; the runtime translates both into the shared substrate.

## 2. Synthetic addresses — DECIDED: keccak256 of (chain_id, name, version)

Detailed above. Deterministic, version-aware, no dependency on deploy order.

## 3. Cross-engine call semantics — DECIDED: reads only, via host fns / EVM precompile

Cross-engine reads land in v0. Cross-engine writes deferred to a future RFC.

## 4. State proofs — DECIDED: unified

One Merkle tree, one root, one proof shape per (engine, identity, key).

## 5. Migration — DECIDED: not applicable

No production chains. v0.3 in-memory state is process-local; v0.4 chains start fresh with the unified KV.

## 6. Performance — OPEN, until benched

The 8% prefix-encoding estimate is back-of-envelope. The implementation issue (#50) benchmarks against `state_get` / `state_set` hot path and either confirms the <5% target or motivates further optimization. If it really can't get under 5%, we relax the constraint to <10% rather than redesign.

# Concrete syntax for cross-engine reads

## WASM reads EVM (via host function)

```cleave
extern host fn evm_storage_read(contract: Address, key: [u8; 32]) -> [u8; 32]

fn check_external_balance(token: Address, holder: Address) -> u256 {
    let slot = compute_balance_slot(holder)
    let bytes = evm_storage_read(token, slot)
    u256::from_be_bytes(bytes)
}
```

The `evm_storage_read` extern host fn lives in the stdlib (RFC 0002). Its implementation in the runtime reads from the shared KV with engine tag = `0x01`.

## EVM reads WASM (via precompile)

Solidity:

```solidity
interface IWasmStateReader {
    function readU64(address module, uint32 slot) external view returns (uint64);
}

contract Bridge {
    IWasmStateReader constant WASM_READER = IWasmStateReader(0x00000000000000000000000000000000000000C0);

    function getCleaveCounter(address counterModule) external view returns (uint64) {
        return WASM_READER.readU64(counterModule, 0);
    }
}
```

The precompile lives at a fixed Cleave-specific address (placeholder `0xC0`; actual address TBD before Phase 5 implementation lands). Reads engine tag = `0x00` from the shared KV.

# Counterarguments

## "Just use option (a) two-namespaces. It's simpler."

Option (a) keeps the two engines isolated and slaps a Merkle layer on top. Sure, "one state root" is true. But the original `#19` requirement was "WASM and EVM coexist in the same state tree" — meaning real cross-engine reads, not just a shared hash. Option (a) doesn't deliver that.

The simplicity savings are also smaller than they look: option (a) still needs an address-mapping layer to bridge cross-engine reads when those are wanted, plus a way to commit both subtrees to one root. The cost between (a) and (b) is small; the value of (b) is large.

## "Why not Component Model with typed cross-engine calls?"

Component Model is the long-run direction; today's tooling does not support it well enough for production chains. v0 commits to a simpler shared-storage model. When Component Model matures, cross-engine direct fn calls become an extension; the shared KV stays underneath.

## "20-byte module addresses collide with EVM addresses. Why are we OK with that?"

The collision space is the same as EVM's EOA-vs-contract space: both are 20-byte values, both derived from hashes of structured inputs. EVM has not seen real-world collisions in 10 years; we accept the same risk here. If it ever becomes a problem, future RFCs can extend the engine tag to a domain separator.

## "Migration is a real headache once we have production chains."

True. The decision to commit to option (b) now is partly because we have NO production chains. Deferring this decision until after the first chain ships locks us into option (a) (no cross-engine reads) since changing the storage layout post-launch is expensive. Better to pay the design cost now than the migration cost later.

# Migration path

- **Existing v0.3 runtime**: in-memory `HashMap` storage gets replaced with the shared KV. Existing tests (counter MVP, EVM tests) keep passing because the hostcall surface is unchanged.
- **`runtime/src/lib.rs`**: `HostState.storage` becomes a reference to the shared backend rather than its own owner. Same for `evm.rs::Evm::storage_ref`.
- **`runtime/src/evm.rs`**: REVM Database impl wraps the shared backend.
- **`runtime/tests/*.rs`**: continue to pass; the in-memory backend the tests use just gains the prefix-encoding step internally.
- **No language-level changes.** Contracts written today (WASM or Solidity) work unchanged.

# Implementation roadmap

Drops into a single implementation issue (#50):

## Phase A: Shared KV backend (~200 LoC, 1 PR)

- New trait `SharedState` with `get(key: &[u8]) -> Option<Vec<u8>>`, `set(key: Vec<u8>, value: Vec<u8>)`
- Default in-memory implementation: `HashMap<Vec<u8>, Vec<u8>>`
- Prefix-encoding helpers (engine tag + identity + key → flat bytes)

## Phase B: Wire WASM engine (~150 LoC, included in same PR)

- `HostState` gains a `shared: Rc<RefCell<dyn SharedState>>` and a `module_addr: [u8; 20]`
- `state_get` / `state_set` hostcalls route through `shared` with the prefix-encoded key
- Synthetic module address computation (keccak256 of chain_id + name + version)
- Tests confirm WASM-only counter still works; bench confirms <5-10% slowdown

## Phase C: Wire EVM engine (~200 LoC, 1 PR)

- REVM `Database` impl wraps `SharedState`
- Storage reads/writes route through the shared backend with engine tag `0x01`
- Tests confirm Solidity ERC-20 still works against the new backend

## Phase D: Cross-engine reads (~300 LoC, 1 PR)

- `extern host fn evm_storage_read(addr, key)` declaration in the stdlib (gated on RFC 0002 acceptance)
- EVM precompile `readU64(module, slot)` at the reserved address
- Integration test: a WASM module reads an EVM contract's storage and vice versa, both pulling from the same shared backend

## Total

~850 LoC compiler + runtime. Smallest of the runtime-side RFC implementations.

# Reversibility

**Low.** Once contracts depend on the unified KV's key encoding (engine tag at byte 0, module address at bytes 1-20, slot at bytes 21+), changing it requires every deployed contract to remigrate. The whole point of this RFC is to pick the encoding once.

What IS reversible:
- The synthetic-address derivation function (keccak256 of structured inputs). Could swap to a different hash; existing addresses keep working if we keep the v1 derivation around.
- The cross-engine read APIs (host function names, precompile addresses). New names can be added; old ones stay deprecated-but-supported.

What is NOT reversible:
- The byte-level key encoding. Frozen after the first chain ships.
- The choice of (b) over (a) / (c) / (d). Same.

# Decision criteria

This RFC moves from `draft` to `accepted` when:

1. No outstanding maintainer objection
2. The performance estimate is sanity-checked (bench the prefix-encoding overhead on synthetic state ops, confirm <10% before committing)
3. At least one independent review (Substrate / Ethereum L2 / Cosmos SDK background) on the cross-engine design

Target: 3-4 weeks. Faster than RFC 0001 because the scope is narrower.

# Related work

- RFC 0001 memory model: state value shapes (especially when heap-allocated types ship)
- RFC 0002 extern host ABI: the mechanism for the WASM-reads-EVM direction
- RFC 16 (closed): DataAvailabilityProtocol; commits over the same state root
- RFC 8 (closed): StateProtocol; the abstraction that the unified KV implements
- Substrate's "Tries with cross-runtime visibility" experience
- Cosmos SDK's IAVL tree commitment model
- Reth's REVM Database trait + state-proof generation

# Discussion

Comments on the tracking issue (#46). RFC stays in `draft` until decision criteria are met. The companion implementation issue (#50) tracks the code.
