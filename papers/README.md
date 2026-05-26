# Cleave papers

This directory holds writeups about Cleave's design and implementation that are intended for an external audience: researchers, engineers at competing chain projects, funders, future contributors deciding whether the design is interesting.

Distinct from:

- [`spec/rfcs/`](../spec/rfcs/) — RFCs are proposals for changes. Audience: maintainers + active contributors.
- [`spec/protocols/`](../spec/protocols/) — stable specs for shipped interfaces. Audience: implementers.
- This repo's `README.md` — the project's surface-level story. Audience: anyone landing on GitHub.

Papers here are longer than READMEs and more deliberate than blog posts. They're meant to be citeable.

## Lifecycle

Each paper has a status header:

| Status | Meaning |
|---|---|
| `outline` | Section structure + bullet-level descriptions. Not a draft. |
| `draft` | Full prose, but unreviewed and likely to change. |
| `review` | Out for review with named reviewers. |
| `published` | Final. Cited externally. |
| `superseded` | Replaced by a later paper. Cite the successor. |

Status changes via the file's frontmatter. No tooling.

## Current papers

| File | Status | Target |
|---|---|---|
| [`design-sketch.md`](design-sketch.md) | outline | arxiv + project website, ~3-6 months from outline |

## Versus academic publishing

These are not peer-reviewed papers. They're position papers / design sketches. Cleave reserves "real" academic publishing for empirical work once the chain layer is shipping (consensus + state + a real testnet). That's the [`#67`](https://github.com/cleave-lang/cleave/issues/67) timeline: ~12-18 months out from now.

A position paper bridges the gap. It exists so:

- Researchers writing about smart-contract languages have something specific to cite
- Engineers at competing chains can read the design without reverse-engineering the repo
- Funders + design partners have a single artifact that captures Cleave's argument
- Future contributors can decide whether the project's direction is worth their time
