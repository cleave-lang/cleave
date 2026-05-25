---
title: "Cleave: a composable programming language for blockchains"
status: outline
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/67
created: 2026-05-25
target_venue: "arxiv preprint + project website"
target_date: "2026-08 (3 months out)"
target_length: "10–15 pages typeset"
---

# Status note

**This is an outline, not a draft.** Each section sketches what the final prose will argue in 2-5 bullets, names the artifacts (RFCs, code, specs) the section will cite, and lists open questions that drafting will resolve. The outline is meant to fix the paper's structure before prose work begins.

When this file moves from `status: outline` to `status: draft`, sections become full paragraphs.

---

# Abstract (placeholder)

Cleave is a programming language for building blockchains. Unlike Solidity, Cardano's Plutus, or Sui Move, Cleave is not a smart-contract language hosted by a single chain; it is the language in which the chain itself is specified. A chain manifest names protocol implementations for consensus, gas, state, data availability, and execution engine; each implementation is replaceable. Contract code targets one of several execution engines (WebAssembly, EVM) hosted by the runtime, with shared state across engines.

This paper sketches Cleave's design contributions: chain-as-composition-of-protocols, multi-VM execution, effect-typed contracts, multi-dimensional gas, sampling-first data availability. We describe the design without claiming production-scale empirical results; those land in a follow-up empirical paper once the chain layer ships.

(Final abstract: 200 words, written once the body is drafted.)

---

# 1. Introduction (~1 page)

**What this section will argue:**

- The "programming language for blockchain" framing usually refers to smart-contract languages (Solidity, Move, ink!) running on a chain someone else built. Cleave inverts this: the language IS how the chain is built.
- The chain-as-monolith approach (one consensus, one VM, one storage layer hardcoded into the framework) made sense when the design space was narrow. It does not survive a decade of diversification.
- A composable language for chains lets implementers pick consensus / gas / state / DA / execution independently rather than forking a framework.
- The paper's thesis: a typed, gas-metered, deterministic language with a chain manifest as the central abstraction is the right shape for the next generation of chain frameworks.

**Citations the section will use:**

- Substrate's "framework" pitch
- Cosmos SDK's module system
- Sui Move's resource model
- A note that this paper makes no empirical claims (those land later)

**Open questions for drafting:**

- How aggressive to be about positioning vs Substrate / Cosmos SDK? The honest answer is "we want to subsume both"; the tactful answer is "they cover overlapping but different design space." Pick a tone before drafting.

---

# 2. Background: what's broken about today's chain languages (~1.5 pages)

**What this section will argue:**

- **Solidity** is the dominant smart-contract language. Its memory model produces approximately every published exploit since 2019 (reentrancy, integer overflow before 0.8, inline assembly, etc.). The EVM is the *target*, not the language; Solidity is just the most common front-end.
- **Move (Aptos, Sui)** improved on Solidity by introducing linear resources. But Move is also tied to specific chain frameworks; you cannot "use Move's type system but my own consensus."
- **Plutus / Marlowe** (Cardano) pushed in the opposite direction — extreme formal-methods rigor — at the cost of developer accessibility and ecosystem reach.
- **Substrate / Cosmos SDK** are chain frameworks, not languages. They let chains compose modules / pallets, but the modules are written in Rust or Go and have no chain-aware type system. The frameworks impose runtime contracts that the host language does not enforce.
- The gap: a chain-aware, type-safe, deterministic, gas-metered language that targets composable execution engines.

**Citations:**

- SWC registry (smart-contract weakness classification) — concrete numbers on Solidity exploits
- Move whitepaper, Sui Move paper
- Substrate's `pallet-contracts` documentation
- Cosmos SDK's keeper / module architecture

**Open questions for drafting:**

- How much detail to give each prior system? Aim for fair and concise; this is a position paper not a survey.
- Should we cite specific exploit dollar amounts? Concrete numbers are persuasive but invite scrutiny that distracts. Probably yes for one or two examples, no for a full audit.

---

# 3. The chain manifest as the central abstraction (~1.5 pages)

**What this section will argue:**

- A Cleave chain is defined by a manifest. The manifest declares which protocol implementation provides each subsystem.
- Syntax example (already shipped in v0.3):

  ```cleave
  chain Native {
      consensus: Tendermint<sig=BLS12_381, finality=Deterministic>
      gas:       Multidim<{cpu, storage, witness}>
      state:     SparseMerkle<key=u256, hash=Blake3>
      exec:      WasmVM<deterministic>
      da:        NativeDA
  }
  ```

