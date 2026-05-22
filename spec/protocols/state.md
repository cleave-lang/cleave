# StateProtocol

**Status:** draft. Tracking issue: [#8](https://github.com/cleave-lang/cleave/issues/8).

`StateProtocol` is the interface for persistent chain state: storage, commitment, and proof generation. The relevant design question is whether a single protocol can accommodate sparse Merkle trees, Verkle trees, IAVL, and future commitment schemes without privileging any of them. The answer this RFC commits to is yes, with the proof type exposed as part of the protocol so light clients can verify against any backing.

## The protocol declaration

```cleave
protocol StateProtocol<Key, Value, Hash> {
    /* ---- types ---- */

    type Key             /* key space, declared by chain (e.g. u256) */
    type Value           /* value space, declared by chain (e.g. Bytes) */
    type Hash            /* commitment hash type (e.g. Blake3 output) */
    type Root = Hash     /* the commitment to the entire state */
    type Proof           /* an inclusion or exclusion proof, opaque structure */
    type Witness         /* the data a verifier needs to re-execute a tx */

    /* ---- effects ---- */

    effect read(key: Key) -> Option<Value>
    effect write(key: Key, value: Value)
    effect delete(key: Key)
    effect snapshot() -> Snapshot
    effect rollback(snap: Snapshot)
    effect commit() -> Root

    /* ---- functions ---- */

    fn prove(key: Key, root: Root) -> Proof
    fn verify_proof(proof: Proof, key: Key, value: Option<Value>, root: Root) -> bool
    fn witness_for(keys: Set<Key>, root: Root) -> Witness
    fn apply_witness(witness: Witness, ops: TxBody, root: Root) -> Root
}
```

## Effects

### `read(key) -> Option<Value>`

Returns the current value associated with a key, or `None` if absent. Reads from the in-flight transaction's view of state, which may include uncommitted writes from earlier in the same transaction.

### `write(key, value)`

Sets the value for a key in the in-flight transaction's view. Persistence happens at `commit`, not here. A write followed by a `rollback(snap)` to a snapshot taken before the write makes the write disappear.

### `delete(key)`

Removes a key from the in-flight transaction's view. Subsequent reads of the same key (within the transaction) return `None`. Delete is semantically distinct from `write(key, default_value)`: an absent key contributes nothing to the state commitment, while a present-but-default-valued key does.

### `snapshot() -> Snapshot`, `rollback(snap)`

Capture and restore the current write set. Used internally by the runtime to implement reverts: when a transaction fails, the runtime calls `rollback` to the snapshot taken before the transaction started.

### `commit() -> Root`

Finalizes all writes accumulated since the last commit. Returns the new state root: a hash that uniquely identifies the entire state. Block production calls this once per block; the resulting root goes into the block header.

## Functions

### `prove(key, root) -> Proof`

Generates a proof that `key`'s value at the given root is what it is. Proofs are opaque to the caller but encode whatever the commitment scheme needs to be verified independently. For sparse Merkle, this is a sibling-hash path. For Verkle, this is a polynomial commitment evaluation. For IAVL, this is a binary tree path.

### `verify_proof(proof, key, value, root) -> bool`

Independent verification. Given just `(proof, key, value, root)`, returns whether the proof is valid. Does not require access to the underlying storage. This is the interface a light client uses.

The verification function is a pure function. A receiver of a proof can verify without trusting the prover and without holding any state beyond the root. This is the property that makes the state commitment a useful trust-minimized object.

### `witness_for(keys, root) -> Witness`

Returns the minimal data a verifier needs to re-execute a transaction that touches a given set of keys. For sparse Merkle this is the union of sibling hashes for each accessed key; for Verkle this is a multi-evaluation proof; for IAVL this is the union of relevant tree paths.

A witness is what a stateless verifier (light client, zk prover, fraud-proof challenger) consumes. The protocol exposes it directly so the rest of the chain runtime does not need to know which commitment scheme is in use.

### `apply_witness(witness, ops, root) -> Root`

Pure re-execution. Given a witness, a transaction body, and a starting root, return the resulting root **without access to the underlying storage**. This is the "stateless re-execution" interface that zk provers, fraud proofs, and light clients all rely on.

A chain that can produce witnesses and apply witnesses can be verified by anyone with `O(witness size)` data, not `O(state size)`. This is the load-bearing property for any zk or stateless-client path.

## Type parameters

`Key`, `Value`, `Hash` are all type parameters. The standard library presets:

```cleave
type Bytes32 = [u8; 32]
type Bytes   = Vec<u8>
type Blake3Hash = Bytes32
type KeccakHash = Bytes32

type SparseMerkleU256 = StateProtocol<Key = u256, Value = Bytes, Hash = Blake3Hash>
type SparseMerkleAddr = StateProtocol<Key = Address, Value = Bytes, Hash = KeccakHash>
type VerkleU256       = StateProtocol<Key = u256, Value = Bytes, Hash = VerkleCommitment>
```

The chain manifest uses these directly:

```cleave
chain MyChain {
    state: SparseMerkle<key=u256, hash=Blake3>
    ...
}
```

A chain whose keys are typed addresses, whose values are typed structs, and whose hash is something custom declares its own state type and supplies an implementation. Nothing about the protocol assumes EVM-style 256-bit-everything.

## Standard library implementations

### Sparse Merkle

```cleave
protocol SparseMerkle<K, V, H> implements StateProtocol<K, V, H> {
    /* state internal to the backing tree */
    state tree: ParticipationTree<K, V, H>

    fn prove(key, root) {
        tree.gather_sibling_hashes(key, root)
    }
    fn verify_proof(proof, key, value, root) {
        let computed = recompute_root(proof, key, value)
        computed == root
    }
    fn witness_for(keys, root) {
        keys.iter()
            .map(|k| tree.gather_sibling_hashes(k, root))
            .union()
    }
    fn apply_witness(witness, ops, root) {
        let mut working_root = root
        for op in ops {
            match op {
                Read(k)     => witness.lookup(k),  /* read doesn't change root */
                Write(k, v) => working_root = witness.apply_write(k, v, working_root),
                Delete(k)   => working_root = witness.apply_delete(k, working_root),
            }
        }
        working_root
    }
    /* effects are wired to the in-memory tree's mutation methods */
}
```

### Verkle

```cleave
protocol Verkle<K, V> implements StateProtocol<K, V, VerkleCommitment> {
    state polynomial: VerkleTree<K, V>

    fn prove(key, root) {
        polynomial.multi_open_proof([key], root)
    }
    fn verify_proof(proof, key, value, root) {
        kzg_verify(proof, key, value, root)
    }
    fn witness_for(keys, root) {
        polynomial.multi_open_proof(keys.into_vec(), root)
    }
    fn apply_witness(witness, ops, root) {
        /* Verkle's main feature: witnesses are O(1) amortized across keys.
         * apply_witness re-evaluates the polynomial commitment after
         * incorporating each write. */
        verkle_replay(witness, ops, root)
    }
}
```

### In-memory mock

For testing and devnets:

```cleave
protocol InMemoryState<K, V> implements StateProtocol<K, V, Bytes32> {
    state map: HashMap<K, V>
    state root: Bytes32

    fn prove(_, _)            = Bytes32::default()  /* placeholder proof */
    fn verify_proof(_, _, _, _) = true              /* trust-everything */
    fn witness_for(keys, _)    = keys.iter().map(|k| (k, map.get(k))).collect()
    fn apply_witness(w, ops, _) = sequential_apply(w, ops)
}
```

The mock cannot be used for production chains (its proofs prove nothing). It exists so unit tests of modules can run without booting a real state backend.

## Design choices and rationale

### Proofs are surfaced, not opaque to the runtime

A chain can have a light client only if proofs are part of the protocol surface. Hiding them inside the implementation forces every light-client integration to reach across the abstraction. Surfacing them is more invasive on the protocol declaration but pays for itself the first time a zk path or fraud-proof system needs to consume them.

The cost: each implementation must produce a `Proof` type compatible with the chain's `verify_proof`. The benefit: a generic light client written against `StateProtocol` works on any chain whose state implements the protocol.

### Witnesses are a separate concern from proofs

A proof shows that a single key has a single value. A witness lets you re-execute an entire transaction touching multiple keys without holding the full state. Some implementations (sparse Merkle) treat proofs and witnesses as essentially the same thing (a set of sibling hashes). Others (Verkle) have a notable cost gap between single-key proofs and multi-key witnesses. Separating them in the protocol keeps the cost story honest.

### `apply_witness` is pure

A stateless verifier should be able to apply a witness to a starting root and get the resulting root, without any side effects, without access to durable storage. This is the property zk proving and fraud proofs depend on. The protocol enforces purity by making `apply_witness` a function, not an effect.

### `K`, `V`, `H` as type parameters

EVM picked 32-byte keys, 32-byte values, Keccak hashes 15 years ago and the choice has not aged well: addresses are 20 bytes wrapped to 32, modern chains have wanted Blake3 / Poseidon / Rescue hashes for years, and code is much more efficient to ship as raw bytes than as zero-padded 32-byte words. Cleave declines to bake this in. The chain declares its types.

### Reads, writes, and deletes are effects

This lets the runtime intercept them for instrumentation, gas accounting, and witness generation, without those concerns leaking into the protocol's function signatures. A `read` that the runtime tracks (for witness generation) and a `read` that the runtime ignores (during a pure simulation) look the same to the calling code.

### Snapshots are part of the protocol

Reverts are a fundamental operation. Modeling them at protocol level (rather than as an implementation detail of the runtime) means alternative implementations have the same revert semantics. A chain whose state backend cannot cheaply rollback (some external KV stores) gets to provide its own snapshot/rollback impl that buffers writes in memory.

## Performance considerations

State is one of the two most common throughput bottlenecks for high-TPS chains (the other being consensus). The protocol shape encodes several throughput-preserving choices:

- **Per-key effects rather than batch APIs as primary.** Effects let the runtime parallelize state reads and writes within a transaction when the keys are independent. A batch-only API would serialize them.
- **`witness_for` accepts a set, not a single key.** Light clients need multi-key witnesses; making the multi-key case the first-class shape avoids N round trips when one would do.
- **`apply_witness` is pure.** Stateless re-execution is required for zk paths to achieve their throughput claims. Embedding state writes inside `apply_witness` would foreclose those paths.
- **Snapshots are buffered, not durable.** Snapshot/rollback should not hit disk. The protocol does not require durable snapshots, leaving implementations free to keep them in memory.
- **Type-parameter K and V.** Smaller key/value types means smaller witnesses, smaller proofs, faster comparisons. EVM's 32-byte default is wasted bandwidth for chains that do not need it.

What this protocol does NOT promise: that any specific implementation is fast. A naive sparse Merkle that recomputes the entire tree on each write will be slow. The protocol surface lets a tuned implementation be fast; it does not force a slow one to become fast.

## Open questions

1. **Range scans.** Some workloads (indexing, iteration over typed collections) need ordered range scans over keys. A `range(start, end) -> Iterator<(Key, Value)>` effect would be natural, but range scans are very implementation-specific (sparse Merkle does not natively support them; B-trees do). Likely a separate `RangeScanProtocol` extension that a chain opts into.
2. **Async commit.** For chains targeting very high throughput, `commit` is a synchronization point. An async variant that returns a handle and lets execution continue could lift that bottleneck. The protocol does not yet model concurrency.
3. **State expiry.** Chains with strict storage growth targets need to expire old state (Ethereum's "state rent" discussions). Expiry might be modeled as a periodic `expire(predicate)` effect; not yet in the protocol.
4. **Proof composition.** When a tx reads N keys, can one combined proof cover all reads, or is it N independent proofs? Verkle gets this for free; sparse Merkle has to compose. The protocol does not yet specify whether composition is a function on `Proof` or just a property of `witness_for`.

Discussion lives on issue [#8](https://github.com/cleave-lang/cleave/issues/8).
