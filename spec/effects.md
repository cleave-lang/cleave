# Effect system

**Status:** draft. Tracking issue: [#9](https://github.com/cleave-lang/cleave/issues/9).

Cleave's effect system is the formalism that makes the seam-first thesis work. The Consensus, Gas, State, and DA protocols all declare their interfaces in terms of effects; without a defined effect system, those declarations are syntactically suggestive but not yet meaningful. This RFC defines the syntax, semantics, and composition rules for effects in Cleave.

The design draws heavily on three modern algebraic-effect languages: **Koka** (effect-row typing, handler composition), **Effekt** (capability-style effects, lexically scoped handlers), and **Unison** (abilities, no syntactic overhead at call sites). Cleave's choice is closest to Koka with one ergonomic concession from Unison (inferred effect sets by default, explicit annotation allowed).

## Declaration syntax

### Effect declaration

```cleave
effect EffectName(param: Type, ...) -> ReturnType
```

Effect declarations appear in protocol declarations and at module level. Examples already in the protocol RFCs:

```cleave
effect propose_block(state: State, mempool: Mempool) -> Block
effect charge(cost: Cost)
effect read(key: Key) -> Option<Value>
effect publish(blob: Blob) -> Commitment
```

An effect with no return type returns `()` (unit). Effects can take any number of arguments, including zero.

### Effect annotation on functions

A function's type includes the set of effects it can perform. By default the effect set is inferred from the body:

```cleave
fn transfer(to: Address, amount: u128) -> Result<()> {
    let from = tx.signer
    let bal = read_state(balance_key(from))?
    require(bal >= amount, "insufficient balance")
    write_state(balance_key(from), bal - amount)
    write_state(balance_key(to), read_state(balance_key(to))? + amount)
    emit_event("Transfer", encode(from, to, amount))
    Ok(())
}
```

The compiler infers `transfer` has effect set `[read_state, write_state, emit_event]`. Callers of `transfer` inherit this set unless they handle the effects.

Explicit annotation overrides inference (used when the author wants the function's effect set to be a hard contract, or when inference would be surprising):

```cleave
fn transfer(to: Address, amount: u128) -> Result<()> with [read_state, write_state, emit_event] {
    /* ... body ... */
}
```

### Pure and view shorthand

Two common effect sets get keyword shorthand:

```cleave
pure fn double(x: u64) -> u64 { x * 2 }
view fn balance_of(addr: Address) -> u128 { read_state(balance_key(addr)).unwrap_or(0) }
```

- `pure`: empty effect set. Function reads no state, performs no I/O, has no observable side-effects beyond its return value.
- `view`: effects are a subset of `{read_state, read_block, read_tx, read_clock}`. Function can read state but cannot mutate it or emit events.

`pure` and `view` are not just annotations; the compiler enforces them. A `view` function calling a `write_state` effect is a compile-time error.

## Handler composition

When a function uses an effect, the runtime must supply a handler. The chain manifest declares which module's handler runs for which effect:

```cleave
chain MyChain {
    consensus: Tendermint
    gas:       Multidim<{cpu, storage, witness}>
    state:     SparseMerkle<key=u256, hash=Blake3>
    exec:      WasmVM<deterministic>

    handlers {
        emit_event:    [Tendermint, Multidim, audit_log_module]
        read_state:    [SparseMerkle]
        write_state:   [SparseMerkle, gas: Multidim]
        publish:       [Celestia]
    }
}
```

The `handlers { }` block is optional. If absent, the runtime uses the default order: subsystem implementations first (consensus, gas, state, exec, da, in that order), then module-level handlers in load order.

### Multiple handlers for the same effect

When multiple modules want to handle the same effect (e.g. both `Tendermint` and a custom `audit_log_module` want to react to `emit_event`), the handlers run in the declared order. Each handler receives the original effect arguments; later handlers see the same arguments (handlers do not modify them).

For effects with return values, only the first handler's return value is used. Subsequent handlers are observers. This matches the Koka semantics: handler stacks are deepest-first for return values, all-fire for observers.

### Effect propagation across protocol boundaries

The interesting case is cross-protocol effect propagation. Example: during `attest` (a consensus effect), a validator might fire `emit_slashing_evidence` (also a consensus effect, but conceptually a separate channel). The handler for `attest` cannot block on the handler for `emit_slashing_evidence` because the latter might involve durable state writes that need to complete first.

The protocol handles this by **deferring** effects. An effect declared as `deferred` runs after the firing function returns:

```cleave
effect emit_slashing_evidence(evidence: Evidence) deferred
```

Deferred effects are batched, deduplicated, and applied at well-defined synchronization points (transaction end, block boundary, epoch boundary, depending on the effect's declaration). This avoids the "what runs first" question for effects that don't need to nest synchronously.

## Row typing

Effect sets compose using rows (Koka-style). A function with effect set `[read_state, emit_event]` has type:

```
fn(...) -> T with [read_state, emit_event | ...]
```

The `| ...` represents the open row: the function can be specialized to a larger effect set without modification. This is what makes effect typing usable in practice: a calling function with more effects can call a function with fewer effects, and the type system figures out the propagation.

Closed rows (explicit `with [...]` with no open tail) mean "exactly these effects, no more." The compiler refuses to compose them with functions that would add effects.

## Inference

By default, inference runs in two modes:

1. **Open inference** (the default): the compiler infers the smallest effect set that the function's body requires, leaving the row open so callers can be specialized.
2. **Closed inference**: explicit `with [...]` (no open tail) means "this function's effect set is exactly this." The compiler enforces it.

The user can almost always omit annotations on internal functions; the compiler infers what they need. Public protocol functions and effect declarations are the places where explicit annotations matter, because they define the contract that other code typechecks against.

## Worked example: cross-cutting `emit_event`

A chain wants every state write to also emit an audit event. The audit module declares:

```cleave
module AuditLog {
    handler write_state(key: Key, value: Value) {
        emit_event("audit", encode("write", key, value))
        forward(write_state(key, value))  /* delegate to next handler in the chain */
    }
}
```

The chain manifest:

```cleave
chain MyAuditedChain {
    state: SparseMerkle<...>
    handlers {
        write_state: [AuditLog, SparseMerkle]
    }
}
```

Now any module on this chain that performs a `write_state` triggers `AuditLog` first (which emits an audit event), then `SparseMerkle` (which actually persists the change). The module that wrote the original code never had to know about audit logging. This is the operational expression of seam-first: a cross-cutting concern is a handler module, not a fork of every module that touches state.

## Design choices and rationale

### Effect rows over capabilities

Koka uses effect rows; Effekt uses capability arguments. Capabilities are more explicit (every effect appears in the function signature as an extra argument) but heavier syntactically. Rows are implicit (callers don't pass capabilities explicitly) but require more inference. Cleave picks rows because the protocol declarations look cleaner: `effect read(key) -> Option<Value>` rather than `fn (key, state_cap: ReadCap) -> Option<Value>`.

### Inference by default, annotation when it matters

Most internal helper functions don't deserve an explicit `with [...]` annotation; the compiler figures it out. Public interfaces deserve explicit annotations because they're contracts. The user controls when to be explicit; the compiler doesn't force annotations everywhere.

### Pure and view as keywords

These two cases come up often enough that the keyword shorthand is worth it. Without them, every read-only getter function would carry a `with [read_state]` annotation, which is noise.

### Handler order in the chain manifest

The alternative would be implicit ordering (handlers run in load order, or alphabetical, or some other rule). Implicit ordering surprises authors at scale; explicit ordering in the manifest gives the chain operator one place to look. The default order (subsystems first, modules after, in load order) is sane for the simple case.

### Deferred effects

Some effects are inherently asynchronous (slashing evidence, audit log writes, event emissions to off-chain consumers). Modeling them as synchronous handlers forces the firing function to wait, which kills throughput. Deferred effects let the compiler batch and apply at safe points, gaining throughput without losing correctness.

### `forward` keyword for handler chains

When a handler wants to delegate to the next handler in the chain (the audit log example above), it uses `forward(effect_call(...))`. This is the explicit version of "call the next handler." Alternative would be implicit delegation (effects automatically propagate down the handler stack), but implicit propagation makes it hard to write handlers that intentionally swallow effects.

## Performance considerations

Effect systems are sometimes deployed with significant runtime overhead (dynamic dispatch on every effect fire). Cleave commits to the opposite end: **effects elaborate at compile time**. Each effect call site is monomorphized against the chain's declared handler stack, producing direct function calls with no runtime dispatch.

The cost: longer compile times, larger binaries (handler stacks are duplicated per call site after monomorphization). The benefit: effect calls have zero runtime overhead beyond the underlying handler function's cost. A `read_state` in user code compiles to a direct call into the `SparseMerkle::read` implementation, not a virtual dispatch through an effect table.

This is the same tradeoff Rust makes for generics, applied to effects. Recent algebraic-effect compilers (Koka with `--multi-resume`, Effekt) demonstrate that whole-program effect elaboration is feasible at the throughput levels Cleave targets.

Specific decisions:

- **No runtime effect dispatch on hot paths.** Hot paths (state read/write, gas charge) are statically dispatched after monomorphization.
- **Deferred effects are batched in memory.** Until the synchronization point fires, deferred effects accumulate without hitting durable storage.
- **Handler composition is resolved at chain-build time.** The handler stack for each effect is fixed once the manifest is loaded; the runtime doesn't re-resolve.

## Open questions

1. **Effect handler escape analysis.** If a handler captures local state from its lexical scope (Effekt's primary feature), does Cleave allow that, and how does it interact with whole-program monomorphization? Currently the RFC says handlers are top-level modules (no closure capture); this is restrictive but easier to compile.
2. **Generic effect quantification.** Functions that are polymorphic in their effect set ("works for any effect that satisfies X") need a way to say so. Likely an `effect bound` syntax similar to trait bounds; not yet specified.
3. **Effect aliases.** A chain might want a name for a common effect set (e.g. `Pure = []`, `View = [read_state, read_block]`, `IO = [emit_event, write_state, ...]`). The pure/view keywords handle the most common two; the design space for more is open.
4. **Reentrancy through effects.** A handler fires an effect that triggers itself (transitively). This is well-defined in the row-based system (the effect set just grows), but the implementation needs guards to avoid stack overflow. Concrete rules pending.

## References

- Koka language: [https://koka-lang.github.io/koka/doc/book.html](https://koka-lang.github.io/koka/doc/book.html). The closest design influence; effect rows, handlers, monomorphization.
- Effekt: [https://effekt-lang.org/](https://effekt-lang.org/). Capability-style effects with lexically scoped handlers. Cleave borrows the "handler ordering matters" semantics.
- Unison abilities: [https://www.unison-lang.org/docs/fundamentals/abilities/](https://www.unison-lang.org/docs/fundamentals/abilities/). The ergonomic argument for inferred effect sets at call sites.
- Daan Leijen, "Type Directed Compilation of Row-Typed Algebraic Effects" (POPL 2017). Foundational paper for the row-typed approach.

Discussion lives on issue [#9](https://github.com/cleave-lang/cleave/issues/9).
