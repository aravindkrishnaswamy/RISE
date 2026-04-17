# RISE Claude Companion

This file intentionally stays thin so it does not drift from the shared docs. Start with [README.md](README.md) for the repo map, [AGENTS.md](AGENTS.md) for the shared agent guide, and [docs/README.md](docs/README.md) for deep dives.

## Quickstart

- Main build: `make -C build/make/rise -j8 all`
- Tests: `make -C build/make/rise tests` then `./run_all_tests.sh`
- Header-only changes: run `make -C build/make/rise clean` before rebuilding because header dependencies are not tracked reliably
- Sample render:

```sh
export RISE_MEDIA_PATH="$(pwd)/"
printf "render\nquit\n" | ./bin/rise scenes/Tests/Geometry/shapes.RISEscene
```

## High-Value Facts

- Public construction API: [src/Library/RISE_API.h](src/Library/RISE_API.h)
- High-level construction interface: [src/Library/Interfaces/IJob.h](src/Library/Interfaces/IJob.h)
- Main assembly implementation: [src/Library/Job.cpp](src/Library/Job.cpp)
- Parser source of truth: [src/Library/Parsers/AsciiSceneParser.cpp](src/Library/Parsers/AsciiSceneParser.cpp)
- Render-pass entry point: [src/Library/Rendering/PixelBasedRasterizerHelper.cpp](src/Library/Rendering/PixelBasedRasterizerHelper.cpp)
- Scene files expect `RISE ASCII SCENE 5`, and chunk braces must be on their own lines
- `Job::InitializeContainers()` creates the `Scene`, named managers, `"none"` defaults, and default shader ops
- `Job::SetPrimaryAcceleration()` replaces the object manager and discards previously added objects
- Named managers plus explicit reference counting are foundational; inspect [src/Library/Managers/GenericManager.h](src/Library/Managers/GenericManager.h) and [src/Library/Utilities/Reference.h](src/Library/Utilities/Reference.h) before changing lifetimes
- Tests are standalone executables, not a unit-test framework suite
- The logger defaults to `RISELog.txt`, but the main CLI overrides it to `RISE_Log.txt` in the working directory

## Thread priority — read before touching anything threading-related

- **Production default (topology-aware):** every P-core gets a render worker, every E-core except **one** also gets a worker. The reserved E-core keeps UI / daemons responsive. macOS workers use `QOS_CLASS_USER_INITIATED`; Linux/Windows use CPU-affinity pinning.
- **Benchmarks:** set `render_thread_reserve_count 0` to give every core a worker. `./bench.sh` at repo root does this automatically. Without the override, Apple Silicon's E-core reservation makes the machine appear ~10 % slower than it really is.
- **Do NOT** put all threads at `QOS_CLASS_UTILITY` / `BELOW_NORMAL` / `nice(10)`. That's the old bug — cost 2–4× real throughput on macOS. A legacy `force_all_threads_low_priority` option still exists for users who explicitly want the background-render mode; it's opt-in only.
- Controls live in `src/Library/Utilities/CPUTopology.{h,cpp}`, `src/Library/Utilities/ThreadPool.cpp`, `src/Library/Utilities/Threads/ThreadsPTHREADs.cpp`, `src/Library/Utilities/Threads/ThreadsWin32.cpp`.
- See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) "Thread priority policy" for the full rationale, per-platform APIs, and measurement recipe.

## Read Next

- Core library map: [src/Library/README.md](src/Library/README.md)
- Interface contracts: [src/Library/Interfaces/README.md](src/Library/Interfaces/README.md)
- Scene language and parser behavior: [src/Library/Parsers/README.md](src/Library/Parsers/README.md)
- Scene taxonomy: [scenes/README.md](scenes/README.md)
- Curated showcase scenes: [scenes/FeatureBased/README.md](scenes/FeatureBased/README.md)
- Regression scenes: [scenes/Tests/README.md](scenes/Tests/README.md)
- Executable tests: [tests/README.md](tests/README.md)
- Thread-safety and scene immutability: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- Planned improvements: [docs/IMPROVEMENTS.md](docs/IMPROVEMENTS.md)