- The five named subsystems (consensus, gas, state, exec, da) are conventions, not parser-level keywords (per RFC #64, the grammar accepts arbitrary identifiers as subsystem keys; chains can add their own axes like `privacy: GrothProver<curve=BN254>`).
- Each subsystem is an interface (`ConsensusProtocol`, `StateProtocol`, etc.) with a contract that protocol implementations satisfy.
- Implementations live in Cleave modules and can be stdlib or third-party (RFC #65).
- The chain manifest is the contract between the chain's identity and its operational behavior. Changing it is a hard-fork-class change.

**Citations:**

- Project repo `spec/protocols/`
- RFCs #6 (consensus), #7 (gas), #8 (state), #16 (data availability), #9 (effects)
- Examples: `examples/minimal-chain.cv`, `examples/multi-vm-chain.cv` (aspirational)

**Open questions for drafting:**

- How to handle the fact that the protocols are RFC-stage, not shipped? Be honest: paint the design picture and label the parts that are not yet implemented.

---

# 4. Multi-VM execution (~1.5 pages)

**What this section will argue:**

- Most chains pick a single VM and live with the consequences (EVM lock-in for Ethereum L2s, Sui Move lock-in for Sui).
- Cleave's `exec:` subsystem treats VM choice as data, not architecture. Today's runtime hosts Wasmtime (for Cleave-compiled WASM) and REVM (for Solidity / EVM bytecode) side by side.
- Concrete demo: `cleave-run --evm <bytecode>` runs unmodified Solidity contracts on Cleave; `cleave-run module.wasm <fn>` runs Cleave-compiled modules. State is shared between them (RFC 0004).
- Shared state across engines means a chain manifest can declare `exec: WasmVM` plus deployed Solidity contracts, and both see the same Merkle-committed state.
- Honest limitation: cross-engine WRITES are deferred; the v0 design allows cross-engine READS. The empirical paper later can compare runtime cost (~10.6M ops/sec WASM hot-path, ~1.18M ops/sec EVM hot-path).
- The novelty is not "host more than one VM" (Polkadot does this); it is treating VM choice as an explicit, replaceable interface that fits inside a chain's type system.

**Citations:**

- v0.3 release notes (multi-VM benchmark numbers, both engines clearing 10K TPS minimum)
- Cleave runtime crate (`runtime/`)
- RFC 0004 (cross-engine state)
- RFC 0002 (extern host ABI, for the cross-engine read mechanism)

**Open questions for drafting:**

- How to position vs Polkadot's parachains (which also host multiple runtimes)? Polkadot's parachains are isolated; Cleave's VMs share state. Make the distinction clear.

---

# 5. Effect-typed contracts (~2 pages)

**What this section will argue:**

- Smart-contract execution involves observable effects (state reads, state writes, event emission, external calls). Existing chains type none of them; contracts that claim to be read-only might write to state silently.
- Cleave's effect system (RFC #9) types effects with row polymorphism, Koka-style. A `pure fn` cannot read state; a `view fn` can read but not write; an effectful function declares its effect set via `with [...]`.
- Compiler enforces effect signatures at every call site. A `pure fn` that calls `view fn` is rejected; type system propagates effects up the call graph.
- Handlers are monomorphized at deploy time; runtime overhead is zero compared to ad-hoc effect tracking.
- This is the first time Koka-style effects have been applied to a chain language. Move has linear resources; Solidity has `view` / `pure` annotations but no row polymorphism; ink! has function attributes; none compose like row-polymorphic effects do.

**Citations:**

- RFC #9 / `spec/effects.md`
- Koka language: Daan Leijen's "Type Directed Compilation of Row-typed Algebraic Effects"
- Effekt language for the more programmer-friendly variant
- Move's resource type for blockchain-context comparison

**Open questions for drafting:**

- How much of the effect system math do we want in the paper? Aim for "describe what programmers see + cite the formal source for those who want it." This is a position paper not a type-theory paper.
- The effect system is not implemented yet (RFC stage). Be honest about that.

---

# 6. Multi-dimensional gas (~1 page)

**What this section will argue:**

- Ethereum's gas is a single scalar. A transaction has one gas budget; every opcode debits from one bucket.
- This conflates resources that scale differently: CPU work, storage I/O, witness size, bandwidth. A transaction that's cheap in CPU but expensive in storage is priced as a generic transaction; gas pricing fights to match reality.
- Cleave's `gas:` subsystem (RFC #7) declares gas as multi-dimensional. A chain manifest declares `gas: Multidim<{cpu, storage, witness}>`; each operation declares which dimensions it charges.
- The compiler computes per-dimension cost statically where possible. The runtime enforces per-dimension budgets at execution time (already shipped in v0.4: `Instance::set_gas_budget(dimension, units)`).
- Why this matters: lets chains price storage growth (the actual scaling bottleneck) independently of compute, which is necessary if validators are going to keep their state databases bounded.

**Citations:**

- RFC #7 / `spec/protocols/gas.md`
- Ethereum Yellow Paper section on gas semantics
- EIP-1559 (the only meaningful multi-axis Ethereum gas change; only price, not budget)
- v0.4 runtime (`runtime/src/lib.rs`)

**Open questions for drafting:**

- How explicit to be about the link to actual chain economics? "Decoupling resource pricing improves validator decentralization" is a real argument but it needs care. Probably one paragraph; not the section's whole thesis.

---

# 7. Sampling-first data availability (~1 page)

**What this section will argue:**

- DA is the most easily overlooked bottleneck. A chain that produces blocks faster than it can publish them stalls. A chain that publishes blocks faster than light clients can sample sacrifices safety.
- Cleave's DA protocol (RFC #16) treats light-client sampling as first-class: `sample(commitment, challenge) -> SamplingResponse` is in the protocol interface itself, not a retrofit.
- Default stdlib implementation `NativeDA` is erasure-coded Reed-Solomon with KZG commitments. Adversaries trying to withhold data must withhold at least half of the encoded chunks; N random samples drive the missing-data probability to `(1/2)^N`.
- The protocol also defines bridges to Celestia, EigenDA, etc. so chains can use external DA without changing application code.
- The novelty: most chains have DA but few make sampling load-bearing in their protocol interface. Treating it as first-class lets light clients verify availability without trusting the DA layer.

**Citations:**

- RFC #16 / `spec/protocols/da.md`
- Mustafa Al-Bassam's PhD thesis (data availability sampling)
- Celestia's modular blockchain framing
- KZG commitments paper (Kate, Zaverucha, Goldberg)

**Open questions for drafting:**

- The connection to Stefan's Celestia conversation: name-drop or not? Probably no; the design stands on its own. Citations to the prior art are enough.

---

# 8. What we deliberately don't optimize (~0.5 pages)

**What this section will argue:**

- Cleave does NOT try to be the fastest VM. Wasmtime + REVM are fast enough; the bottleneck for real chains is consensus / commit / network, not VM dispatch.
- Cleave does NOT try to be the easiest language for "hello world." A Solidity tutorial is 5 lines; a Cleave equivalent is closer to 15. The win is at the production-scale code, not the tutorial.
- Cleave does NOT try to support every existing chain pattern unchanged. Some patterns (`unsafe` blocks, untyped events, single-axis gas) we deliberately omit because we think they're mistakes.
- This section exists because being explicit about non-goals is more honest than implying we optimize everything.

**Citations:**

- Reference v0.4 benchmarks (raw VM ops/sec vs realistic chain throughput estimates)

---

# 9. Implementation status (~1.5 pages)

**What this section will argue:**

- Cleave is not vaporware but is also not yet production. Be specific:
  - **Shipped:** Lexer, parser, type checker (with Copy classification + move tracking), WASM codegen (let, if/else, match, state ops, fn calls), runtime (Wasmtime + REVM, gas budgets, fuel metering), Solidity toolchain integration (real `solc` ERC-20 deploys), `cleave-run` CLI, tree-sitter grammar, VS Code extension.
  - **In design (RFCs accepted-ready):** memory model (RFC 0001), extern host ABI (RFC 0002), cross-engine state (RFC 0004).
  - **In design (RFCs open):** project metadata (RFC 0003), third-party protocol extension (RFC 0005).
  - **Not started:** standard-library implementations of consensus + state + multi-dim gas + NativeDA. These are the chain-layer work that turns Cleave from "language compiles + runtime runs single modules" into "chain produces blocks + commits state + sustains throughput."

- Concrete code metrics (be honest, no inflation):
  - ~3,500 LoC C compiler (`compiler/`)
  - ~1,800 LoC Rust runtime (`runtime/`)
  - 503 test assertions across 5 binaries
  - 7.9M WASM ops/sec and 1.18M EVM ops/sec on the hot path (Apple M3 Pro)
  - 5 open RFCs with 3 decision-ready, 2 still open-design

**Citations:**

- v0.3.0 release notes
- CHANGELOG
- All shipped issues + the RFC directory

**Open questions for drafting:**

- How much detail on benchmarks? Probably one table with the hot-path numbers + caveats about consensus / commit / network not being measured yet.

---

# 10. Roadmap (~0.5 pages)

**What this section will argue:**

- The next 6 months: memory model + extern host + sum types (RFC 0001 phases 1-5)
- The next 12 months: stdlib protocol implementations (consensus, state, multi-dim gas, NativeDA) — the chain-layer work
- The next 18 months: empirical paper with real chain-level throughput numbers vs Substrate / Cosmos SDK

**Citations:**

- The five v0.4 RFCs
- Open implementation issues

**Open questions for drafting:**

- How committal should the roadmap be? Honest position: "this is our intent; reality will diverge." Avoid hard dates beyond the next phase.

---

# 11. Related work (~1 page)

**What this section will argue:**

- **Move (Aptos / Sui)**: closest prior art on linear resources for smart contracts. Cleave borrows the type-safety angle but goes further on protocol composability.
- **Solidity + EVM**: the incumbent. Cleave's argument is that the language layer should not inherit the EVM's design choices.
- **Substrate (Polkadot ecosystem)**: chain framework with hot-swappable runtimes. Cleave's argument: the LANGUAGE should know what the chain is doing, not just the runtime configuration.
- **Cosmos SDK**: module-based chain framework. Cleave's argument: modules are not strongly typed against the chain's contract.
- **Plutus / Cardano**: extreme formal-methods rigor. Cleave's argument: the formal rigor is achievable without the developer-experience sacrifice.
- **ink! (Substrate's contract language)**: closest to Cleave on the "typed contract language" axis. Cleave's argument: chain-aware type system + chain-as-language framing.
- **Koka / Effekt**: source of the effect-typing techniques.
- **WebAssembly Component Model**: the long-run direction for typed cross-language interop. Cleave aligns with this.

**Citations:**

- Each system's seminal paper + ecosystem overview

**Open questions for drafting:**

- Section length depends on how rigorously we want to do the comparison. For a position paper, one paragraph each is sufficient.

---

# 12. Conclusion (~0.5 pages)

**What this section will argue:**

- Cleave's bet: chains will be built by composing protocols, not forking frameworks.
- The bet pays off if the protocol interfaces are right and the language layer is type-safe enough to make the composition work.
- Empirical validation comes in a follow-up paper once the chain layer ships (12-18 months).
- Invitation to read the RFCs, run the runtime, and engage on tracking issues.

---

# Appendices

## A. Cleave grammar summary (1 page)

Reference EBNF, condensed.

## B. Selected example modules (1-2 pages)

- Counter contract (Cleave-WASM)
- ERC-20 (Solidity, running on Cleave's REVM)
- Custom consensus protocol declaration (aspirational, illustrates the protocol shape)

## C. RFC index (0.5 pages)

Table mapping RFC numbers to titles + tracking issues + status.

---

# Drafting notes

**Tone:** Specific, opinionated, honest about limitations. Not marketing. Not academic-flowery.

**Style references:** Stripe's "Apple Pay" engineering writeup style; the original Ethereum yellow paper for technical rigor without inaccessibility; Bryan Cantrill's writeups for tone.

**Length target:** 10-15 pages typeset (single-column, two-column, or web — depends on venue). At outline level: ~80-110 lines of body per page, so ~1,200 lines of prose total.

**Process:**

1. Lock outline (this file) — ~1 day, done with this PR
2. Draft section by section over 3-4 weeks — 1 PR per section, status `outline` → `draft` per section
3. Internal review pass — 1 week
4. External review pass (target: someone at Sui Labs / Move project, someone with Substrate background, someone academic) — 2-3 weeks
5. Final polish + publish — 1 week
6. Post to arxiv + project website

**Open questions list (for the drafting phase):**

1. Tactical positioning vs Substrate / Cosmos SDK — neighborly or competitive?
2. How much formal-methods math? Lean towards "describe what programmers see + cite formal sources."
3. Concrete exploit dollar amounts in the Solidity comparison? Probably one or two examples, not exhaustive.
4. Polkadot parachain distinction — emphasize or hand-wave?
5. Roadmap commitment level — directional or hard dates? Directional.
6. Section length balance — current outline assumes 10-15 pages total; if a section grows, others shrink.

**Pre-draft action items:**

- Confirm reviewer list before opening Drafts
- Decide on a single bibliography style (BibTeX / IEEE / ACM)
- Pick a publishing tool (Pandoc / LaTeX template) and lock the format

# References (to be expanded during drafting)

Initial list of works to cite:

- Daan Leijen. "Type Directed Compilation of Row-typed Algebraic Effects." POPL 2017.
- Sam Blackshear, et al. "Move: A Language With Programmable Resources." 2019.
- Mustafa Al-Bassam. "Data Availability and Erasure Coding in Blockchain Systems." PhD thesis, UCL.
- Aniket Kate, Gregory M. Zaverucha, Ian Goldberg. "Constant-Size Commitments to Polynomials and Their Applications." ASIACRYPT 2010.
- Gavin Wood. "Ethereum: A Secure Decentralised Generalised Transaction Ledger." Yellow Paper.
- Substrate documentation.
- Cosmos SDK whitepaper.
- WebAssembly Component Model specification.
- Sui Move documentation.
- Plutus and Marlowe papers (Cardano).
- ink! documentation.

This list expands during drafting as specific claims need specific citations.
