# Changelog

All notable changes to Cleave will be documented here. Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project uses [Semantic Versioning](https://semver.org/).

Prose references a version as `v0.X.Y`; headings stay bare `[0.X.Y]`.

## [Unreleased]

## [0.0.1]

### Added

- Repository scaffolding: README, LICENSE (Apache-2.0), DESIGN.md, CONTRIBUTING.md, CODE_OF_CONDUCT.md, SECURITY.md, issue and PR templates.
- Brand assets in `.github/`: mark + wordmark lockup, light and dark variants.
- Example `.cv` files in `examples/`: minimal chain manifest, multi-VM chain, counter module, token module, custom consensus protocol.
- Compiler bootstrap in C11 (`compiler/`): Makefile, `cleavec` binary with `--version`, `--help`, and `--tokens <file.cv>` commands.
- Lexer covering keywords, identifiers, integer literals (decimal, hex, binary, with `_` separators), string and character literals, single and multi-character punctuation, `//` line and `/* */` block comments, source line and column tracking, and error recovery on unknown characters.
- Hand-rolled testing harness (`compiler/tests/test.h`, ~60 lines) and 224 lexer assertions across 29 tests.
- CI workflow building the compiler and running the test suite on every push and PR.
- Branch protection ruleset on `main`: required PR review, required CI check, linear history, signed commits, no force push, no deletion; OrganizationAdmin bypass.
- GitHub Discussions, milestones (v0.1, v0.2), and labels (`compiler`, `stdlib`, `tooling`, `design`).
