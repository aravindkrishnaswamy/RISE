# Consolidation arc — Windows verification pass

**Why this exists:** the entire 2026-06/07 agentic arc (Model-B cutover → F5 agent
surface → shared undo → F2 render coordinator → Secure-MCP → chat providers) was
developed on a Mac with **no MSVC available**.  All Windows legs were written by
symmetry (precedent commit `96e74f59`) and have **never been compiled**.  This
pass gates the fast-forward merge of `feature/gui-snapshot-prototype` to master.

> **Progress update (2026-07-08):** the solution BUILD was fixed on a real
> Windows machine — `a30d7ca0` (`#include "pch.h"` across the Agent module +
> the loopback server's Windows bound-address readback) — and the Windows GUI
> gained a chat panel (`d7c9e598`, under review).  **Still owed from this
> checklist:** a clean-rebuild warnings check, `run_all_tests.ps1` (full suite),
> and the §3 manual smoke including the new chat panel.

## 1. Build steps (in order)

```powershell
# in repo root, on the branch
git submodule update --init extlib/oidn/source     # if not already done
pwsh -File extlib\oidn\build.ps1                   # optional; CPU fallback works

# Full solution build, Release — Library + RISE-CLI + RISE-GUI
# (open build\VS2022\ solution in VS2022, Build Solution, Release x64)

# Full test suite (builds tests via CMake under build\cmake\rise-tests)
.\run_all_tests.ps1
.\run_all_tests.ps1 -Config Debug                  # if time permits
```

Expected: **0 warnings on the projects listed below** (warnings are bugs — fix
the root cause, never suppress), and test parity with the Mac suite
(185 executables green; POSIX-only smoke tests like `AgentStdioSmokeTest` are
`#ifndef _WIN32`-guarded and self-skip).

## 2. Watch-list — code that has never seen a Windows compiler

Highest-risk first:

1. **`src/Library/Agent/AgentLoopbackHttpServer.{h,cpp}`** — the Winsock leg:
   `WSAStartup`, `std::atomic<SOCKET>`, `inet_pton`, `closesocket`, socket
   timeouts; plus the S4 bearer-token CSPRNG (`BCryptGenRandom`).  Written
   entirely by symmetry.
2. **Link libraries** — `ws2_32` + `bcrypt` were added to
   `build/VS2022/RISE-GUI/RISE-GUI.vcxproj` and `build/VS2022/RISE-CLI/RISE-CLI.vcxproj`.
   The tests CMake (`build/cmake/rise-tests/CMakeLists.txt`) gained the same two
   libs during this arc's prep (2026-07-08) — if `AgentLoopbackHttpTest` or
   `AgentMcpAdapterTest` fail to LINK, look here first.
3. **`build/VS2022/RISE-GUI/RenderEngine.{h,cpp}` + `ViewportBridge.cpp`** —
   the F2 S2b/S4 symmetry edits: production renders route through
   `SceneEditController::SubmitProductionRenderSync` / the composed
   progress-cancel helper; the `StopInteractive()` vs full `Stop()` split
   (commits `f8447594`, `eae7178d`, `ab58136d`, `bc4538eb`).  The **explicit
   `guiProgress` param** exists because Windows installs its progress callback
   per-render, not persistently — verify the Cancel button still works (this
   exact seam was a found bug, fixed blind).
4. **Property-panel refresh on `SetProperty` false** (commit `96e74f59`,
   wording follow-up `2e4298a2`) — Windows + Android panels follow the Mac
   idiom; compile + a quick manual poke.
5. **`build/VS2022/Library/Library.vcxproj` + `.filters` completeness** — the
   Agent module grew to 9 file pairs (`AgentChatCodecs`, `AgentChatLoop`,
   `AgentDiagnostic.h`, `AgentLoopbackHttpServer`, `AgentMcpAdapter`,
   `AgentRpc`, `AgentSession`, plus `Base64`, `Json`, `SchemaGen`,
   `InMemoryRasterizerOutput`).  A missing `<ClCompile>` entry = unresolved
   externals at link.
6. **MSVC-only diagnostics** the Mac toolchain can't surface: signed/unsigned
   `/W4`-class warnings in the new socket code, `SOCKET` (unsigned) vs `int`
   comparisons, `%zu`-style format strings in any diagnostics.

Known trap: if the tests build fails with `LNK1104` on `RISE.lib`, it's the
`$(SolutionDir)` path trap (see docs/skills/variance-measurement.md) — rebuild
the Library project first.

## 3. Manual Windows GUI smoke

MCP hosting is still **Mac-only**, but as of `d7c9e598` the Windows GUI has its
own chat panel — smoke it alongside the F2 coordinator symmetry edits:

- [ ] Chat: provider pick + key save + a real edit turn ("make the orange
      things red") → viewport updates, Save enables, Undo works.
- [ ] Click Render (production) while a chat turn is mid-flight → no crash
      (the Mac render-race class — pending the d7c9e598 review verdict).
- [ ] Close the scene mid-chat-turn → no crash/hang.
- [ ] Stop button mid-turn → turn ends promptly.

- [ ] Open a scene, interactive viewport renders and refines.
- [ ] Production render (Render button): progress bar ticks, **Cancel works**
      (the composed-callback seam — item 3 above).
- [ ] Pause the viewport, then production render → **must not be refused**
      (the `StopInteractive()` split; on the Mac this was the user-found
      round-4/5 bug).
- [ ] Repeat a second production render in the same session (agent worker not
      retired by the pause path).
- [ ] Edit a property that fails validation → panel refreshes to actual state
      (commit `96e74f59`).
- [ ] Save, close, reopen — no prompt-less data loss.

## 4. Reporting back

For each failure: project, file:line, full error text, and (for test failures)
the per-test log from the `run_all_tests.ps1` output directory.  Fixes will be
authored on the Mac against this list; do not hand-patch on Windows unless
trivial, so the branch stays the single source of truth.
