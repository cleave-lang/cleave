---
rfc: 0000
title: "RFC template (copy this for new RFCs)"
status: draft
authors: ["Your Name"]
tracking: https://github.com/cleave-lang/cleave/issues/NNN
created: YYYY-MM-DD
---

# Summary

One paragraph: what does this RFC propose? Stated as a noun phrase ("A type-level effect system for...") rather than a verb ("We should add..."). Someone reading just this paragraph should be able to decide whether the RFC is relevant to them.

# Motivation

Why does this need to happen? What's broken / missing / suboptimal without it? Concrete examples beat abstract claims. Link to issues, prior discussions, real bugs that this would prevent.

# Design

The proposed change. Long-form. Sections as needed.

## Sub-design topics

Break into subsections when there's structure: API surface, type rules, runtime semantics, error messages, etc. Show code examples in the language being designed.

## What changes externally

What does a developer using Cleave see differently? New syntax? New error messages? Different runtime behavior? Different gas costs?

## What changes internally

What does the compiler / runtime have to do differently? Touch which files? What's the migration path for existing code?

# Alternatives

What other designs were considered? Why is this one preferred?

- **Alternative A**: brief description, pros, cons
- **Alternative B**: same shape
- **Do nothing**: what happens if we don't do this? (Always worth considering.)

# Drawbacks

The cost of saying yes. There always is one. Be honest:

- Implementation complexity
- Audit surface
- Developer-facing complexity
- Risk of being wrong (and the cost to revert)
- Interaction with other planned work

# Open questions

Things the author can't decide alone or doesn't know yet. Each is a bullet, ideally with an open-ended question mark. Resolved questions move to the Design section; rejected directions move to Alternatives.

# Reversibility

How hard is this to undo if it turns out wrong?

- **High**: a flag flip, a backward-compatible deprecation
- **Medium**: a breaking compiler version + migration tool
- **Low**: every contract on every chain has to remigrate; in practice we don't reverse it

State this honestly. Low-reversibility decisions deserve longer review windows.

# Related work

Prior art in other languages / chains / academic papers. Cite specifically; vague references don't help.

# Implementation roadmap

If accepted, what happens next? Sub-issues, ordering, dependencies. Keep this light; the real plan emerges from implementation discussion.
