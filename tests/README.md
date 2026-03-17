# Test Guide

The tests in this repository are standalone executables built from `tests/*.cpp`. They are not managed by a unit test framework.

## Build And Run

```sh
make -C build/make/rise tests
./run_all_tests.sh
```

Build behavior comes from [../build/make/rise/Makefile](../build/make/rise/Makefile). The makefile glob picks up every `tests/*.cpp` file automatically and links it against the core library.

Built binaries land in `bin/tests/`.

## Current Test Inventory

- `ClippedPlaneGeometryTest.cpp`: clipped plane geometry behavior
- `FinalGatherShaderOpTest.cpp`: final gather interpolation helpers and stability logic
- `IrradianceCacheTest.cpp`: irradiance cache behavior
- `OpticsTest.cpp`: optics utilities
- `PrimesTest.cpp`: prime-related utilities

## Style Of Test Used Here

- Each file is an executable with its own `main`.
- Assertions are usually plain `assert(...)`.
- Helpful progress text is printed with `std::cout`.
- The best targets are deterministic helpers, math utilities, cache logic, and other focused behavior that does not require comparing full rendered images.

## Adding A New Test

1. Add a new `tests/<Name>.cpp` file.
2. Include the minimal headers you need from `src/Library`.
3. Keep the test deterministic and fast.
4. Use `assert` for pass/fail checks.
5. Build with `make -C build/make/rise tests`.
6. Run with `./run_all_tests.sh`.

No makefile edit is needed for a new `tests/*.cpp` file because the existing wildcard-based rule discovers it automatically.

## Relationship To Sample Scenes

- Use `tests/` for deterministic logic and small subsystem checks.
- Use `scenes/FeatureBased/` for authored end-to-end coverage of parser and renderer features.
- If a feature is user-visible and deterministically testable, it usually deserves both.
