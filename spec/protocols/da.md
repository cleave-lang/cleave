# DataAvailabilityProtocol

**Status:** draft. Tracking issue: [#16](https://github.com/cleave-lang/cleave/issues/16).

`DataAvailabilityProtocol` is the interface for publishing block data so that any node (full, light, or stateless) can recover it. Cleave's chain manifest declares a `da:` slot so chains can choose between native DA, Celestia, EigenDA, Avail, or a future scheme without changing application code.

The DA layer is the most easily overlooked bottleneck for throughput. A chain that produces blocks faster than it can publish them stalls. A chain that publishes blocks faster than light clients can sample them sacrifices safety. This RFC defines a surface that lets implementations push hard on both throughput and sampling cost.

## The protocol declaration

```cleave
protocol DataAvailabilityProtocol<Commitment> {
    /* ---- types ---- */

    type Commitment              /* what gets stored on the consensus layer */
    type Blob                    /* the actual block data (large) */
    type ChunkProof              /* proof that a chunk is part of a Commitment */
    type SamplingChallenge       /* a request for specific chunks (used by light clients) */
    type SamplingResponse        /* answer to a SamplingChallenge */

    /* ---- effects ---- */

    effect publish(blob: Blob) -> Commitment
    effect fetch(commitment: Commitment) -> Result<Blob, FetchError>
    effect sample(commitment: Commitment, challenge: SamplingChallenge) -> SamplingResponse

    /* ---- functions ---- */

    fn commit(blob: Blob) -> Commitment
    fn verify_chunk(proof: ChunkProof, commitment: Commitment) -> bool
    fn verify_sample(response: SamplingResponse, commitment: Commitment, challenge: SamplingChallenge) -> bool
    fn challenge(commitment: Commitment, seed: Hash, count: u32) -> SamplingChallenge
}
```

## Effects

### `publish(blob) -> Commitment`

Publishes block data and returns the commitment that goes on the consensus layer. The commitment is small (typically 32-48 bytes); the blob is large (kilobytes to megabytes). Block production calls this once per block; only the commitment lands in the block header.

### `fetch(commitment) -> Result<Blob, FetchError>`

Retrieves a full blob given its commitment. Used by full nodes that need the raw data to re-execute transactions. The `Result` shape acknowledges that fetch can fail: the DA layer may experience temporary unavailability, the blob may have expired (for DA layers with pruning), or the commitment may not refer to any published blob.

### `sample(commitment, challenge) -> SamplingResponse`

Probabilistically verifies availability. A light client sends a challenge (requesting specific chunks); the DA layer returns the chunks plus proofs they belong to the commitment. Successful sampling of N random chunks gives the light client high confidence that the underlying blob is published in full, without requiring the client to download all of it. This is the operation that makes light clients viable on a high-throughput DA layer.

## Functions

### `commit(blob) -> Commitment`

Pure: derives the commitment from the blob locally. A block proposer computes this before publishing so it can include the commitment in the block header it signs. Implementations are typically `commit = KZG(blob)` or `commit = root_of_erasure_coded_merkle_tree(blob)`.

### `verify_chunk(proof, commitment) -> bool`

Independent verification that a chunk is part of the underlying blob, given just the chunk's content + a proof + the commitment. Light clients and stateless verifiers use this to validate sampling responses without trusting the DA layer.

### `verify_sample(response, commitment, challenge) -> bool`

Higher-level verification: returns whether a sampling response is valid for a given commitment and challenge. Equivalent to `response.chunks.all(|c| verify_chunk(c.proof, commitment))` plus the freshness check (the chunks correspond to the requested challenge).

### `challenge(commitment, seed, count) -> SamplingChallenge`

Deterministically derives a sampling challenge from a commitment, a seed (typically the chain's recent randomness beacon), and a count. Different light clients can derive the same challenge from the same seed, which means a successful sample by one is verifiable by others without re-doing the network round trip.

## Type parameter: `Commitment`

The commitment type varies by implementation. Standard library presets:

```cleave
type KzgCommitment      = [u8; 48]    /* G1 point on BLS12-381 */
type MerkleRoot         = [u8; 32]    /* sparse-merkle / RS-encoded merkle root */
type CelestiaNamespace  = [u8; 29]    /* Celestia namespace ID + index */
```

The chain manifest references the implementation, which determines the commitment type:

```cleave
chain Native {
    da: NativeDA<commit=KzgCommitment>
    ...
}

chain CelestiaBased {
    da: Celestia<namespace=...>
    ...
}
```

## Standard library implementations

### `NativeDA` (the Cleave-recommended default)

Erasure-coded Reed-Solomon with KZG commitments. Blob is split into chunks, encoded with rate-1/2 RS (so any half of the chunks reconstruct the original), each chunk committed via KZG. Light clients sample random chunks; with N samples the probability of missing more than half of the encoded blob is exponentially small.

```cleave
protocol NativeDA implements DataAvailabilityProtocol<KzgCommitment> {
    state recent_blobs: TimedCache<KzgCommitment, EncodedBlob>

    fn commit(blob) {
        let chunks = rs_encode(blob, rate = 1/2)
        kzg_commit(chunks)
    }

    fn verify_chunk(proof, commitment) {
        kzg_verify_opening(proof.commitment_point, proof.chunk_data, proof.opening)
    }

    fn verify_sample(response, commitment, challenge) {
        challenge.chunk_indices.iter()
            .zip(response.chunks.iter())
            .all(|(idx, chunk)| verify_chunk(chunk.proof, commitment) && chunk.index == idx)
    }

    fn challenge(commitment, seed, count) {
        let indices = (0..count).map(|i| {
            let mix = hash(seed, commitment, i.to_le_bytes())
            (u64::from_le_bytes(mix[0..8]) % NUM_CHUNKS) as u32
        }).collect()
        SamplingChallenge { commitment, chunk_indices: indices }
    }
}
```

Native DA is the option for chains that want self-sovereign data availability without bridging to a separate DA layer. The cost: the chain's validators host the blob data themselves, which scales differently than dedicated DA-layer hosting.

### `Celestia` (bridge to the Celestia DA layer)

Wraps Celestia's namespaced blob inclusion mechanism. The chain pays Celestia to publish; in exchange Celestia's nodes do the heavy lifting of sampling, storage, and serving.

```cleave
protocol Celestia implements DataAvailabilityProtocol<CelestiaNamespace> {
    state namespace: [u8; 29]

    fn commit(blob) {
        let txid = celestia_rpc::submit_blob(namespace, blob)
        CelestiaNamespace { namespace, txid }
    }

    fn verify_chunk(proof, commitment) {
        celestia_proof::verify(proof, commitment)
    }

    /* ... etc, mostly forwards to Celestia's existing primitives */
}
```

A chain wanting Celestia DA writes `da: Celestia<namespace=0xCAFE...>` in its manifest and gets the bridge for free.

### `EigenDA` (bridge to EigenLayer's DA)

Same shape as `Celestia`, different upstream. Lets chains that prefer the EigenLayer security model use it as their DA.

### `MockDA` (for testing)

```cleave
protocol MockDA implements DataAvailabilityProtocol<MerkleRoot> {
    state blobs: HashMap<MerkleRoot, Blob>

    fn commit(blob) = merkle_root(blob.chunks())
    fn verify_chunk(_, _) = true       /* trust everything */
    fn verify_sample(_, _, _) = true
    /* effects store in memory, return everything on request */
}
```

Useful for unit tests of modules that interact with DA without booting a real DA backend.

## Design choices and rationale

### Commitment as a type parameter

Different DA layers commit differently: KZG, Merkle roots, polynomial commitments, custom structures. Locking the commitment type in the protocol forces a privileged choice that doesn't survive the lifetime of a serious chain. Parameterizing it costs declaration surface and pays for itself the first time we add a new DA backend.

### Sampling is first-class

Light clients are not optional for a chain serious about long-term decentralization. Sampling is the only mechanism that lets a resource-constrained client probabilistically verify availability without downloading everything. The protocol makes sampling a first-class effect rather than an implementation detail, which means: every standard library DA implementation must support sampling, and every chain that uses a DA-conformant module gets light-client compatibility for free.

### Erasure coding is the default

Rate-1/2 Reed-Solomon is the standard tool for making sampling work: with rate-1/2 encoding, any half of the chunks reconstructs the original, so an adversary trying to withhold data must withhold at least half of the chunks. With N random samples, the probability of missing the dishonest half drops as `(1/2)^N`. The standard library's `NativeDA` defaults to rate-1/2; chains with different security/storage tradeoffs can override.

### Fetch can fail

`fetch` returns `Result<Blob, FetchError>` because DA failures are real. Some DA layers prune old blobs (Celestia after some time). Some experience temporary network partitions. Treating fetch as fallible is more honest than pretending it always succeeds, and it forces upstream code (block re-execution, light-client verification) to handle the failure case explicitly.

### `commit` is pure

A proposer must be able to compute the commitment locally before publishing, so it can include the commitment in the block header it signs. Making commit pure (no network access, no state access) keeps it usable as a building block.

### Standard library bridges to external DA layers

Celestia, EigenDA, Avail all have substantial existing infrastructure. Cleave does not try to replace them; it provides bridges so chains can use them through a uniform protocol. The bridge implementation hides the DA-layer-specific RPC behind the protocol's effect signatures. From the chain's application code, switching between Celestia and EigenDA is a one-line manifest change.

## Performance considerations

DA is one of the two scaling bottlenecks (the other being state). Specific decisions tied to the throughput target:

- **Sampling first-class.** Without efficient sampling, light clients cannot scale, and without light clients, the cost of running a node grows with the chain's throughput. The protocol makes sampling the first-class operation rather than an afterthought.
- **Erasure-coded native DA.** Native DA throughput is bounded by the chain's own validator bandwidth. Rate-1/2 RS doubles the data shipped but enables sampling, which is the only way to make light clients viable at high throughput.
- **Pure `commit`.** A proposer computing commitments serially (one block's commit waiting on the previous block's publish) caps block rate. Pure commit means the proposer can compute commitments in parallel with publication.
- **`challenge` is deterministic and pure.** Light clients can derive challenges independently from a shared seed (consensus randomness beacon) without round-tripping. This lets a network of light clients aggregate sampling without coordination overhead.

What this protocol does NOT optimize: the raw publish throughput. A chain that publishes 1 GB/sec needs hardware and bandwidth that match. The protocol shape lets implementations push to that limit; it does not magically increase available bandwidth.

## Open questions

1. **Blob lifecycle.** When can a chain prune historical blobs? Some chains may want them permanent; others want them expirable. The protocol does not yet model lifecycle.
2. **Multi-DA chains.** Can a chain use Celestia for some blobs and NativeDA for others? Likely yes if `da:` is a vector instead of a single value, but the chain-manifest grammar does not support that today.
3. **Posted-to-consensus vs separate DA.** Some designs (Ethereum blobs via EIP-4844) post blobs and consensus messages over the same network. The protocol surface is silent on this; it might need a hint so the runtime can co-batch.
4. **Reconstruction protocol.** When a node detects that too many chunks are missing, how does it reconstruct? A separate effect `reconstruct(commitment) -> Result<Blob>` might be needed. Not yet in the protocol.

Discussion lives on issue [#16](https://github.com/cleave-lang/cleave/issues/16).
