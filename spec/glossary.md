# Glossary

Terms used across the Cleave codebase, documentation, and design discussions. Where a term has a precise technical meaning in Cleave that differs from its general usage, this file is the authoritative reference. Definitions are short by design; deeper explanations live in `DESIGN.md` and the per-protocol specs.

## Chain

A complete distributed agreement about state transitions, packaged as a runnable node binary. In Cleave, a chain is the value produced by a `chain { ... }` declaration. One chain manifest, one node binary, one running network.

## Chain manifest

The top-level `chain Name { ... }` block. Declares which protocol each subsystem satisfies (consensus, gas, state, execution, optionally data availability). The smallest interesting Cleave program is a chain manifest with four lines.

## Subsystem

One of the named components a chain manifest configures: `consensus`, `gas`, `state`, `exec`, or `da`. Each subsystem is a protocol slot. The value assigned to it must satisfy that subsystem's protocol.

## Protocol

A named interface that any number of implementations can satisfy. The standard library defines protocols for each subsystem (e.g. `ConsensusProtocol`, `GasProtocol`). Third-party modules and user code can also define protocols. A protocol is the seam at which Cleave makes substitution easy; see `DESIGN.md` for why this is the load-bearing idea.

## Module

A named unit of code containing state, gas declarations, functions, and effect declarations. Declared with `module Name { ... }`. Modules are deployable units that run on a chain (analogous to a smart contract, but the term "contract" is avoided in Cleave; the unit of deployment is the module).

## Effect

A typed side-channel that a function can produce or consume: `emit_event`, `read_state`, `gas_charge`, etc. Effects are declared in protocol signatures and elaborated by the compiler at compile time, not interpreted at runtime. Cleave's effect system is the formalism that makes the seam-first architecture work.

## Handler

The implementation that runs when an effect fires. The chain manifest declares which handler module owns which effects. The same protocol can have different handlers in different chains.

## Predicate

A function that returns a boolean given some inputs. Predicates appear in slashing rules (`when` clauses), finality conditions, and protocol-level invariants. Cleave's design treats predicates as ordinary functions rather than enum cases, which is what lets new slashing conditions ship without recompiling the framework.

## Primitive type

A built-in scalar: `u8`, `u16`, ... `u256`, `i8` ... `i256`, `bool`, `Address`, `Bytes`, `Hash`. The set is intentionally small; user code defines structs and protocols on top.

## Derived state

State whose value is a pure function of other state. The validator set, for example, can be declared as derived state of staking balances rather than a separate top-level value. The chain manifest declares the derivation function.

## Type parameter

An entry inside `< ... >` on a type expression. Can be positional (`Result<u64>`) or keyword (`Tendermint<sig=BLS12_381>`). Type parameters can carry types, identifiers, numbers, or set literals.

## Set literal

A brace-delimited list inside a type expression: `{cpu, storage, witness}`. Represents a multi-dimensional record of named axes, used most often in multi-dimensional gas declarations. In the AST it's a `TypeGeneric` with empty name.

## Source span

A line + column range attached to every AST node, used for error messages and tooling. The lexer produces 1-based line and column numbers; the parser propagates them as nodes are constructed.

## Cleavec

The Cleave compiler binary. The C bootstrap is at `compiler/`. A future self-hosted compiler (issue [#20](https://github.com/cleave-lang/cleave/issues/20)) will replace it.

## Bootstrap compiler

The C11 implementation of the compiler used to produce the first usable artifacts. Retires when the self-hosted compiler is stable. Lives in `compiler/` for now; will move to `legacy/` post-v0.4.

## ABI

The contract between compiled Cleave code (WASM bytecode) and the host runtime. Defines hostcall signatures for state access, gas accounting, event emission, and so on. Documented in `spec/abi/wasm.md` (planned, lands with codegen in v0.3).

## Manifest grammar

The subset of the language that describes chains (as opposed to module bodies). Currently the only grammar the parser implements. See `grammar.ebnf`.

## Pinned identifier

A reference to a type, protocol, or module by its fully-qualified name (e.g. `std::consensus::Tendermint`). In the AST this is currently represented as a chain of `ExprBinary` nodes with `::` operators; a future spec change may introduce a dedicated `PathExpr` node.

## Slashing

The mechanism by which validators lose stake when they misbehave. Defined per-consensus-protocol via `slash_on` declarations. Predicates are arbitrary functions over evidence and state, which is the axis that makes Cleave's consensus protocols more expressive than fixed-enum frameworks.

## v-prefixed versions

Releases are tagged as `vX.Y.Z`. Prose references write the version with the `v` (e.g. "v0.1.0 ships the lexer"). Headings in `CHANGELOG.md` use the bare form `[0.1.0]` per Keep-a-Changelog convention. The two styles coexist intentionally.
