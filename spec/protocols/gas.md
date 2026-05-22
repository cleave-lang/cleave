# GasProtocol

**Status:** draft. Tracking issue: [#7](https://github.com/cleave-lang/cleave/issues/7).

`GasProtocol` is the interface for metering and pricing resource consumption on a Cleave chain. The interesting design question is not "how do we charge for compute" but "what does a chain pay for, and how much of that can the compiler know before the chain runs?" The answer Cleave commits to is **multi-dimensional gas with compile-time cost declarations and runtime enforcement**.

This RFC is also where Cleave's throughput posture starts to bite. Single-dimensional gas (the Ethereum model) caps real-world TPS because the worst-case path through any opcode determines the price of every path. A chain serious about throughput cannot ship single-dimensional gas. The protocol below makes the multi-dimensional case the default.

## The protocol declaration

```cleave
protocol GasProtocol<Dims> {
    /* ---- types ---- */

    type Dims        /* the set of cost dimensions, declared by the chain */
    type Cost        /* a vector of u64 values, one per dimension in Dims */
    type Price       /* a vector of u64 values, one per dimension in Dims */
    type Fee = u128  /* the scalar charged to the payer, derived from Cost x Price */

    /* ---- effects ---- */

    effect charge(cost: Cost)
    effect refund(cost: Cost)
    effect check_budget(remaining: Cost) -> bool

    /* ---- protocol-level functions ---- */

    fn cost_of(op: Operation) -> Cost
    fn price_at(block_height: u64, state: State) -> Price
    fn fee(cost: Cost, price: Price) -> Fee
    fn budget_for(tx: Transaction, state: State) -> Cost

    /* ---- payer resolution ---- */

    fn payer(tx: Transaction, state: State) -> Address
}
```

The next sections walk through the moving parts.

## Cost dimensions

`Dims` is a type parameter to the protocol: each chain declares the set of dimensions it meters. The standard library ships two presets:

```cleave
type SingleDim   = { compute }
type Multidim    = { cpu, storage, witness }
```

Single-dim is the Ethereum-compatible default. Multidim is the Cleave-recommended default. Chains targeting zk proving or specialized hardware can declare their own:

```cleave
chain ZkRollup {
    gas: GasProtocol<{ cpu, storage, witness, proof, bandwidth }>
    ...
}
```

The compiler checks that `gas`, `cost_of`, and `charge` use the dimensions declared in the chain manifest. Cost vectors with the wrong shape are a compile-time error, not a runtime panic. This is one of Cleave's substantive throughput moves: misconfigured gas cannot brick a chain at runtime because the compiler refuses to build a chain whose gas declarations are inconsistent.

### Why multiple dimensions

A single scalar gas figure forces pessimistic pricing: every transaction is priced at the worst-case resource demand across all axes. A transaction that uses only CPU pays as if it had also written to storage and generated a witness, because the price has to cover the worst case.

Multi-dimensional gas decouples the axes. A transaction that only reads state pays the CPU dimension and the witness dimension; it does not pay for storage writes it never performs. The result, in chains that have shipped this (Solana compute units, EIP-7706 in proposal), is meaningfully higher real-world TPS at the same security and economic safety margins.

### Why these specific dimensions

The three dimensions in the default `Multidim` were chosen because they correspond to the three resources that bottleneck chains in practice:

- **cpu**: instructions executed by the VM. Scales with code size and loop counts.
- **storage**: state KV writes (and to a lesser extent, reads). Scales with the size of the chain's working set.
- **witness**: proof-system witness contribution. Scales with what a zk prover or light client must commit to. For non-zk chains this is "size of the data needed to verify the tx against the state root."

Chains with different bottlenecks add or remove dimensions in the chain manifest.

## Compile-time cost declarations

The biggest Cleave-specific move: every module declares its per-function gas cost statically, and the compiler validates that the declaration matches what the function actually does.

```cleave
module Counter {
    state count: u64

    gas increment = { cpu: 100, storage: 8 }

    fn increment() -> Result<u64> {
        count = count + 1
        Ok(count)
    }
}
```

The `gas increment = { ... }` declaration is a constraint, not a hint. The compiler statically computes an upper bound on the cost of the function body and **refuses to ship the module** if the declared cost is smaller than the actual cost.

For straight-line code this is trivial. For code with loops, the compiler requires either:

1. A static bound on the loop (e.g. `for i in 0..N` where N is a compile-time constant), OR
2. An explicit cost annotation on the loop body that compounds into the function's declared cost, OR
3. A `cost dynamic` marker on the function, which downgrades the cost to a runtime computation. Dynamic-cost functions can still ship, but they sacrifice the static-bound guarantee.

The point: a chain built from modules with static costs has predictable throughput. The block-production scheduler can fit transactions into a block based on declared costs without speculatively executing them, which is one of the main reasons Solana hits higher TPS than Ethereum at the same hardware level.

## Effects

### `charge(cost: Cost)`

Fired by the runtime when a metered operation runs. Subtracts the cost from the current transaction's budget. If the budget would go negative, the transaction aborts (clean revert, no partial state change).

### `refund(cost: Cost)`

Fired when an operation completes and returns resources (e.g. `SSTORE` clearing a storage slot, freeing storage budget). Adds to the budget. Refund amounts and rules are protocol-implementation-specific; the protocol just gives implementations a place to declare them.

### `check_budget(remaining: Cost) -> bool`

Pure query. Returns whether the remaining budget covers the given cost. Used by the runtime before firing operations whose cost is uncertain at compile time.

## Protocol-level functions

### `cost_of(op: Operation) -> Cost`

Maps a primitive operation (VM opcode, hostcall, intrinsic) to its cost vector. This is where the standard library's defaults live: `cost_of(WASM::I64_ADD) = { cpu: 1 }`, `cost_of(Hostcall::StateWrite { bytes }) = { storage: bytes, witness: 32 + bytes }`, etc. A chain can override individual op costs by providing its own `cost_of`.

### `price_at(block_height, state) -> Price`

Returns the per-unit price of each dimension at a given block. State is read so the price can be EIP-1559-style adaptive (price rises if recent blocks were full, falls if they were empty). Constant prices are also legal: `fn price_at(_, _) = { cpu: 1, storage: 100, witness: 10 }`.

### `fee(cost, price) -> Fee`

Maps a (cost, price) pair to the scalar fee charged. Default implementation: dot product, `cost.cpu * price.cpu + cost.storage * price.storage + ...`. Chains with non-linear pricing models override.

### `budget_for(tx, state) -> Cost`

Returns the maximum cost (per dimension) that a transaction is allowed to incur, given its fee declaration and current prices. Used by the runtime to reject transactions whose declared fee cannot cover their declared cost before any execution starts. This is the analog of Ethereum's `gasLimit`, generalized to multi-dim.

### `payer(tx, state) -> Address`

Returns the address charged for the transaction's fees. Default implementation: `tx.signer`. Overrides enable:

- **Account abstraction**: payer is a smart-contract account that signed off on paying
- **Sponsored transactions**: a third party (faucet, paymaster) pays for end-user txs
- **Subsidized chains**: payer is always a chain-level fund (the chain's "free tier")

Sponsored gas without compromising the static-cost guarantee is one of the things this protocol shape unblocks.

## Standard library implementations

### `SingleDim` (Ethereum-compatible)

```cleave
protocol SingleDim implements GasProtocol<{ compute }> {
    fn cost_of(op) = { compute: ethereum_opcode_cost(op) }
    fn price_at(_, s) = { compute: s.eip1559.base_fee + s.eip1559.priority_tip }
    fn fee(c, p) = c.compute * p.compute
    fn budget_for(tx, _) = { compute: tx.gas_limit }
    fn payer(tx, _) = tx.signer
}
```

### `Multidim` (the Cleave default)

```cleave
protocol Multidim implements GasProtocol<{ cpu, storage, witness }> {
    fn cost_of(op) = match op {
        WASM::I64_ADD                  => { cpu: 1, storage: 0, witness: 0 }
        WASM::I64_LOAD                 => { cpu: 2, storage: 0, witness: 8 }
        Hostcall::StateRead(n)         => { cpu: 5, storage: 0, witness: 32 + n }
        Hostcall::StateWrite(n)        => { cpu: 5, storage: n, witness: 32 + n }
        Hostcall::Hash(n)              => { cpu: 10 + n, storage: 0, witness: 32 }
        // ... etc
    }

    fn price_at(h, s) {
        /* multi-dim EIP-1559: each dim adjusts independently
         * based on observed utilization of that dim in recent blocks. */
        let cpu_util     = s.recent_blocks.avg_utilization(cpu_dim)
        let storage_util = s.recent_blocks.avg_utilization(storage_dim)
        let witness_util = s.recent_blocks.avg_utilization(witness_dim)
        {
            cpu:     adjust(s.base_price.cpu,     cpu_util),
            storage: adjust(s.base_price.storage, storage_util),
            witness: adjust(s.base_price.witness, witness_util),
        }
    }

    fn fee(c, p) = c.cpu * p.cpu + c.storage * p.storage + c.witness * p.witness

    fn budget_for(tx, s) {
        let max_total = tx.fee_declaration / tx.priority_price
        /* distribute across dims by declared ratio, or equal split */
        { cpu: max_total * tx.cpu_ratio, storage: ..., witness: ... }
    }

    fn payer(tx, _) = tx.signer
}
```

### `NoGas` (subsidized chains / devnets)

```cleave
protocol NoGas implements GasProtocol<{ compute }> {
    fn cost_of(_)          = { compute: 0 }
    fn price_at(_, _)      = { compute: 0 }
    fn fee(_, _)           = 0
    fn budget_for(_, _)    = { compute: u64::MAX }
    fn payer(tx, _)        = tx.signer
}
```

Useful for devnets, testnets, and chains where the operator subsidizes all execution.

## Design choices and rationale

### Multi-dim by default

Single-dim gas (Ethereum) is the historically dominant choice and arguably the largest single source of mainnet TPS ceiling for L1s. The protocol surface accommodates SingleDim as a compatibility implementation, but the standard library default is Multidim. Chains opt into single-dim explicitly, not by accident.

### Compile-time cost declarations

The biggest Cleave-specific feature, and the one that's hardest to retrofit later. Forces module authors to think about cost when they write the code, and gives the runtime predictable scheduling material. The cost of this choice: harder to write functions with unbounded loops. The reward: chains that don't have to speculatively execute transactions to schedule blocks.

### Static + dynamic split

Not every function can have a static cost (anything reading off-chain data, anything iterating over user-supplied lists). `cost dynamic` is the explicit escape hatch. The compiler tracks which functions are static vs dynamic, so a chain can refuse to admit dynamic-cost modules at the manifest level if it wants strict predictability.

### Cost dimensions as a type parameter

Different chains have different bottlenecks. A zk-rollup cares about witness size in a way an EVM-compatible chain does not. Locking the dimensions at the protocol level (rather than allowing per-chain customization) would either force every chain to carry irrelevant dimensions or force fork-style customization. Making `Dims` a type parameter is the unification.

### `cost_of` reads `Operation`, not the calling function

Pricing decisions live on opcodes, not call sites. The same opcode costs the same wherever it runs. This keeps the cost model auditable: a chain's `cost_of` table is a finite list of opcode-cost pairs, not a recursive function over the program.

### Payer is a function over `(tx, state)`

The simplest representation of sponsored gas, account abstraction, and subsidized chains. Default implementation returns `tx.signer`; overrides do the rest. No special "paymaster module" needed.

### Refunds are an effect, not a function

Refunds tend to be the source of consensus bugs (the SELFDESTRUCT refund saga on Ethereum). Modeling them as a runtime effect means refund logic lives in one place (the protocol implementation's effect handler) rather than spread through opcode-level pricing.

## Performance considerations

The 10k TPS target shapes this protocol more than any other. Specific decisions:

- **Multi-dim is the default.** Single-dim caps throughput; the standard library defaults to multi-dim and chains have to opt into single-dim explicitly.
- **Compile-time cost declarations.** A scheduler that can fit transactions into a block without speculative execution is dramatically faster than one that has to simulate each tx to learn its cost. This is the gas-side analog of why ahead-of-time WASM compilation is faster than interpreters.
- **`cost dynamic` is explicit.** A chain can refuse to admit modules with dynamic cost at the chain-manifest level (`chain Strict { gas: Multidim<strict_static_only=true> ... }`). This is the throughput-conscious choice for production chains.
- **Pricing is a function**, allowing implementations to do per-dim EIP-1559 adjustment in parallel without serializing on a single global price counter.

## Open questions

1. **Static-bound enforcement granularity.** Does the compiler reject a module whose declared cost is too low, or does it reject a chain whose modules collectively exceed a per-block budget? Probably both, but the wording in the spec is not yet settled.
2. **Refund visibility.** When does a `refund` effect run, relative to the originating `charge`? Within the same tx, or batched at tx end? Affects whether out-of-budget reverts can be saved by a later refund.
3. **Cross-VM cost normalization.** When a Cleave WASM module calls an embedded EVM contract, do both share the same cost vector or do they have separate budgets? Depends on the EVM module RFC ([#19](https://github.com/cleave-lang/cleave/issues/19)).
4. **Multi-dim mempool ordering.** A scalar price gives the mempool a natural sort order. Multi-dim does not. What's the priority function? Likely a per-dim weighted sum, but the weights need a story.

Discussion lives on issue [#7](https://github.com/cleave-lang/cleave/issues/7).
