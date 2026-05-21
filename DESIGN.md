# Design

This document explains why Cleave is shaped the way it is. It is reading material for anyone deciding whether to contribute, and the architecture brief for anyone implementing parts of the language. It is not a tutorial.

The compiler does not exist yet. Treat what follows as a snapshot of intent, not a guarantee of behavior. The `git log` of this file is authoritative.

## What Cleave is

A programming language for building blockchains. Not a smart-contract language that runs on someone else's chain. A language whose primitives are the components of a chain itself: consensus, gas, state, mempool, execution. Each is a value of a known type, declared in source, compiled into a runnable node.

## The problem

Every existing chain framework (Substrate, Cosmos SDK, Sovereign SDK) converges on the same shape. Write a state-transition function in a general-purpose language, plug into a fixed consensus protocol, accept a fixed validator-set abstraction, accept a fixed mempool, accept a fixed gas-pricing scheme. The framework's authors decided how each subsystem behaves. The application author works inside those decisions.

This is fine until a project needs a behavior the framework did not anticipate. The standard response is to fork the framework, edit its source, and ship a divergent codebase. Forks accumulate. Drift accumulates. Each chain ends up its own dialect. The "chain framework" abstraction breaks down precisely at the point where novel chain design becomes interesting.

Cleave's premise is that this is a language-design problem, not a framework-engineering problem. The seams between subsystems need to be expressive. The subsystems themselves can stay boring.

## The thesis

**The language's primitives are the protocols between subsystems, not the implementations of those subsystems.**

A "consensus protocol" in Cleave is not Tendermint. It is the signature of any consensus: given a validator set, a pending tx set, and prior state, produce a block, a finality proof, and optionally slashing evidence. Tendermint is one implementation. HotStuff is another. Narwhal-Bullshark is another. A novel protocol someone invents tomorrow is another. Any of them can be plugged into a chain by satisfying the protocol.

The compiler checks the protocol boundary at compile time. The rest of the chain (state, execution VM, mempool) does not care which consensus implementation is in use, as long as the protocol is satisfied.

This is not novel as a PL idea. ML modules, Rust traits, and Haskell type classes all express it. What is novel for the chain domain is treating *every* subsystem this way, including the ones that today's frameworks treat as fixed.

## Design moves

### 1. Protocols, not implementations

Each subsystem has a single protocol declaration in the standard library:

```cleave
protocol ConsensusProtocol {
    effect propose_block(state: State) -> Block
    effect attest_vote(block: Block) -> Signature
    effect finalize(block: Block)
    effect slash(evidence: Evidence)

    fn select_proposer(epoch: Epoch) -> Address
    fn finality_threshold(block: Block) -> Stake
}
```

A chain references *protocols*, not implementations:

```cleave
chain MyChain {
    consensus: WeightedVRF
    gas:       Multidim<{cpu, storage, witness}>
    state:     SparseMerkle<key=u256, hash=Blake3>
    exec:      WasmVM<deterministic>
}
```

Standard library modules (Tendermint, HotStuff, Aura) satisfy the same protocols. Substituting one for another is a one-line change.

### 2. Algebraic effects for cross-cutting concerns

Cross-cutting concerns (logging, slashing, finality, gas charging) are expressed as effects, not as runtime callbacks or middleware. A protocol declares which effects it produces and consumes. The runtime supplies handlers. Adding a new effect category (for example, cross-chain message emission) does not require changing the consensus protocol. A new effect is a new declaration, satisfied by a handler module that the chain manifest pulls in.

This is the modern PL answer to "how do you let users extend the language without touching the compiler." Languages that have committed to algebraic effects (Koka, Effekt, Unison) demonstrate that they scale further than typeclasses or trait inheritance.

### 3. No assumed control flow

Existing frameworks assume a particular pipeline: collect txs into a mempool, run consensus, finalize a block, execute the STF, emit events. The moment you want to reorder this (Solana-style parallel pipelining, Narwhal-Bullshark where consensus and mempool are the same protocol, MEV-Boost where block construction happens off-protocol) the framework breaks.

