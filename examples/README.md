# Examples

These files demonstrate Cleave syntax. **None of them compile yet**; the compiler is still being built (see the roadmap in the project root). They exist so you can read what a Cleave program looks like, and so the syntax has somewhere to live while the language is still being designed in public.

If you spot something that reads wrong or could be tighter, open an issue.

## Files

| File | What it shows |
|---|---|
| [`minimal-chain.cv`](minimal-chain.cv) | The smallest interesting Cleave program: a chain manifest with the four standard subsystems (consensus, gas, state, exec). |
| [`multi-vm-chain.cv`](multi-vm-chain.cv) | A chain that embeds EVM and SVM as execution modules alongside Cleave's native WASM. Solidity contracts deploy unmodified. |
| [`counter.cv`](counter.cv) | A contract module showing how state, gas cost, and effect signature are declared together. The simplest possible "hello world" beyond a chain manifest. |
| [`token.cv`](token.cv) | A fungible token module. Demonstrates events, gas cost per function, the `view` effect on read-only functions, and pattern matching. |
| [`custom-consensus.cv`](custom-consensus.cv) | The central demo: a custom consensus protocol expressed as a Cleave module. Application-aware leader selection, programmable slashing predicates, and conditional finality thresholds. This is the case that forces a framework fork in Substrate-style systems. |

## What's not here yet

- Full standard library reference (will live in `spec/` once it stabilizes)
- Tests against the compiler (compiler does not exist yet)
- Cross-VM call examples (Cleave module calling a Solidity contract on the same chain; requires the EVM module from the roadmap)
