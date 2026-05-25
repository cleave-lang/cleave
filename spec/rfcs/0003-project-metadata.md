---
rfc: 0003
title: "Project metadata format and build system for Cleave projects"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/45
created: 2026-05-25
---

Open question from the project's earliest design conversations. Every developer-facing language needs an answer for:

- How do I declare a project?
- How do I declare dependencies?
- How do I build it?
- Where do dependencies come from?

This RFC lays out the design space.

## Status

Draft. Open for discussion.

## Constraints unique to a chain language

1. **Reproducibility is non-negotiable.** Two builds of the same source from the same dependencies must produce byte-identical WASM. A chain depends on it.
2. **Dependencies are part of the security model.** A compromised dependency means a compromised contract means user funds at risk. Higher bar than `npm install`.
3. **No global mutable registry of human-readable names.** A name like `cleave-stdlib` is too valuable a target. Either content-addressed (IPFS-style) or namespaced under verified publishers (Maven-style).

## Strawman

A `Cleave.toml` (or `cleave.toml`) at the project root:

```toml
[project]
name = "my-token"
version = "0.1.0"
authors = ["alice@example.com"]
license = "Apache-2.0"

[chain]
manifest = "chains/mainnet.cv"

[dependencies]
stdlib = { content-hash = "blake3:abc..." }
my-utility = { git = "https://github.com/...", rev = "..." }
```

`cleavec build` reads it, resolves deps, compiles every `.cv` in `src/`, emits artifacts to `build/`.

## Questions

1. **TOML or something else?** TOML is the default but Cleave isn't dogmatic about it. We discussed this very early in the project; the conversation didn't conclude.
2. **Lockfile.** Committed `Cleave.lock` with content-addressed pins for reproducibility?
3. **Workspace.** Multi-package projects? Sub-package dependencies?
4. **Build artifacts.** Per-module `.wasm` per chain? One bundle? Cross-chain compile?
5. **Build script.** Hook for codegen / asset processing during build (like Rust's `build.rs`? Probably no, since determinism is paramount.
6. **Package registry.** Necessary? Or do we stay git-only forever?
7. **Vendoring.** Inline-copy dependency source for audit? Default-off, opt-in?
8. **Conditional compilation.** Per-chain features (e.g. EVM-enabled vs Wasm-only)? Or always-compile-everything?

## Related work

- Cargo (Rust): the closest reference
- Move's 
- Solana's 
- Foundry's 
- Hardhat's 

## Out of scope for this RFC

- A package registry implementation (its own RFC; needed only if we go that route)
- IDE integration (LSP, project discovery)
- CI templates

Discussion: comment thread. RFC stays draft for at least two weeks.
EOF
)
