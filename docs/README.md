# RISE Documentation Guide

This directory holds focused design notes and planning docs. Start at [../README.md](../README.md) for the repo map, then use this file to decide which deeper document you actually need.

## Stable Reference Docs

- [ARCHITECTURE.md](ARCHITECTURE.md): focused deep dive on scene immutability, thread safety, render phases, and known exceptions. Read this when touching rasterizers, animation-time mutation, caches, or shared renderer state.

## Planning Docs

- [PATH_TRANSPORT_ROADMAP.md](PATH_TRANSPORT_ROADMAP.md): prioritized backlog for path transport work, including validation expectations, acceptance criteria, and likely starting files.

## Related Docs Outside `docs/`

- [../README.md](../README.md): top-level repo map and canonical command quick reference
- [../AGENTS.md](../AGENTS.md): concise working guide for LLM contributors
- [../CLAUDE.md](../CLAUDE.md): thin Claude-compatible shim that points back to the shared docs
- [../scenes/README.md](../scenes/README.md): overall scene taxonomy and placement rules
- [../src/Library/README.md](../src/Library/README.md): core library structure and extension checklist
- [../src/Library/Parsers/README.md](../src/Library/Parsers/README.md): scene language and parser rules
- [../scenes/FeatureBased/README.md](../scenes/FeatureBased/README.md): curated showcase and torture scenes
- [../scenes/Tests/README.md](../scenes/Tests/README.md): focused regression and comparison scenes
- [../tests/README.md](../tests/README.md): executable tests and validation scenes
- [../README.txt](../README.txt): historical manual and user-facing background

## Organization Rule Of Thumb

- Put repo entry-point guidance in `README.md`.
- Put tool-specific shims in `AGENTS.md` or `CLAUDE.md`, but keep them thin and link back to shared source docs instead of duplicating long explanations.
- Put subsystem navigation close to the code in subtree `README.md` files.
- Put focused design notes and roadmaps in `docs/`.
