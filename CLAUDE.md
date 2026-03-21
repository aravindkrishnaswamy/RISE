# RISE - Realistic Image Synthesis Engine

## Build System

- **Makefile location**: `build/make/rise/Makefile`
- **Build command**: `cd build/make/rise && make -j8`
- **Clean build**: `cd build/make/rise && make clean && make -j8`
- **Config**: `build/make/rise/Config.specific` — platform-specific compiler flags, feature toggles
- **Important**: The Makefile does NOT track `.h` file dependencies. If you only change a header file, you must `make clean` to force recompilation.
- **Output binaries**: `bin/rise`, `bin/meshconverter`, `bin/imageconverter`, `bin/biospecbsdfmaker`

### Build Feature Flags (Config.specific)

- `DEF_PROFILING = -DRISE_ENABLE_PROFILING` — enables intersection counter profiling (prints stats at end of render). Comment out to disable.
- `DEF_MAILBOXING = -DRISE_ENABLE_MAILBOXING` — enables mailboxing optimization for BSP traversal (avoids redundant triangle intersection tests).

## Running

```bash
echo "render" | bin/rise "scenes/path/to/scene.RISEscene"
```

- Scenes are `.RISEscene` files (ASCII format, parsed by `AsciiSceneParser`)
- Scripts are `.RISEscript` files (loaded via `> run` directive)
- Set `RISE_MEDIA_PATH` environment variable for texture/HDR probe file resolution
- Rendered output goes to the `rendered/` directory (configured per scene via `file_rasterizeroutput`)

## Logging System

### Overview

RISE uses a singleton logging system accessed via `GlobalLog()` (returns `ILog*`). The logger writes to two destinations simultaneously with different filter levels.

### Log Levels (defined in `src/Library/Interfaces/ILog.h`)

| Level | Value | Description |
|-------|-------|-------------|
| `eLog_Event` | 1 | User-facing events (render progress, scene loading) |
| `eLog_Info` | 2 | Benign internal information |
| `eLog_Warning` | 4 | Non-fatal warnings |
| `eLog_Error` | 8 | Errors that should be investigated |
| `eLog_Fatal` | 16 | Critical failures |

Composite masks:
- `eLog_Benign` = Event + Info
- `eLog_Serious` = Warning + Error + Fatal
- `eLog_Console` = Serious + Event (used for stdout)
- `eLog_All` = everything

### Output Destinations (configured in `src/Library/Utilities/Log/Log.cpp`)

1. **Console (stdout)**: Filter = `eLog_Console` (events + serious only). This is what you see in terminal output.
2. **Log file (`RISELog.txt`)**: Filter = all messages. Written to the current working directory.

The log file name can be changed before first use via `SetGlobalLogFileName("CustomName.txt")`.

### Print Methods

- `PrintEx(LOG_ENUM level, const char* format, ...)` — printf-style formatted output (most common)
- `PrintEasyEvent/Warning/Error/Info(const char* msg)` — convenience wrappers
- `PrintSourceInfo/Warning/Error/Event(msg, __FILE__, __LINE__)` — includes source location
- `PrintNew(ptr, __FILE__, __LINE__, "description")` — memory allocation tracking
- `PrintDelete(ptr, __FILE__, __LINE__)` — memory deallocation tracking

### Key Insight for Debugging

Since `eLog_Info` is filtered out of console output but written to the log file, you can add `eLog_Info` level logging for diagnostic purposes that won't clutter the user's terminal but will appear in `RISELog.txt`. Use `eLog_Event` for messages you want visible in both terminal and log file.

## Architecture Notes

### Scene Parser Pipeline

Scene files are parsed by `AsciiSceneParser` (`src/Library/Parsers/AsciiSceneParser.cpp`). Each scene element type has a corresponding `*AsciiChunkParser` struct. The parser calls methods on the `IJob` interface which delegates to `RISE_API_Create*` functions.

**Adding a new parameter to an existing element** requires changes through the full pipeline:
1. The underlying class (e.g., `SpotLight.h/.cpp`) — add member + constructor param
2. `RISE_API.h/.cpp` — add param to Create function
3. `IJob.h` — add param to virtual method
4. `Job.h/.cpp` — add param to concrete implementation
5. `AsciiSceneParser.cpp` — add parsing in the chunk parser

### Photon Mapping

- **PhotonTracer** (`src/Library/PhotonMapping/PhotonTracer.h`) — template base class for all photon tracers. Contains `TraceNPhotons()` (inner loop) and `TracePhotons()` (outer driver).
- Three photon tracer types: `CausticPelPhotonTracer`, `GlobalPelPhotonTracer`, `TranslucentPelPhotonTracer`
- Photon budget is divided proportionally across light sources by their total radiant exitance
- **Safety valve**: If a light shoots `thislummax * 100` photons and stores zero, the loop breaks to avoid infinite loops
- `shootFromMeshLights` / `shootFromNonMeshLights` — control whether mesh luminaries or non-mesh lights participate in photon shooting (scene file params on photon map blocks)
- `shootphotons` — per-light parameter (on `spot_light` and `omni_light` blocks) that controls whether `CanGeneratePhotons()` returns true. Default is TRUE.

### Light Types

| Type | Class | `CanGeneratePhotons()` | Scene keyword |
|------|-------|----------------------|---------------|
| Spot | `SpotLight` | Controlled by `bShootPhotons` (default true) | `spot_light` |
| Point/Omni | `PointLight` | Controlled by `bShootPhotons` (default true) | `omni_light` |
| Ambient | `AmbientLight` | Always false | `ambient_light` |
| Directional | `DirectionalLight` | Always false | `directional_light` |

### Spatial Acceleration

- BSP trees (SAH-based): `> set accelerator B <max_elements_per_node> <max_recursion>`
- Octree: `> set accelerator O <max_elements_per_node> <max_recursion>`
- Mailboxing: compile-time flag `RISE_ENABLE_MAILBOXING` — tags objects with a ray ID to skip redundant intersection tests during BSP traversal

### Key Directories

- `src/Library/` — core library code
- `src/Library/Interfaces/` — pure virtual interfaces (ILight, IJob, ILog, etc.)
- `src/Library/Lights/` — light implementations
- `src/Library/PhotonMapping/` — photon tracer and photon map classes
- `src/Library/Parsers/` — scene file parsers
- `src/Library/Intersection/` — ray-object intersection code
- `src/RISE/` — main executable entry point
- `scenes/` — scene files organized by feature category
