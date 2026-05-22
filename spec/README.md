# Cleave language reference

The formal specification of the Cleave language. Lives in `spec/` so the source of truth is in the same repository as the compiler that implements it. Grammar is in EBNF; protocol specs (when they land) will be in markdown referencing the grammar.

## Status

The spec evolves alongside the compiler. Sections marked **stable** match what the compiler currently parses and accepts; sections marked **draft** are in active design and may change. Sections marked **planned** have no implementation yet and the wording is intent, not commitment.

| Section | Status | Notes |
|---|---|---|
| [Grammar (chain block)](grammar.ebnf) | stable | What `cleavec --ast` parses today |
| [Glossary](glossary.md) | stable | Project terminology |
| Protocols: Consensus | draft (issue [#6](https://github.com/cleave-lang/cleave/issues/6)) | RFC in progress |
| Protocols: Gas | draft (issue [#7](https://github.com/cleave-lang/cleave/issues/7)) | RFC in progress |
| Protocols: State | draft (issue [#8](https://github.com/cleave-lang/cleave/issues/8)) | RFC in progress |
| Protocols: DataAvailability | draft (issue [#16](https://github.com/cleave-lang/cleave/issues/16)) | RFC in progress |
| Effect system | draft (issue [#9](https://github.com/cleave-lang/cleave/issues/9)) | RFC in progress |
| Module bodies (state, gas, fn, effect) | planned | Parser does not yet handle these |
| Type system | planned | No type checker exists yet |
| ABI (WASM hostcalls) | planned | Codegen lands in v0.3 |
| Standard library reference | planned | First impls land in v0.2 |

## Versioning

The spec versions independently of the compiler. Each `## [X.Y.Z]` heading in `CHANGELOG.md` notes the spec version it ships against. Breaking spec changes bump the spec major version. The current spec snapshot corresponds to **compiler v0.1.0**.

## Reading order

1. [Glossary](glossary.md) for terminology.
2. [Grammar](grammar.ebnf) for what the parser accepts.
3. The protocol RFCs above as they land in `protocols/`.

## Contributing changes

The spec is a working document. PRs that change syntax, semantics, or protocol contracts should explain the motivation in the description, point to the corresponding compiler change (if any), and update the **Status** table above. Discussion lives on the matching RFC issue.
