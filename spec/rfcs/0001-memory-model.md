---
rfc: 0001
title: "Memory model for Cleave (ownership, GC, escape hatches)"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/42
created: 2026-05-23
updated: 2026-05-25
---

# Summary

**Linear / affine ownership with borrow checking, no `unsafe` opt-out, typed `extern host` for native primitives.** Move semantics by default; primitives are implicit `Copy`. References (`&T`, `&mut T`) for non-owning access, scope-local lifetimes only in v0. RAII-style drop at end of scope. Stack for primitives and small fixed-size arrays; linear-memory heap (per-call bump arena) for everything else. Strings are length-prefixed UTF-8 byte slices, owned `String` vs borrowed `&str`. Collections (`Vec`, `HashMap`, `Map`) are stdlib types with compiler-known intrinsics until self-hosting lands.

This RFC promotes from open design to a specific proposal. If accepted, every contract on every chain built with Cleave depends on it; the cost of getting it wrong is high. Open questions reduced from 10 to 3.

# Motivation

Cleave has no memory model today. The compiler / runtime sidesteps the question because the language surface is so small (only integers, bool, str, char, type params, record literals; no pointers, references, or heap allocation). That has to change before we can:

- Implement standard-library collections (`Vec<T>`, `Map<K, V>`, `HashMap<K, V>`) which the protocol RFCs (#6, #7, #8, #16) reference
- Compile `Result<T>` / `Option<T>` (codegen #17 deliberately rejected these)
- Self-host the compiler (#20, the v0.4 milestone)
- Land any non-trivial application module

Picking now lets the next ~3,000-5,000 LoC of compiler / runtime work proceed with confidence. Picking late means rewriting the type checker and codegen mid-stream.

# Hard constraints

Blockchain languages have constraints that rule out most of the conventional design space:

1. **Deterministic execution.** Two nodes running the same transaction against the same state must produce byte-identical post-state. Rules out non-deterministic GC pause patterns.
2. **Gas metering.** Every allocation must charge gas with bounded worst-case cost. Rules out implementations whose cost is opaque to the gas accountant.
3. **Bounded execution.** A transaction has finite gas. Memory growth must be metered against that budget so a malicious contract cannot exhaust node memory.
4. **Persistent state round-trips.** Anything written to chain state must serialize. Rules out in-memory cycles in stored data structures.
5. **No security footguns at the language layer.** Memory corruption in shared chain state is catastrophic. One bug, billions lost. The cost of a memory bug here is not comparable to a bug in a normal program.

# Design space

| Approach | Determinism | Gas accounting | Dev ergonomics | Risk |
|---|---|---|---|---|
| **GC (mark + sweep, RC, generational)** | hard to make deterministic across nodes; pause times must be metered | gas charge per allocation is doable; gas charge per collection cycle is awkward | familiar from JS/Python/Go | implementation complexity, audit surface |
| **Linear / affine ownership (Rust, Move)** | trivially deterministic; no GC | every alloc has a known site, gas is straightforward | steeper learning curve; lifetime annotations | borrow-checker frustration; UX cost |
| **Per-transaction arena** (auto-free at tx end) | trivially deterministic | one arena = one gas allocation pool | very simple model | cannot share heap across txs; collections that outlive a tx are awkward |
| **All-by-value with copy-on-write** | trivially deterministic | gas scales with data size on every copy | simple to teach | quadratic cost on large structures |
| **Reference-counted, cycles statically prohibited** | deterministic; RC ops are bounded | gas per inc/dec | middle ground | type system has to forbid cycles, which is a non-trivial restriction |

# Recommended approach

**Linear / affine ownership with borrow checking, no opt-out, typed `extern host` escape hatch for raw performance.**

## Why ownership, not GC

The Sui Move + Aptos Move ecosystem has demonstrated that linear / affine type systems work for smart contracts at scale, and the model maps cleanly onto deterministic gas. GC is theoretically possible (the JVM family proved that determinism is achievable with care), but it adds an audit surface that we should not pay for at v0:

- Every GC implementation has had real-world correctness bugs (V8 GC bugs are a regular source of Chrome CVEs). The audit surface is a recurring tax.
- GC pause distribution affects validator timing. Even a deterministic GC produces variable latency under load, which interacts badly with consensus timeouts.
- Gas accounting for "the collection cycle that just freed 1000 objects" is harder to make worst-case-bounded than "this allocation freed when its scope ended."
- Move and Sui Move have shown that smart-contract programmers can be productive with linear types. The ergonomic gap to GC is real but smaller than the Solidity-vs-Rust gap that's already been crossed.

The ergonomic cost of ownership is real, but on a blockchain the cost of memory bugs is far higher than the cost of a stricter type system. Treat the borrow checker as a feature, not a tax.

## Why no `unsafe` opt-out

"Let me manage memory myself" almost always translates to "I want raw pointer arithmetic to be fast." In a gas-metered environment, the upper bound on that speedup is whatever gas allows. The downside is severe: memory corruption in shared chain state means an exploit pattern that no other system in the world has to defend against, because no other system has "a million people send signed transactions to your VM" as its threat model.

Other languages that exposed memory escape hatches:

- **C**: every CVE-2024-* shows the cost.
- **Rust `unsafe`**: works because Rust has external auditors and a culture of writing wrapper crates. Cleave does not yet have that culture and chain contracts have less leeway for "the wrapper is sound, trust me."
- **Solidity inline assembly**: the source of approximately every published smart-contract exploit since 2019.

The pattern that does work in a blockchain context: type-safe escape hatches at well-defined boundaries. That's what `extern host` is.

## What `extern host` looks like

```cleave
extern host fn blake3(input: &[u8]) -> [u8; 32]
extern host fn ec_recover(message: [u8; 32], v: u8, r: [u8; 32], s: [u8; 32]) -> Address
extern host fn bls12_381_aggregate(sigs: &[Signature]) -> Signature
```

A declaration that says "this function is provided by the host runtime, runs outside the WASM gas meter, has a known typed signature, and the host is responsible for implementing it safely." Cryptographic primitives, hash functions, signature verification, pairing operations, big-integer arithmetic. All the things people would want unsafe memory ops for in conventional languages are better served as native host functions in a blockchain.

Established pattern: Substrate "host functions," Solana "syscalls," Cosmos SDK "keepers." Detailed ABI is RFC `0002-extern-host-abi.md`; this RFC just commits to the shape.

# Concrete syntax

What does Cleave look like with ownership? Concrete examples to ground discussion.

## Today (no memory model)

```cleave
module Counter {
    state count: u64

    fn increment() -> u64 {
        let prev = count
        let next = prev + 1
        count = next
        next
    }
}
```

Primitives only. Nothing on the heap. This continues to work unchanged.

## With ownership (proposed v0.4+)

```cleave
module Token {
    state balances: Map<Address, u128>
    state total_supply: u128

    // String is owned (heap-allocated), returned by-value.
    // The caller takes ownership.
    fn name() -> String {
        String::from("Cleave Token")
    }

    // &str is a borrow, no allocation. Caller already owns the bytes.
    fn check_symbol(s: &str) -> bool {
        s == "CLV"
    }

    // `to: Address`: Address is Copy (it's a [u8; 20] alias), passed by value.
    // `amount: u128`: also Copy. Passed by value.
    // `name: String`: NOT Copy. Caller's `name` is moved into this fn.
    fn mint(to: Address, amount: u128, name: String) -> Result<u128, Error> {
        // `balances` is state; reading borrows it temporarily.
        // We cannot hold `balances.get(&to)` across the .insert call
        // (the borrow checker requires the read to end before the write).
        let prev = balances.get(&to).unwrap_or(0u128)
        let next = prev + amount
        balances.insert(to, next)
        total_supply = total_supply + amount

        // `name` is moved into Ok; it lives inside the returned value.
        // After `Ok(name)` runs, `name` is gone in this fn.
        emit Mint { to, amount, name }
        Ok(next)
    }
}
```

Key things to read in this example:

- **`Address` and `u128` are `Copy`.** Passing them by value is cheap; the original keeps existing. The compiler auto-derives `Copy` for primitives and aggregates of primitives.
- **`String` is NOT `Copy`.** Passing it by value moves it. The caller's local is no longer usable. The borrow checker flags `let n = name; use(name);` as an error.
- **`&str` and `&[u8]` are borrows.** They do not transfer ownership; they're a pointer + length pair that lives as long as the lender's value lives. Scope-local lifetimes only in v0 (no explicit `'a` annotations).
- **State access through borrows.** `balances.get(&to)` returns an immutable borrow of the value; `balances.insert(...)` mutably borrows the whole map. The borrow checker rejects code that tries to do both at once.
- **Move into return position.** `Ok(name)` is the moment `name` leaves the function. After this statement, it's gone (and the runtime can free its heap memory).

## Move semantics, made explicit

```cleave
fn use_name(n: String) { ... }

fn caller() {
    let n = String::from("hello")
    use_name(n)      // n is moved into use_name
    // use_name(n)   // compile error: n was already moved
}
```

Versus the `Copy` case:

```cleave
fn use_id(id: u64) { ... }

fn caller() {
    let id = 42u64
    use_id(id)       // id is COPIED (u64 is Copy)
    use_id(id)       // OK: id is still here
}
```

## Borrows in the wild

```cleave
fn longest<'_>(a: &str, b: &str) -> &str {
    if a.len() > b.len() { a } else { b }
}
```

The `'_` is a placeholder lifetime. For v0 we infer all lifetimes from scope; the user never writes explicit `'a` annotations. This is a v0 simplification; richer lifetime ergonomics are a follow-up.

## RAII drop

```cleave
fn process() {
    let buf = Vec::with_capacity(1024)
    // ... use buf ...
    // end of scope: buf is dropped, its 1024-byte allocation is freed
}
```

No `free`, no `delete`. Drop is automatic at end of scope. Custom drop logic via `impl Drop for MyType { fn drop(&mut self) { ... } }`.

# Resolutions to open questions

The original RFC listed 10 open questions. This revision proposes a position on each. Open questions become **DECIDED** (with rationale) or stay **OPEN** (with a description of what would resolve them).

## 1. Stack vs heap distinction — DECIDED

- **Primitives** (u8..u256, i8..i64, bool, char): operand stack (WASM native).
- **Fixed-size arrays `[T; N]`**: stack for small N (`size_of::<T>() * N <= 64`), heap otherwise. Cutoff lifts later if profiling shows it's tight.
- **Records / structs**: heap (linear memory).
- **References (`&T`, `&mut T`)**: stack — represented as a 32-bit linear memory offset.
- **`String`, `Vec<T>`, `HashMap<K,V>`**: heap, with a fat-pointer (offset + length + capacity) on the stack.

Rationale: matches Rust's model and WASM's strengths. The stack is fast and bounded; the heap handles everything dynamic.

## 2. String representation — DECIDED

- **Encoding**: UTF-8 enforced (compile-time literal validation; runtime validation on construction from `&[u8]`).
- **Layout in memory**: length-prefixed, not null-terminated. `(usize length, *const u8 data)` for `&str`; `(usize length, usize capacity, *mut u8 data)` for owned `String`.
- **Default ownership**: literals (`"hello"`) are `&'static str`. Conversion to owned via `String::from(...)`.
- **Indexing**: only by byte offset, not by code-point (consistent with Rust's choice; UTF-8 indexing is a footgun).

## 3. Collection types — DECIDED (with implementation note)

- `Vec<T>`, `HashMap<K, V>`, `Map<K, V>`, `BTreeMap<K, V>`, `String` are **stdlib types**, not language built-ins.
- They have **special compiler knowledge** in v0.4-v0.5 so they can be implemented before self-hosting (#20) lands. The compiler emits direct hostcalls for `Vec::push`, `HashMap::insert`, etc.
- Once self-hosting lands, the stdlib gets reimplemented in `.cv` using the lower-level memory primitives the language exposes by then.
- Built-in syntax: `[T; N]` for fixed-size arrays; `&[T]` for slices. No `[T]` syntax for `Vec` (use `Vec<T>`).

## 4. Move vs copy semantics — DECIDED

- **Move by default** for non-Copy types (Rust semantics).
- **Primitives are auto-`Copy`** (u8..u256, i8..i64, bool, char, fn pointers, immutable references `&T`).
- **Aggregates of `Copy` types are `Copy`** if the user marks them so (`#[derive(Copy)]`), opt-in.
- **User-defined types are not `Copy` by default** unless they only contain `Copy` fields and the user opts in.

Rationale: copy-by-default makes large struct copies invisible in source. Move-by-default forces the cost into source where it can be audited. Primitives stay copy-cheap because they always were.

## 5. References / borrows — DECIDED

- Yes. Both `&T` (immutable / shared) and `&mut T` (mutable / exclusive).
- **Aliasing rule**: one mutable XOR any number of shared, never both. Classic Rust.
- **Lifetimes**: scope-local only in v0. The compiler infers; the user never writes `'a`. Functions cannot store borrows across calls (no returning a borrow that outlives a parameter, beyond simple identity cases).
- **No reborrowing or lifetime polymorphism in v0.** Add when actual code patterns demand it.

Rationale: borrowing is the entire ergonomics win over Move's pure-linear model. Without it, every "read this value" requires a costly clone. The simplification (no explicit lifetimes) is a real bet on what v0 code patterns look like.

## 6. Drop semantics — DECIDED

- **End of scope.** RAII pattern.
- **Implicit drop** for types with no special needs (`Vec`, `String`, primitives etc. all get default drop = "release my heap memory").
- **`impl Drop for MyType`** for custom cleanup. Called at end of scope after the value's other fields are dropped.
- **No `defer` or explicit drop block** in v0. The shape exists in some languages (Zig `defer`, Go `defer`) but Rust has demonstrated that RAII is sufficient for the use cases we need.

## 7. Persistent state types — DECIDED

- A `Serialize` trait that types implement. State slots store the serialized byte representation.
- **Built-in `Serialize` impls** for: primitives, `String`, `Vec<T: Serialize>`, `HashMap<K: Serialize, V: Serialize>`, fixed arrays, tuples of `Serialize` types.
- **User types** opt in via `#[derive(Serialize)]` on the type definition.
- **Cycles in serializable data are rejected** at compile time. (Combined with our type system, this is enforceable.)
- **Versioning** of stored data is the chain's responsibility (storage layout changes between contract upgrades are out of scope for this RFC; tracked separately).

## 8. Cross-engine memory — DEFERRED to RFC 0004

The cross-engine state sharing RFC (`0004-cross-engine-state.md`) handles the question of how WASM and EVM values interoperate. This RFC commits to: anything that crosses the WASM↔EVM boundary serializes to a canonical byte representation. WASM doesn't see EVM's storage layout directly; bridge code marshalls.

## 9. Failure modes — DECIDED

- **Default allocation**: allocation failure traps the call.
- **Fallible allocation**: opt-in via `.try_*` methods (`Vec::try_with_capacity`, `String::try_with_capacity`) that return `Result<T, OutOfMemory>`.
- **OOM is treated as out-of-gas**: the runtime maps an allocator-side failure to the same trap-code the gas-budget hostcall raises. Chain logic above sees an "out of gas" transaction, not an "out of memory" one.

Rationale: defaulting to trap matches Rust 1.0's choice and avoids `Result<_, OutOfMemory>` noise on every allocation site. Fallible alloc exists for the rare case where graceful degradation matters.

## 10. `extern host` security model — DECIDED (in this RFC), full ABI in RFC 0002

- **Allow-list per chain**, declared in the chain manifest:
  ```cleave
  chain MyChain {
      ...
      host_extensions: [Blake3, Bls12_381, EcRecover]
  }
  ```
- A module that imports an `extern host fn` not in the chain's allow-list is **rejected at module load time**, before any execution.
- **Default stdlib set**: cryptographic primitives only. New host functions added by community RFC.
- **Versioning**: by namespace. `host_v1.blake3` and `host_v2.blake3` can coexist; the chain manifest pins specific versions.
- **Determinism**: host functions are required to be deterministic. The runtime's reference implementations are; chains that register custom host functions are responsible for that property. RFC 0002 details enforcement.

# Counterarguments

Steelmanning positions this RFC rejects, with responses.

## "GC is fine. Solana's Sealevel uses BPF, JVM-derived chains exist. Why pay the ownership tax?"

The languages-on-GC chains that have shipped well (Solana SBPF programs in Rust, Aptos / Sui in Move) have *not* used GC. Solana programs use Rust ownership (the Rust toolchain is the language; SBPF is just the backend). Aptos and Sui use Move's resource model, which is closer to linear ownership than to GC. The chains that nominally have GC (Ethereum smart contracts in Solidity have no explicit memory model; they hand-allocate in storage) have a far worse security track record than the ownership-typed ones.

Where GC chains have won is the LLVM-class JIT engines (V8 isolates, JVM-on-the-fly), and those are explicitly not the target Cleave is aiming at.

## "Borrow checker frustration is real. Devs will fork the language to remove it."

Possible but unlikely. The same fear applied to Rust at v1.0 and the borrow checker stuck. Move shipped with linear types and has not seen a "Move-without-resources" fork take off. The frustration is real but the trade is good enough.

If frustration becomes a real adoption problem, the response is *better diagnostics and a culture of patterns*, not removing the type system. This is a long-tail UX problem, not a language-design problem.

## "Why not let me write a Cleave program that's just C++ with extra steps?"

In a normal application, C++ is one option among many and bugs are local. In a blockchain context, every contract is publicly accessible code that adversaries actively look at, every function call comes from someone who might be hostile, and every memory bug is a potential billion-dollar exploit. The threat model is uniquely hostile and the cost-of-bugs uniquely high. C++-class memory management is the wrong tool, the same way C++-class memory management is the wrong tool for an aircraft control system.

## "Cleave is supposed to be fast. Ownership is slower than 'let the GC figure it out.'"

Empirically false. Rust outperforms most GC'd languages on most workloads, including allocation-heavy ones, because the allocator knows when memory is freed and can reuse it predictably. The slow case for ownership is "many small short-lived allocations," and Cleave's per-call bump arena handles exactly that case well.

Where ownership is genuinely slower than GC is "highly concurrent shared data," because the borrow checker forces synchronization to be explicit. That cost is irrelevant in a single-threaded deterministic VM execution model.

## "Won't this slow down the team? You should just pick GC for speed-of-shipping."

The team's bottleneck is not the type system, it's the chain-layer work (#53 consensus, #54 state, etc.). Picking GC saves zero time on the chain layer and costs us the entire security envelope. The trade isn't worth it.

# Migration path

What changes for code that exists today?

- **Existing `examples/*.cv` and `examples/counter-mvp.cv`**: nothing. They use only primitives (`u64`, etc.) which stay `Copy` and are stack-allocated. Programs compile unchanged.
- **The compiler / runtime**: significant. Type checker gains linearity tracking; codegen gains heap allocation + reference lowering; runtime gains an allocator and a memory pool per call.
- **`spec/protocols/*.md`**: references to `Vec<T>`, `HashMap<K,V>`, `Bytes` etc. become honest, not aspirational.
- **`spec/abi/wasm.md`**: extended with the memory-layout conventions (fat pointer format for `String` / `Vec` / slice, etc.).

No deployed contracts to migrate (we have no production chains yet). This RFC happens cleanly. If we waited until after a real chain ran, we'd have to break it.

# Implementation roadmap

If accepted, work breaks into:

## Phase 1: Type system extensions (~700 LoC, 1 PR)

- Linearity tracking in the type checker. Annotate every let-binding with a `moved` flag.
- Diagnostic when a moved value is used again.
- `Copy` trait detection: built-in for primitives, opt-in via `#[derive(Copy)]` for user types.
- Borrow tracking: shared vs exclusive, scope-local lifetimes inferred.

Lands as an extension to `compiler/src/typecheck.c`.

## Phase 2: Grammar + AST (~400 LoC, 1 PR)

- `&T` / `&mut T` type expressions
- `#[derive(...)]` attribute syntax on type definitions
- `extern host fn` declarations (parsed and registered; runtime wiring is Phase 5)
- `impl Trait for Type { ... }` blocks for traits + impls (needed for `Drop`, `Serialize`, `Copy`)
- Tree-sitter grammar updated in lockstep

## Phase 3: Codegen + heap allocation (~1200 LoC, 1-2 PRs)

- Per-call bump arena: WASM linear memory, allocator state in a known offset.
- Heap allocation primitives: `alloc(size, align) -> *mut u8`, `free(ptr, size, align)` (the bump allocator's free is a no-op until end-of-call).
- Codegen for owned values: emit alloc on construction, follow lifetime to emit drop at end-of-scope.
- Codegen for borrows: pass pointer + length as i32 pairs on the WASM stack.

## Phase 4: Stdlib types (~900 LoC, 1 PR)

- `String`, `Vec<T>`, `Bytes`, `HashMap<K, V>`, `Map<K, V>` implemented as compiler-known intrinsics
- `Serialize` trait + `#[derive(Serialize)]` for the state-slot ↔ value bridge
- `Drop` trait + default-derive

## Phase 5: `extern host` ABI (~500 LoC, 1 PR; per RFC 0002)

- Grammar already in Phase 2; codegen lowers calls to imports under `host_v1.*` namespace
- Runtime exposes registration API
- Chain manifest `host_extensions:` allow-list parsing
- Module-load-time validation

## Total

~3,700 LoC across 5 PRs, plus 800 lines of new docs/spec. Two months of focused work at v0.4-shipped pace.

# Reversibility

**Low.** Once contracts ship against a memory model, changing it requires either:

- A breaking compiler version bump and a chain hard fork (every deployed contract recompiles, every state slot remigrates)
- Multi-version support in the runtime (load v1 and v2 contracts side by side, each with its own runtime semantics)

This is why the RFC matters more than feature issues. The proposal in this revision aims to spend the cost once and well.

# Remaining open questions

Reduced from 10 to 3:

1. **Explicit lifetime annotations** (`'a`): never, or in v1 once code patterns prove they're needed? Leaning "never" since most blockchain code is shallow.
2. **Custom allocators**: should contracts be able to opt into a different allocator (e.g., a slab allocator for hot collections)? Leaning "no" for v0; revisit if profiling demands it.
3. **`async` / coroutines**: not in this RFC, but the memory model interacts with them (async fns need self-referential state, which fights with ownership). Punt to a separate RFC once async is even on the table.

# Decision criteria

This RFC is ready to move from `draft` to `accepted` when:

1. No outstanding objection from a maintainer
2. At least one independent review by someone familiar with Rust + a smart-contract language (Move, Solana Rust, ink!)
3. A skim by someone at Sui Labs or the Move project to flag obvious mistakes

If those three happen and no new red flags surface, we accept. Target: 4-6 weeks from this revision being merged.

If the answer is "no" or "not yet," the most likely alternative path is per-tx arena (option C from the design space) as a stopgap, deferring full ownership until v1.

# Related work

- Move's "Resource" type and Sui's "object-centric" extension — closest blockchain prior art
- Rust's RFC #1444 (linear types attempt) and the reasoning for why borrow-checking won
- Koka and Effekt for the effect-typing connection (since effects are RFC #9 / `spec/effects.md`)
- Cyclone (predecessor to Rust) for region-based memory in a typed setting
- Solidity inline assembly + the post-mortems on every assembly-related exploit since 2019 — what we are avoiding
- Substrate's host function ABI design (`pallet-contracts` runtime API)
- WebAssembly Component Model — the long-run industry direction for typed host-function boundaries

# Discussion

Comments on the tracking issue (#42). Substantive changes via PRs against this file. The RFC stays in `draft` status until the decision criteria above are met.