Cleave does not assume a pipeline. The chain manifest declares which subsystem handles which effect; the runtime composes them according to the chain's own declarations. If your consensus and your mempool are the same module (DAG-based protocols do this), that is a manifest you can write.

### 4. Programmable predicates

Slashing predicates, finality thresholds, and leader selection are functions the protocol declares, not enums the framework offers. The slashing predicate for "submitted contradictory votes" is a function `(state, evidence) -> Option<Penalty>`. Anyone can register new evidence types and new predicates by adding modules. The consensus protocol does not need to change.

A consequence: slashing predicates can read arbitrary state. There is no firewall between application state and consensus state. Whether this is safe depends on the predicate. It is the protocol author's job to write predicates that terminate and that do not allow malicious validators to construct exploitive evidence.

### 5. State as shared, not partitioned

Existing frameworks treat consensus state (validator sets, staking balances) as a privileged module with a special channel into the application. Cleave treats it as ordinary state. The validator set is a value derivable from the application state. The chain manifest declares the derivation function. A chain whose validator set depends on arbitrary application state (token holdings, oracle prices, attestation history) can be expressed without a framework escape hatch, because there is no framework, only a language.

## What this looks like in practice

See [`examples/custom-consensus.cv`](examples/custom-consensus.cv) for the concrete demo. A consensus where the proposer weight depends on application history, slashing predicates fire on application-level evidence, and the finality threshold is a function of block contents. None of which is expressible in current frameworks without a fork.

Then look at [`examples/multi-vm-chain.cv`](examples/multi-vm-chain.cv) for a chain that runs EVM, SVM, and WASM contracts side by side. The chain is the boundary; the VM is one of several interpreters of the same shared state.

## What this is not

- **Not a smart-contract language.** Cleave compiles to multiple targets (WASM for the embedded VM, eventually native for the chain binary). Smart-contract modules are one kind of Cleave code, but the language is also for the chain itself.
- **Not an L2 framework.** Cleave outputs a sovereign chain. Using Cleave to build a rollup is plausible (the chain manifest declares a `da:` slot for the data availability layer) but is not the default shape.
- **Not magic.** A novel consensus protocol still has to be designed and proved correct by humans. Cleave makes it possible to *express* such protocols. The hard work of designing them remains hard.
- **Not finished.** The compiler does not exist yet. The standard library does not exist yet. The protocol declarations above are the design we are working toward. None of this is shipped.

## Open questions

These are unresolved as of v0.0.1. We are interested in collaborators on any of them.

- **Effect-handler composition.** When two subsystems declare overlapping effects (e.g. consensus and execution both want to fire `emit_event`), how does the compiler decide which handler runs first? Algebraic-effect literature has several proposals. We have not chosen.
- **Cross-VM call semantics.** When a Cleave module on the WASM VM calls a Solidity contract on the embedded EVM, what is the cost model? Reentrancy semantics? Gas budgeting? This is a design question that gets answered when the EVM module lands.
- **Bootstrap path.** The v0.1 compiler is in C. The v0.4 milestone is self-hosting. In between, what is the minimum standard library needed to write the self-hosted compiler in Cleave itself? Almost certainly a smaller subset than the full language. How small is open.
- **Determinism guarantees.** A WASM module compiled by an untrusted source could in theory exploit floating-point quirks or memory ordering to produce nondeterministic output. The standard library will need to enforce determinism either via a verified compile pipeline or via runtime checks. We have not picked.

## Influences

- **Move** (Aptos / Sui) for resource-linear types and formal verification posture.
- **Sovereign SDK** for the multi-VM-on-shared-state architecture.
- **Substrate** as the framework whose rigidity motivated this project.
- **Koka, Effekt, Unison** for algebraic effects as a language primitive.
- **ML's first-class modules** for the protocol-substitution pattern.
- **MLIR** for the extensible-dialect approach to compiler IR.

If you have worked on chain frameworks and felt the friction this document describes, or you have worked on PL theory and have a sharper way to express any of the design moves above, [open a discussion](https://github.com/cleave-lang/cleave/discussions).
