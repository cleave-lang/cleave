---
rfc: 0005
title: "Third-party protocol extension story (publish, discover, audit, version)"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/65
created: 2026-05-25
---

**Status:** draft. Open for discussion.

Cleave's design promises chain composability: a chain manifest names protocol implementations for each subsystem, and any module satisfying the protocol can be plugged in. The standard library will ship `Tendermint` (consensus), `SparseMerkle` (state), `Multidim` (gas), `NativeDA` (da), and others. **The promise extends to third-party implementations too: anyone should be able to publish a `MyPoUA` consensus implementation that other chains can adopt.**

Today nothing about that workflow exists. This RFC scopes what we need.

## What this RFC covers

The end-to-end story for "I built a custom consensus protocol; another team wants to use it on their chain":

1. **Author** writes a Cleave module declaring `protocol MyPoUA implements ConsensusProtocol { ... }`
2. Author **publishes** it somewhere
3. Consumer **discovers** it
4. Consumer **adds it as a dependency** of their chain project
5. Consumer's chain manifest **references** it: `consensus: MyPoUA<...>`
6. The tooling **verifies** the implementation satisfies `ConsensusProtocol` at compile time
7. At chain launch, the runtime **loads** the implementation alongside the rest

Each step has open questions.

## Open questions

### 1. Publishing

- Git URLs (Cargo-style): `MyPoUA = { git = "https://github.com/author/myorg", rev = "abc123" }`. Pros: no registry needed, content-addressed via git SHA. Cons: discovery is hard.
- Content-addressed registry (IPFS / Sigstore / dedicated). Pros: discovery + signing built in. Cons: infrastructure to maintain.
- Dedicated registry with namespacing (npm / crates.io). Pros: human-friendly names. Cons: name squatting, governance, trust.

### 2. Trust model

A malicious consensus implementation can rewrite history. A malicious state-protocol implementation can silently lose data. A malicious DA implementation can withhold blocks. The blast radius of a bad third-party protocol module is catastrophic, far worse than a bad npm package.

Possible models:

- **Caveat emptor.** Chain operators audit before adopting. Same as today's "trust the developer" model that has produced every published smart-contract exploit.
- **Reproducible builds + signatures.** Authors sign releases; consumers pin to specific signed builds. Doesn't prevent malice but makes it attributable.
- **Trusted set.** A community-curated list of audited protocol implementations; chains can reference either trusted or arbitrary ones with a warning.
- **Formal verification gate.** Protocols ship with proofs of key properties (safety, liveness, etc.). High bar; eliminates a class of bugs but raises the cost of publication.
- **Sandboxing at runtime.** Even a malicious protocol implementation is constrained by what the runtime exposes. The host could limit syscall surface so the protocol cannot escape its lane. Works for some surface (no DB writes outside state) but not others (consensus controls block order by definition).

### 3. Version pinning + upgrade

A chain in production pins to specific protocol versions. When the author publishes an upgrade, how does the chain adopt it?

- Manual: chain operators bump version in manifest, rebuild, hard-fork
- Automatic with consent: a governance vote upgrades a soft-coded version
- Never: once deployed, never upgrade

Same questions for security patches.

### 4. Interface satisfaction checking

How does the compiler verify that `MyPoUA implements ConsensusProtocol` actually satisfies the protocol contract? Today the type checker treats `implements` as a structural assertion that the names match; it does not verify behavior.

- Structural: check method signatures match
- Semantic with annotations: protocol declares postconditions, impl asserts they hold
- Runtime conformance tests: protocol ships a test suite, impl must pass
- Formal: impl ships proofs

### 5. Multiple competing implementations

If chain A uses `MyPoUA` and chain B uses `TheirPoUA`, both implementing `ConsensusProtocol`, do they interop? Probably not at the consensus layer (each chain has its own). But what about cross-chain bridges that span both?

### 6. Deprecation + sunset

A protocol implementation might be abandoned, get superseded, or be revealed to have a bug. How do consumers know? How do they migrate?

## Strawman: minimum-viable v0

Pick the simplest answer to each question that does not foreclose better answers later:

1. **Publishing**: git URLs only; no registry
2. **Trust**: caveat emptor + reproducible builds via the Cleave build system
3. **Versioning**: pin to git SHA in the chain's project manifest
4. **Interface**: structural check on method signatures (extension of #34 typecheck)
5. **Interop**: out of scope for v0
6. **Deprecation**: signal via README and abandoned-tag conventions

This is "what Cargo does, plus content-addressed pinning, minus a registry." Most of the open questions get deferred to "v0.5+ governance" decisions.

## Reversibility

High up-front. We can ship the strawman and gather data. Each later change (add a registry, add signing, add proofs) is a strict extension.

## Related issues

- #45 (RFC: project metadata + build system) — companion. The build system needs to understand third-party protocol modules.
- #53 (stdlib: ConsensusProtocol impl) — protocol satisfaction can only be checked once a real protocol exists
- #54 (stdlib: StateProtocol impl) — same
- The grammar work to lift subsystem-key hardcoding (separate issue) — prerequisite for new third-party subsystems beyond consensus/gas/state/exec/da

## Discussion

Comment thread. RFC stays draft until the strawman either solidifies or gets replaced. No timeline; this is a long-tail design effort.

