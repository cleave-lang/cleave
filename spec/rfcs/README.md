# Cleave RFCs

This directory holds Cleave's RFC (Request for Comments) documents. RFCs are how we propose, discuss, and record substantial changes to the language, runtime, and ecosystem before they land in code.

If you have an idea that's small and uncontroversial (a one-line grammar fix, a bug, a new test), open a normal issue or PR. If it's load-bearing and reversible only with a hard fork (memory model, ABI, consensus interface), it's an RFC.

## What gets an RFC

Anything that satisfies one or more of:

- Changes the language surface (grammar, types, semantics)
- Changes a stable ABI (hostcall surface, WASM module shape, storage layout)
- Changes the standard-library protocol contracts (consensus, gas, state, da, exec)
- Imposes a new build-time or runtime-time requirement on chain operators
- Locks in a decision that's expensive to revisit (security model, gas accounting, determinism guarantees)

If you're unsure, ask in the tracking issue. Better to over-RFC than to ship a load-bearing decision without one.

## How to open one

1. **Open a tracking issue** in `cleave-lang/cleave` with the `design` label and `RFC: <short title>` as the title. Describe the problem in 2–4 paragraphs and link to prior art if any.
2. **Discuss informally** in the issue thread to confirm the problem is real and the RFC route is the right shape.
3. **Open a PR** adding a file to this directory: copy `0000-template.md` to `NNNN-short-slug.md` (next available number). The PR body should link back to the tracking issue.
4. **Review** happens on the PR. The author keeps a `Status: draft` frontmatter line until the conversation reaches a decision.
5. **Acceptance**: a maintainer marks the RFC `Status: accepted`, merges the PR. Implementation work then references the RFC by number.

A first-time author can skip step 3 if they're not comfortable with the PR workflow; we'll happily co-author from issue text.

## Lifecycle

| Status | Meaning |
|---|---|
| `draft` | Proposed, under discussion. The default for any new RFC. |
| `accepted` | Approved by maintainers. Implementation can begin. The RFC file is the source of truth; tracking-issue discussion may continue. |
| `implemented` | Code shipped. RFC stays in the repo as historical record. |
| `superseded` | Replaced by a later RFC. Frontmatter cites the successor. |
| `rejected` | Decided against. Stays in the repo as historical record so the same proposal does not re-emerge without context. |

State transitions happen via a PR that updates the frontmatter. No automation.

## Frontmatter format

Every RFC starts with YAML frontmatter:

```yaml
---
rfc: 0001
title: "Memory model for Cleave (ownership, GC, escape hatches)"
status: draft
authors: ["Cleave Labs"]
tracking: https://github.com/cleave-lang/cleave/issues/42
created: 2026-05-23
---
```

Fields:

- `rfc`: zero-padded sequence number, matches the filename
- `title`: human-readable, can change as the proposal evolves
- `status`: one of the values in the lifecycle table
- `authors`: list of names or handles; not enforced
- `tracking`: link to the GitHub issue that hosts discussion
- `created`: ISO-8601 date
- `superseded_by` / `supersedes`: optional, when applicable
- `implemented_in`: optional, link to merge commit / release tag once shipped

## File layout

```
spec/rfcs/
  README.md                              # this file
  0000-template.md                       # copy this for new RFCs
  NNNN-short-slug.md                     # one file per RFC
```

`spec/protocols/` continues to hold the standard-library protocol specs (consensus, gas, state, da, effects). Those are different: they're stable interface documents, not proposals. An RFC that proposes a NEW stdlib protocol goes through this directory first; once accepted and implemented, the stable spec lands in `spec/protocols/`.

## Numbering

Strict monotonic. Pick the next available number when you open the PR. If two PRs race, the later one rebases.

## What an RFC is not

- **Not a binding spec.** The RFC describes intent. The compiler and runtime are the spec. If they diverge, the RFC documents what we meant and the code documents what we shipped; reconciling them is its own follow-up.
- **Not a roadmap.** RFCs can sit in `draft` for months. Acceptance does not commit to an implementation timeline; that's tracked on the implementation issues.
- **Not a vote.** Maintainers make the final call. The RFC process is for surfacing arguments, not counting noses.

## Related

- [`spec/grammar.ebnf`](../grammar.ebnf): reference grammar
- [`spec/abi/wasm.md`](../abi/wasm.md): hostcall ABI
- [`spec/protocols/`](../protocols/): stable standard-library protocol specs
- [`spec/effects.md`](../effects.md): effect system reference
