# M2 design: solver contract, hosts, fire extraction

2026-11-23. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §4.1–4.2, §5 M2. Inventory:
[SIMULATION_M0_DRIVER_INVENTORY.md](SIMULATION_M0_DRIVER_INVENTORY.md). The contract stays open to change until M3.

## Work split

| Track | Scope | Depends on |
|---|---|---|
| A | Contract header, in-process host, worker host (`rise-sim`), mesh frame codec, two reference solvers, tests | M1 cache |
| B | Fire driver moved out of `tests/FireSequenceTest.cpp` into the library behind a fire-private API; byte-identical | nothing in A |
| C | Fire adapter: the extracted driver behind the contract; `rise-sim bake` of the fire; environment options as parameters | A and B |

A and B run in parallel. C follows.

## Contract (track A), `src/Library/Simulation/SimulationSolver.h`

Frames cross the contract as encoded payloads in the cache's codecs (`grid.v1`, `mesh.v1`, later points and
transforms), so the cache writer, the in-process publisher and the renderer share one representation.

```cpp
enum class BackendStatus { Supported, Experimental, Unsupported };
struct BackendInfo   { std::string name; BackendStatus status; bool available; std::string detail; };
struct Capabilities  { std::vector<BackendInfo> backends; bool interactive, deterministic, resumable,
                       externalMacroStep; std::vector<std::string> fidelityModes; };
struct EvidenceCase  { std::string id, description, digest; double error, tolerance; bool passed; };
struct Evidence      { std::string fidelity; std::vector<EvidenceCase> cases; };   // "preview" | "predictive"
struct PortDeclaration { std::string name, quantity, units, codec; bool offers; }; // offers or accepts

struct SolverDescription {            // returned after Configure
    std::string solverId, version;
    CacheDescription cache;           // time support, outputs, fidelity, evidence digests, physical records
    std::vector<std::string> inputs;  // scene elements read
    std::vector<PortDeclaration> ports;
    Capabilities capabilities;
    Evidence evidence;
};

class ISceneInputs  { virtual bool Sample(name, time, Bytes& encoded, std::string& hash, error) = 0; };
class IFrameSink    { virtual bool Publish(index, output, time, shared_ptr<const Bytes>, receipt, error) = 0; };
class IPortExchange { virtual bool Offer(port, time, const Bytes&, error) = 0;
                      virtual bool Accept(port, time, Bytes&, error) = 0; };
class IProgress     { virtual bool Report(simulatedTime, endTime, detail) = 0; };  // false requests a stop

class ISimulationSolver {
    virtual bool Configure(const RISECBOR64::Value& parameters, ISceneInputs&, SolverDescription&, error) = 0;
    virtual bool Start(error) = 0;                                  // from the initial state
    virtual bool Restore(const Bytes& checkpoint, error) = 0;       // instead of Start
    // Advance to the next output index, or to macroEnd when the host imposes a macro step (co-stepping).
    virtual bool Advance(const AdvanceRequest&, IFrameSink&, IPortExchange*, IProgress*, AdvanceResult&, error) = 0;
    virtual bool Checkpoint(Bytes&, error) = 0;                     // valid between Advance calls
    virtual void RequestStop() = 0;                                 // thread-safe; Advance returns at a safe point
};
```

- Parameters are a canonical CBOR map. M4 binds it to a chunk descriptor; the solver also exposes the descriptor
  then. The cache key is `MakeCacheKey(solverId, version, effective parameters, input hashes)`.
- A solver registers a factory under its id (`RegisterSolver`), so hosts and tools never name a solver type.
- A solver that cannot honour `macroEnd` reports `externalMacroStep = false`; a co-stepping group refuses it.

## Hosts (track A)

- **`InProcessHost`:** owns a solver and an in-memory publisher implementing `IReadOnlyCacheSnapshot`; steps on
  request from a caller thread or its own worker thread; latest frame readable while stepping; optional recording
  through `CacheWriter`.
- **Worker (`rise-sim`, `tools/rise_sim.cpp`):** `bake <solver> <parameters.cbor|json> <cache>`, `resume <cache>`,
  `status <cache>`. Writes frames through `CacheWriter`, checkpoints atomically at a wall-time cadence and on
  SIGINT/SIGTERM, a small status record (state, simulated time, frames, wall time, last error), exit codes that
  distinguish finished, stopped and failed. `bake <scene> [simulation]` arrives with the chunks in M4.
- **Group host:** interface only in M2 (macro step, exchange order); implemented in M6.

## Reference solvers (track A, tests only)

Two small deterministic CPU solvers prove the hosts are not fire-shaped: a scalar advection–diffusion solver on a
grid with an analytic solution, and a fixed-topology mesh solver (a vibrating membrane with a known mode
frequency). Each declares evidence from its analytic check. They live under `tests/`, not in the shipping library.

## Fire extraction (track B)

- Move the production path named in the inventory into `src/Library/Simulation/Fire/` (or `Utilities/` beside the
  existing fire sources if the mirrors require it) as a fire-private API: options struct, run/restore, frame
  observer, checkpoint codec, legacy frame and manifest writers. Test fixtures, fault injection, synthetic inputs
  and `Check` assertions stay in the test file, which then calls the library.
- No numerical, ordering or rounding change. After every move: the capstone tier-6 and tier-8 windows against the
  M0 references, and the tier-8 resume snapshots (`r273_poor_burn/speed_v4.snap`, 25 steps, one per 5), byte
  compared apart from the producer id and its two checksums.
- Environment reads stay where they are in this track.

## Fire adapter (track C)

- `FireSolver : ISimulationSolver`. The first version runs the extracted loop on its own thread and hands control
  across at each published frame, so `Advance` returns at frame boundaries without restructuring the loop.
  `externalMacroStep` is false until M6.
- Legacy VDB frames and the sealed legacy manifest remain an export of the worker for the fire solver, so existing
  tools and scenes keep working; the native cache is written alongside.
- Every `RISE_FIRE_*` option that changes results becomes a parameter in the cache key. The environment is read
  once at the fire factory boundary and mapped to parameters; nothing below that reads it.
- Backends: Metal `supported`; CPU `unsupported`.

## Acceptance (plan §5)

Certified fire output byte-identical to the M0 references through `rise-sim`; fast mode within r275 tolerances;
stop and resume equal to an uninterrupted run; both reference solvers pass through both hosts with grid and mesh
frames; the test file contains tests only.
