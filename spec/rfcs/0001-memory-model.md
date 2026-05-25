---
rfc: 0001
title: "Memory model for Cleave (ownership, GC, escape hatches)"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/42
created: 2026-05-23
---

**Status:** draft. Open for discussion before any implementation work.

Cleave has no memory model today. The current compiler / runtime sidesteps the question entirely because the language surface is so small (only integers, bool, str, char, type params, record literals; no pointers, references, or allocation primitives). That has to change before we can:

- Implement standard-library collections (\`Vec<T>\`, \`Map<K, V>\`, \`HashMap<K, V>\`) which the protocol RFCs (#6, #7, #8, #16) reference
- Compile \`Result<T>\` / \`Option<T>\` (codegen #17 deliberately rejected these as out of scope)
- Self-host the compiler (#20, the v0.4 milestone)
- Land any non-trivial application module

This RFC picks the memory model. Once picked, every contract on every chain built with Cleave depends on the choice; changing it later breaks the world. So it's worth being deliberate.

## Hard constraints

Blockchain languages have constraints that rule out most of the conventional design space:

1. **Deterministic execution.** Two nodes running the same transaction against the same state must produce byte-identical post-state. Rules out non-deterministic GC pause patterns.
2. **Gas metering.** Every allocation must charge gas with bounded worst-case cost. Rules out implementations whose cost is opaque to the gas accountant.
3. **Bounded execution.** A transaction has finite gas. Memory growth must be metered against that budget so a malicious contract cannot exhaust node memory.
4. **Persistent state round-trips.** Anything written to chain state must serialize. Rules out in-memory cycles in stored data structures.
5. **No security footguns at the language layer.** Memory corruption in shared chain state is catastrophic. One bug, billions lost. The cost of a memory bug here is not comparable to a bug in a normal program.

## Design space

| Approach | Determinism | Gas accounting | Dev ergonomics | Risk |
|---|---|---|---|---|
| **GC (mark + sweep, RC, generational)** | hard to make deterministic across nodes; pause times must be metered | gas charge per allocation is doable; gas charge per collection cycle is awkward | familiar from JS/Python/Go | implementation complexity, audit surface |
| **Linear / affine ownership (Rust, Move)** | trivially deterministic; no GC | every alloc has a known site, gas is straightforward | steeper learning curve; lifetime annotations | borrow-checker frustration; UX cost |
| **Per-transaction arena** (auto-free at tx end) | trivially deterministic | one arena = one gas allocation pool | very simple model | cannot share heap across txs; collections that outlive a tx are awkward |
| **All-by-value with copy-on-write** | trivially deterministic | gas scales with data size on every copy | simple to teach | quadratic cost on large structures |
| **Reference-counted, cycles statically prohibited** | deterministic; RC ops are bounded | gas per inc/dec | middle ground | type system has to forbid cycles, which is a non-trivial restriction |

## Recommended approach

**Linear / affine ownership, no opt-out, with a typed \`extern host\` escape hatch for raw performance.**

The reasoning:

### Why ownership, not GC

The Sui Move + Aptos Move ecosystem has demonstrated that linear / affine type systems work for smart contracts at scale, and the model maps cleanly onto deterministic gas. GC is theoretically possible (the JVM family proved that determinism is achievable with care), but it adds an audit surface that we should not pay for at v0. We are not Solidity; we do not need to inherit the EVM's "everything is a 32-byte word" model. We are not the JVM; we do not need to inherit JIT-friendly GC complexity.

The ergonomic cost of ownership is real, but on a blockchain the cost of memory bugs is far higher than the cost of a stricter type system. Stefan should treat the borrow checker as a feature, not a tax.

### Why no opt-out

\"Let me manage memory myself\" almost always translates to \"I want raw pointer arithmetic to be fast.\" In a gas-metered environment, the upper bound on that speedup is whatever gas allows. The downside is severe: memory corruption in shared chain state means an exploit pattern that no other system in the world has to defend against, because no other system has \"a million people send signed transactions to your VM\" as its threat model.

Other languages that exposed memory escape hatches:

- **C**: every CVE-2024-* shows the cost.
- **Rust \`unsafe\`**: works because Rust has external auditors and a culture of writing wrapper crates. We do not yet have that culture.
- **Solidity inline assembly**: the source of approximately every published smart-contract exploit since 2019.

The pattern that does work in a blockchain context: type-safe escape hatches at well-defined boundaries.

### What the escape hatch should be

\`\`\`cleave
extern host fn blake3(input: bytes) -> [u8; 32]
extern host fn bls12_381_aggregate(sigs: Vec<Signature>) -> Signature
\`\`\`

A declaration that says \"this function is provided by the host runtime, runs outside the WASM gas meter, has a known typed signature, and the host is responsible for implementing it safely.\" Cryptographic primitives, hash functions, signature verification, pairing operations, big-integer arithmetic. All the things people would want unsafe memory ops for in conventional languages are better served as native host functions in a blockchain.

This pattern is established: Substrate calls them \"host functions,\" Solana calls them \"syscalls,\" Cosmos SDK has \"keepers.\" Cleave should adopt it under the \`extern host\` name (or similar; bikeshed-able).

## Open questions

1. **Stack vs heap distinction.** Do small types (u64, bool, fixed-size arrays) live exclusively on the WASM operand stack? Larger types in linear memory? Cleave-managed heap on top of linear memory?
2. **String representation.** UTF-8 byte slice? Length-prefixed? Null-terminated? Borrowed-by-default vs owned?
3. **Collection types in the stdlib.** Are \`Vec<T>\`, \`HashMap<K, V>\`, \`Map<K, V>\` part of the language (built-in syntax) or stdlib types (defined in \`.cv\` once self-hosting lands)?
4. **Move vs copy semantics.** Does \`let x = y\` move \`y\` (Rust default) or copy \`y\` (most languages)? Move-by-default is the rigorous choice; copy-by-default is the gentle one.
5. **References / borrows.** Do we want a borrow checker (Rust-style) or are linear types enough (Move-style)? Linear is simpler and works for most contract patterns.
6. **Drop semantics.** When does memory get freed? End of scope? End of transaction? At an explicit \`drop()\` call?
7. **Persistent state types.** What's the contract between in-memory values and \`state\` slots? Do all serializable types go through a single trait?
8. **Cross-engine memory.** When a Cleave WASM module reads a value written by a Solidity contract, how does the type system understand the byte layout?
9. **Failure modes.** What happens on out-of-memory? Trap? Allocate-result-Result?
10. **\`extern host\` security model.** Who can declare host functions? Are they per-chain (manifest declares allowed extensions) or per-module? How is the host-function ABI versioned?

## Implementation roadmap

If we land on \"ownership + extern host,\" the work breaks into:

1. **Type system: linearity tracking.** Extend the type checker (#34 surface) with use-counting per binding. Diagnostic when a moved value is used again. Pure mechanical pass; no codegen changes.
2. **AST + grammar: \`drop\` keyword, move/borrow annotations** (if we add them). Maybe \`&T\` for borrows; maybe Move-style implicit. RFC-level decision.
3. **Codegen: heap allocation.** \`bump-allocate\` primitive in linear memory; \`drop\` emits a free or a no-op depending on allocator. Most likely choice: per-call arena that resets between calls, plus per-instance arena for state slots.
4. **Stdlib: \`Vec<T>\`, \`String\`, \`Bytes\`.** Built on the allocator. Maybe defined in \`.cv\` once self-hosting lands; until then, defined as compiler intrinsics.
5. **\`extern host\` syntax + ABI.** Grammar addition. Codegen lowers to WASM imports under a new namespace (e.g. \`host\` instead of \`env\`). Runtime exposes a registration API.

Estimate: each of (1) through (5) is its own issue, each comparable in size to v0.3's parser-extension PR. Total: ~3000-5000 LoC, several weeks of work, plus design churn from this RFC.

## Reversibility

**Low.** Once contracts ship against a memory model, changing it requires either:

- A breaking compiler version bump and a chain hard fork (every deployed contract recompiles, every state slot remigrates)
- Multi-version support in the runtime (load v1 and v2 contracts side by side, each with its own runtime semantics)

This is why the RFC matters more than feature issues. Worth absorbing several weeks of discussion before committing.

## Related work to read before commenting

- Move's \"Resource\" type and Sui's \"object-centric\" extension
- Rust's RFC #1444 (linear types attempt) and the reasoning for why borrow-checking won
- The \"What every systems programmer should know about concurrency\" line of thinking applied to multi-validator consensus
- Solidity's inline assembly + the post-mortems on every assembly-related exploit since 2019
- Substrate's host function ABI design (\`pallet-contracts\` runtime API)

## Discussion

Comment thread on this issue. RFC stays draft for at least two weeks before any \"recommended approach\" gets promoted to a binding decision.
