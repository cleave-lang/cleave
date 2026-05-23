# Examples

These files demonstrate Cleave syntax. They exist so you can read what a Cleave program looks like, and so the syntax has somewhere to live while the language is built in public.

If you spot something that reads wrong or could be tighter, open an issue.

## Files

| File | What it shows | Parses with `cleavec --ast`? |
|---|---|---|
| [`minimal-chain.cv`](minimal-chain.cv) | The smallest interesting Cleave program: a chain manifest with the four standard subsystems (consensus, gas, state, exec). | yes |
| [`counter.cv`](counter.cv) | A contract module showing how state, gas cost, and a public `fn` are declared together. The simplest possible "hello world" beyond a chain manifest. | yes |
| [`multi-vm-chain.cv`](multi-vm-chain.cv) | A chain that embeds EVM and SVM as execution modules alongside Cleave's native WASM. Solidity contracts deploy unmodified. | no (requires nested-block subsystem syntax not yet in the grammar) |
| [`token.cv`](token.cv) | A fungible token module. Demonstrates events, gas cost per function, the `view` modifier on read-only functions, and pattern matching. | no (requires unit type `()` and struct-literal shorthand not yet in the grammar) |
| [`custom-consensus.cv`](custom-consensus.cv) | The central demo: a custom consensus protocol expressed as a Cleave protocol declaration. Application-aware leader selection, programmable slashing predicates, and conditional finality thresholds. | no (requires closure syntax `|v| ...` not yet in the grammar) |

The "no" rows are intentional. They document the syntax we are working toward; the grammar additions that unlock them are tracked under future issues against the parser.

## What's not here yet

- Full standard library reference (will live in `spec/` once it stabilizes)
- End-to-end compile + run tests (codegen + runtime are the next milestone)
- Cross-VM call examples (Cleave module calling a Solidity contract on the same chain; requires the EVM module from the roadmap)
