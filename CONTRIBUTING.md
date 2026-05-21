# Contributing to Cleave

Cleave is in very early design. The compiler does not exist yet; the language is being worked out in public. That changes what's useful to contribute and how.

## Where to help right now

- **Read the design and push back on it.** Architecture feedback is more valuable than code at this stage. The thesis is that subsystems (consensus, gas, state, VM) should be expressed as protocols you can satisfy, not framework hooks you adapt to. If you think that's wrong, or you think a specific protocol shape is too narrow, file an issue.
- **Bring weird chain designs.** If you've worked on something that doesn't fit Substrate or Cosmos SDK or Sovereign cleanly, write up what hurt. Concrete pain points shape the primitives.
- **Audit the README and roadmap.** Cleave will get cited in other places once people start linking. If you spot overclaiming or sloppiness, send a PR.

Code contributions come later. When they do, the rules below apply.

## PR flow

1. Open an issue or discussion first for anything beyond a typo. Scope alignment up front saves rewriting.
2. Fork, branch off `main`, commit, open a PR.
3. PRs need one approving review before merging into `main`.
4. Commits are one-line subjects. No multi-paragraph commit bodies, no `Co-Authored-By` trailers.

## Tone

Direct, technical, under-promising. Match the README's voice. No marketing language, no "powerful," "next-generation," "seamless." Say what the thing does. If you're not sure whether something is true yet, mark it as not yet shipped.

## Questions

Open a [Discussion](https://github.com/cleave-lang/cleave/discussions). That's the channel until something more formal exists.
