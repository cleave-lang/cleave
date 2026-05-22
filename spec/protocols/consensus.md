# ConsensusProtocol

**Status:** draft. The signature here matches the current intent and has not yet been compiled. Tracking issue: [#6](https://github.com/cleave-lang/cleave/issues/6).

`ConsensusProtocol` is the single most important protocol in the Cleave standard library. It is the demo case for the seam-first thesis: consensus implementations differ wildly (Tendermint, HotStuff, Narwhal-Bullshark, novel research designs), but every chain framework before Cleave forced a fork the moment a project needed a consensus shape the framework's authors had not anticipated. This RFC defines an interface broad enough that those four examples, plus future research, all satisfy it without modification to the language.

## The protocol declaration

```cleave
protocol ConsensusProtocol {
    /* ---- effects produced or consumed during a round ---- */

    effect propose_block(state: State, mempool: Mempool) -> Block
    effect attest(block: Block) -> Attestation
    effect collect_attestations(block: Block) -> AttestationSet
    effect finalize(block: Block, attestations: AttestationSet)
    effect emit_slashing_evidence(evidence: Evidence)

    /* ---- protocol-level functions ---- */

    fn select_proposer(epoch: Epoch, state: State) -> Address
    fn finality_threshold(block: Block, state: State) -> Stake
    fn validator_set(state: State) -> ValidatorSet

    /* ---- extension points ---- */

    slash_on { type: Evidence, when: fn(State, Evidence) -> bool, penalty: fn(State, Evidence) -> Penalty }
}
```

The next sections explain each piece and the rationale for each design choice.

## Effect surface

Six effects, declared at the protocol level. Each effect is a side-channel that consensus implementations produce or consume. The runtime supplies handlers; the consensus protocol itself is pure with respect to state mutation.

### `propose_block(state, mempool) -> Block`

Fired by the selected proposer for the current epoch when its turn comes. The handler reads pending transactions from the mempool, applies them speculatively against current state, and returns a candidate block. Pure with respect to durable state; persistence happens at `finalize`.

### `attest(block: Block) -> Attestation`

Fired by every validator that wants to vote for a proposed block. The handler inspects the block, validates it against local state, and returns a signed attestation if it passes. Validators that abstain or vote against do not fire this effect.

### `collect_attestations(block: Block) -> AttestationSet`

Fired by the proposer (or the next block's proposer, depending on protocol) to gather signed attestations from the network. Returns the set that satisfied the protocol's collection rule. The collection rule is internal to the implementation: round-based BFT collects until threshold, DAG-based protocols collect references to a quorum of predecessor blocks.

### `finalize(block: Block, attestations: AttestationSet)`

Fired when a block has accumulated enough attestations to be considered final. Persists state changes, advances the chain head, and unblocks any waiting queries. No return value: failure to finalize is a panic, since the upstream protocol logic should have refused to fire it on an under-attested block.

### `emit_slashing_evidence(evidence: Evidence)`

Fired by any validator that observes evidence of misbehavior (double-sign, equivocation, invalid attestation, application-level rule violation). The runtime collects evidence, evaluates `slash_on` predicates, and applies penalties at the next epoch boundary. Evidence types are open: protocol implementations can define new ones.

### What's deliberately absent

- No `vote` distinct from `attest`. The two are the same operation in this surface. Implementations that distinguish prevote / precommit (e.g. Tendermint) collapse them into one `attest` effect with phase information carried in the `Attestation` payload.
- No `gossip` effect. Networking is below the consensus protocol; the runtime provides it as ambient infrastructure.
- No `view_change` effect. View changes are an implementation detail of leader-based protocols; DAG-based protocols do not have them at all. Implementations that need view changes handle them internally without making the language model them.

## Protocol-level functions

Three functions every implementation must provide.

### `select_proposer(epoch: Epoch, state: State) -> Address`

Returns the address of the validator authorized to propose the next block (or epoch's blocks, for protocols that propose multiple blocks per epoch). The function reads `State` so the proposer selection can depend on **arbitrary application state**, not just consensus-internal data. This is the axis that forces a framework fork in Substrate-style systems: the staking module is privileged and its API is fixed. In Cleave the validator set and its weights are ordinary state, and the protocol function reads it like any other code does.

Implementations:

- Round-robin: `fn select_proposer(epoch, _) = validator_set[epoch.index % len]`
- Stake-weighted VRF: `fn select_proposer(epoch, s) = vrf_select(epoch.seed, weighted_by_stake(s.validators))`
- Useful-work weighted: `fn select_proposer(epoch, s) = vrf_select(epoch.seed, weighted_by_attestation_history(s.validators))`

### `finality_threshold(block: Block, state: State) -> Stake`

Returns the stake required to finalize a given block. **A function, not a constant.** The threshold can depend on block contents and current state. This unblocks several common research patterns:

- Two-thirds for high-value transactions, simple majority for routine ones (per-block-type threshold)
- Higher threshold during emergency / fork periods (state-conditional threshold)
- Adaptive threshold based on observed network health

The default implementation is the BFT constant: `fn finality_threshold(_, s) = two_thirds(validator_set(s))`.

### `validator_set(state: State) -> ValidatorSet`

Derives the active validator set from current state. **Derived state, not a privileged subsystem.** The chain manifest declares which state fields contribute to the validator set; the consensus protocol reads them through this function. Putting the derivation in user space (within the protocol implementation) means a chain can rotate its validator set every block, every epoch, never, or on arbitrary application-level triggers, all without compiler changes.

## Extension points

### `slash_on { type, when, penalty }`

`slash_on` is a declarative form, not an effect. It registers a (evidence type, predicate, penalty) triple with the runtime. When evidence of the matching type is observed via `emit_slashing_evidence`, the predicate runs against current state. If it returns true, the penalty function determines the consequence.

```cleave
slash_on InvalidAttestation {
    when:    fn(s, ev) -> bool { !verify_block_against_state(ev.block, s) }
    penalty: fn(s, ev) -> Penalty { Stake::burn(ev.signer, 5%) }
}
```

Programmable predicates over arbitrary state, programmable penalties over arbitrary state. The default standard library provides slash rules for double-sign and equivocation; novel chains add their own without forking the language.

### `Penalty` is an open enum

Penalties are not limited to "burn N% of stake." Implementations can define new penalty kinds: jailing, demotion, forfeiture of fees, public marking. The runtime knows how to apply built-in kinds; user kinds are applied via an effect handler.

## Worked example 1: Tendermint satisfies ConsensusProtocol

A standard library Tendermint implementation. Round-based BFT, two-thirds finality, fixed proposer rotation by stake-weighted VRF, slashing on double-sign and equivocation.

```cleave
protocol Tendermint implements ConsensusProtocol {
    state height: u64
    state round: u32

    /* effects: implementations that fire the protocol-level effects in
     * the Tendermint-specific sequence */
    fn propose_block_impl(s, m) {
        let txs = m.peek(MAX_TXS_PER_BLOCK)
        let next_state = apply(s, txs)
        Block { height: height + 1, txs, prev_root: s.root, next_root: next_state.root }
    }

    fn attest_impl(block) {
        if verify_block(block) { Some(sign_attestation(block)) } else { None }
    }

    fn collect_attestations_impl(block) {
        /* Tendermint collects in two phases: prevote then precommit.
         * Both are modeled as attestations on the same block; the phase
         * is carried in the Attestation payload. */
        gather_until(block, threshold = two_thirds(validator_set(state)))
    }

    /* protocol-level functions */
    fn select_proposer(epoch, s) {
        let v = validator_set(s)
        vrf_select(epoch.seed, weighted_by_stake(v))
    }

    fn finality_threshold(_block, s) {
        two_thirds(validator_set(s).total_stake)
    }

    fn validator_set(s) {
        s.staking.active_validators()
    }

    /* slashing */
    slash_on DoubleSign {
        when:    fn(_, ev) -> bool { ev.signer_voted_twice_in_same_round() }
        penalty: fn(_, ev) -> Penalty { Stake::burn(ev.signer, 100%) }
    }
    slash_on Equivocation {
        when:    fn(_, ev) -> bool { ev.a.height == ev.b.height && ev.a.signature != ev.b.signature }
        penalty: fn(_, ev) -> Penalty { Stake::burn(ev.signer, 100%) }
    }
}
```

This is the entire Tendermint module surface. Anything more (gossip, peer discovery, mempool ordering) is below the consensus protocol and lives in the runtime.

## Worked example 2: WeightedVRF (the demonstrative novel case)

A consensus where proposer eligibility depends on application-level history, not just stake. Slashing predicates fire on application-level evidence. Finality threshold varies with block contents.

This is the case that motivates Cleave's existence: in Substrate-style frameworks the validator set is set by a privileged staking module with a special channel into the application. In Cleave, the validator set is just state, and the protocol reads it through `validator_set(state)` like any other code.

```cleave
protocol WeightedVRF implements ConsensusProtocol {
    state useful_work_score: Map<Address, u64>

    fn select_proposer(epoch, s) {
        let weights = s.validators
            .iter()
            .map(|v| (v.address, s.useful_work_score.get(v.address).unwrap_or(0)))
        vrf_select(epoch.seed, weights)
    }

    fn finality_threshold(block, s) {
        /* High-value blocks demand supermajority; routine blocks finalize on majority. */
        match block.max_value_tier() {
            High   => two_thirds(validator_set(s).total_stake),
            Normal => majority_plus_one(validator_set(s).total_stake),
        }
    }

    fn validator_set(s) {
        s.staking.active_validators()
    }

    /* Application-aware slashing: a validator who submits a bad attestation
     * (one that fails the application-level verifier) loses 5% of stake.
     * This evidence type does not exist in Tendermint. */
    slash_on InvalidAttestation {
        when:    fn(s, ev) -> bool { !s.schemas[ev.attestation.schema_id].verify(ev.attestation) }
        penalty: fn(_, ev) -> Penalty { Stake::burn(ev.attestation.signer, 5%) }
    }
    slash_on Equivocation {
        when:    fn(_, ev) -> bool { ev.a.height == ev.b.height && ev.a.signature != ev.b.signature }
        penalty: fn(_, ev) -> Penalty { Stake::burn(ev.signer, 100%) }
    }
}
```

None of this requires a special API. The validator set, the application schemas, the useful-work score are all ordinary state. The protocol implementation reads them through ordinary functions.

## Design choices and rationale

### Effects over callbacks

Effects let the protocol declare what it produces and consumes without specifying how the runtime delivers it. Callbacks force a specific control flow (event loop, hook table) that locks out alternative implementations (parallel execution, DAG ordering). The effect system elaborates at compile time, not at runtime, so the runtime cost is zero.

### `select_proposer` reads `State`, not just `Epoch`

The PoUA-style case (useful-work weighting) needs application state. Restricting `select_proposer` to consensus-internal state would force every chain whose validator weighting depends on app-level information to fork the framework. Reading state is the unification.

### `finality_threshold` is a function, not a constant

Same reasoning, different axis. Block-type-conditional finality is a real research direction. Locking finality to a constant `2/3` is exactly the kind of "framework decided for you" choice Cleave is built to avoid.

### Validator set is derived, not privileged

In Substrate the validator set lives in a staking module with a special hook into the application. In Cleave the validator set is whatever `validator_set(state)` returns. That function is part of the protocol implementation; nothing about it is privileged. Restaking, hot-spare validators, application-conditional eligibility all become expressible by writing a different `validator_set` function.

### `slash_on` is a declarative form, not a function

A function-returning-evidence pattern works in principle but loses two things: discoverability (the runtime cannot enumerate registered evidence types) and static checking (the compiler cannot detect missing slashing rules). The declarative form is more constrained but allows tooling to reason about it.

### Penalty is open

Built-in penalties (burn, jail, freeze) cover the common cases. User-defined penalties are applied via effect handlers, which means a chain can add "send 10% to a community fund" without compiler changes.

### What this protocol does NOT specify

- **Pipelining.** Whether proposer selection runs concurrently with attestation collection from the previous block is an implementation choice. The protocol surface allows pipelined implementations; it does not require them.
- **DAG vs round-based.** A DAG protocol satisfies this surface by treating each DAG node as a "block" and using `collect_attestations` to gather quorum references rather than signed votes. The protocol is intentionally agnostic to the data structure of consensus messages.
- **Single-leader vs multi-leader.** Multi-leader protocols (Narwhal-Bullshark) fire `propose_block` from multiple validators per round. The protocol allows it; `select_proposer` becomes a set of addresses in those implementations.

## Performance considerations

Cleave's stated throughput target requires that the consensus protocol design preserve the path to high throughput. This RFC explicitly:

- Supports DAG-based protocols (no assumption that consensus is single-leader round-based)
- Supports block pipelining (no implicit ordering between `propose_block` for height N+1 and `finalize` for height N)
- Supports parallel attestation collection (`collect_attestations` is allowed to fire concurrently across blocks in flight)
- Does not assume serial execution of any phase

What the protocol does NOT mandate: a fast implementation. A naive Tendermint implementation will hit the same throughput ceiling Tendermint hits everywhere. The protocol surface lets faster implementations exist; it does not produce them automatically.

## Open questions

1. **Effect-handler composition for cross-cutting effects.** When `emit_slashing_evidence` fires from inside `attest` (a validator observes misbehavior while attesting), does the slashing handler run before or after the attestation handler completes? See effects RFC ([#9](https://github.com/cleave-lang/cleave/issues/9)).
2. **Epoch boundary semantics.** When does a validator-set change take effect: at the next block, the next epoch, or N blocks after the change is observed? Probably configurable per-implementation; needs a worked example.
3. **`Attestation` payload shape.** Phase information (prevote/precommit), aggregation support (BLS), and signature scheme are not standardized in this RFC. Likely a separate sub-protocol or a type parameter on `ConsensusProtocol`.
4. **Recovery from a stuck round.** If `collect_attestations` times out, what fires? A `view_change` effect would re-couple us to leader-based protocols; we may need a more general `escalate` effect or leave recovery as implementation-internal.

Discussion lives on issue [#6](https://github.com/cleave-lang/cleave/issues/6). This file will be revised as those questions converge.
