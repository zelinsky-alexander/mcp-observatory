# AGENTS.md

## Project intent

Build MCP Observatory as an original, dependency-minimal C++20 single-binary research tool for bounded collection and longitudinal comparison of MCP security observations.

## Working rules

- Inspect the repository before changing it.
- Propose and implement one small milestone at a time.
- Preserve deterministic behavior and explicit resource limits.
- Do not add network scanning, package execution, authentication, or MCP tool invocation without a separately reviewed milestone.
- Keep `mcp-native-guard` and this repository separate; use versioned data formats as the integration boundary for now.
- Do not copy code, structure, comments, or naming from public repositories, tutorials, or answers.
- Use documented public specifications, standard algorithms, and conventional C++ facilities.
- Do not add third-party dependencies without documenting package name, licence, purpose, maintenance status, and security/licensing concerns.
- Do not introduce GPL, AGPL, SSPL, source-available, or similarly restrictive code without explicit approval.
- Add focused tests for malformed, duplicate, reordered, oversized, and excessive-depth inputs where relevant.
- Never claim the code is legally cleared or plagiarism-free; require manual licence, similarity, security, and legal review before publication or commercial use.
