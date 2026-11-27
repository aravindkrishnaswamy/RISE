# M6 results: coupling between simulations

2026-11-25. Plan: [SIMULATION_IN_RISE_PROPOSAL.md](SIMULATION_IN_RISE_PROPOSAL.md) §2 (decision 2), §4.1–4.2, §5 (M6).
Contract: [SIMULATION_SOLVER_CONTRACT.md](SIMULATION_SOLVER_CONTRACT.md) v1, unchanged; additive entries in its
Appendix C (§10 below). Codec: [SIMULATION_TRANSFORM_FORMAT.md](SIMULATION_TRANSFORM_FORMAT.md).

**Status.** All seven stages were built and tested: the transform codec and a rigid-body solver with computed
evidence; the co-stepping group host with five port adapters; the cloth's port-driven collider and contact
reaction; two-way cloth ↔ rigid coupling with the conservation and weak-coupling evidence; scene syntax and
`rise-sim bake <scene> <group>` (and the bake manager) baking a group as one job; `transform_source` rendering;
fire → cloth one-way from a fire cache. What was not done or not verified is listed in §9.

## 1. Files

| Area | Files |
|---|---|
| Codec | `Simulation/SimulationTransform.{h,cpp}` (`transform.v1`; native cache type) |
| Rigid solver | `Simulation/Rigid/RigidBody.{h,cpp}` (mass properties, integrator, plane impulses), `RigidSolver.{h,cpp}` (`rigid.bodies`), `RigidParameterSchema.{h,cpp}` (`rigid_simulation` chunk) |
| Group host | `Simulation/SimulationGroup.{h,cpp}` (`SimulationGroup : ISimulationGroupHost`, coupling-evidence registry) |
| Adapters | `Simulation/SimulationPortAdapters.{h,cpp}` (factory registry, unit table, five adapters) |
| Cloth ↔ rigid | `Simulation/Coupling/ClothRigidCoupling.{h,cpp}` (scenarios, totals, the coupling evidence) |
| Scene | `SceneSimulation.{h,cpp}` (groups, `port_source`, `BuildSceneGroup`), `SceneSimulationWorker.cpp` (coupled bakes), `SimulationBakeManager.cpp` (a group is one job), `SceneInputs.{h,cpp}` (simulation-driven objects), `Job.{h,cpp}` (`SetObjectTransformSource`), `ChunkParserRegistry.cpp`, `Interfaces/IJob.h`, `Interfaces/SimulationDescriptor.h` |
| Tests | `SimulationTransformTest`, `RigidSolverTest`, `SimulationGroupTest` (+ `SimulationCouplingSolvers.h`), `SimulationPortAdapterTest`, `SimulationCouplingTest`, `SimulationGroupSceneTest`, `SimulatedTransformTest`, `SimulationPlumeBannerTest` |
| Scenes, script | `scenes/Tests/Simulation/rigid_cloth_trampoline.RISEscene`, `plume_banner_from_cache.RISEscene`; `scripts/simulation_m6_trampoline.sh` |

All new library sources are in the five build lists (all three Xcode Sources phases: rise, RISE-GUI, rise-sim),
compiled without fast-math and FP contraction (the cloth's rule) except the schema.

## 2. The rigid-body solver `rigid.bodies` (version `m6.1`)

**Model.** Free rigid bodies; mass and inertia from density (or a total mass) and shape: box, sphere, cylinder
(axis z), a closed outward mesh by its polyhedral mass properties (divergence theorem over signed tetrahedra; a
tessellation's split seam vertices are welded for the closure check only), or a scene object's geometry
(`body_object`, world space at t = 0; the body's pose is its centroid and principal axes). Body frame = principal
frame at the centre of mass (analytic shapes keep their axes). Gravity; loads from the port `body_load` (force at a
world point plus a torque, held over each macro step). No joints, no body–body contact.

**Integration.** Kick–drift–kick; the free rotation is the symplectic principal-axis splitting of Dullweber,
Leimkuhler and McLachlan (1997), H = Σ Πᵢ²/(2Iᵢ) split into three exact axis rotations composed (1/2, 2/2, 3,
2/2, 1/2). Each sub-flow preserves the spatial angular momentum and |Π| exactly; energy error O(h²), bounded.
The kicks change the angular momentum about the origin by exactly h(τ + p × F), so an impulse exchanged through a
port is conserved exactly (§5).

**Plane contact (preview).** Sequential impulses at the deepest support point (Newton restitution, zero below a
resting speed 2|g|h; Coulomb friction inside the cone), then a projection out of the plane. Not validated.

**Evidence** (computed at Configure at the configured substep, memoised, digest-bound; tolerances ≈ 2× the error
at the default 1/240 s substep):

| Case | Error at h = 1/240 s | Tolerance | Order (h → h/2) |
|---|---|---|---|
| `symmetric-top` (closed form R(t) = exp(t L̂/I₁) R₀ exp(t μ ê₃), 5 s) | 3.36e-4 (max ‖R − R_exact‖_F) | 7e-4 | 2.000 |
| `tennis-racket` (I = 1, 2, 3, spun near axis 2, 60 s, ≥ 4 flips) | 3.01e-6 (max rel. change of E and |L|) | 7e-6 | 2.000 |
| `projectile` (spinning box, 1 s) | 4.8e-15 (position / range) | 1e-12 | exact |
| `pendulum` (1 m rod on a stiff damped external spring, 0.5 rad) | 7.8e-4 (period vs 4√(I_p/mgd)K(sin θ₀/2)) | 1.6e-3 | — |

`predictive` is requested only without plane contact; a coarse substep (0.2 s) fails the cases and publishes
preview (tested). Ports: offers `body_state` (`transform.v1`, the `bodies` output), accepts `body_load` (`field.v1`
over `TransformBodyDomain(names)`: `force` N, `point` m, `torque` N m). Macro steps, rollback, checkpoints,
deterministic (tested byte for byte), CPU supported, interactive. Physical records (`rigid.bodies.v1`): per body
mass, moments and its surface in the body frame (the `rigid_surface` adapter reads it).

## 3. The group host (`SimulationGroup`)

- **Members and connections.** Live members (each must have `externalMacroStep`; implicit needs `rollback`),
  cached members (a snapshot read through `CachedPortFeed`, optionally at a time offset), `PortConnection`s with
  an explicit adapter, an automatic one (`FindPortAdapter`) or none (`PortsCompatible`), and auxiliary ports an
  adapter reads.
- **Macro step.** `GroupPolicy::macroStep`, else the smallest live output interval; the macro grid is k·H.
- **Start.** Each member is primed with a zero-length macro step (publishes frame 0 and offers its initial state).
- **Explicit staggered.** Jacobi: every member reads values offered at the step start. Gauss–Seidel (in
  `memberOrder`): a later member reads an earlier member's end-of-step offer. A cache counts as having advanced.
- **Implicit.** Members `SaveState`, advance with frames held back; the residual of every connection (what it
  would serve now minus what it served) is tested against `convergenceTolerance·|value| + absoluteTolerance`; if
  not converged the members `RestoreState` and repeat with the previous iterate's end-of-step values,
  under-relaxed per (connection, channel group) — position and velocity of a mesh, force/point/torque of a load
  relax separately — with Aitken's factor from the second iterate (either sign, |ω| ∈ [1e-4, 2]). After
  `subIterations` without convergence: fail with a diagnostic naming each connection's residual, or (`Halve`)
  retry as two half steps down to `minimumMacroStep` (default H/64).
- **Checkpoint.** Member checkpoints plus every offered value in flight and the offered statics, at a macro
  boundary; `Restore` refuses another configuration's state. Stop/resume is byte-identical (tested with the
  oscillators, the cloth ↔ rigid trampoline in memory and through `rise-sim`).
- **Caches.** Each member publishes to its own memory snapshot and, with `Record`, its own disk cache; each recorded
  cache gets `rise-sim.group.cbor` (group identity, policy, members with key, epoch and fidelity, connections).
- **Keys.** A coupled member gains the key input `rise.coupling` (§7 of the brief's M4 gap): its identity covers the
  policy, every member upstream of it (live: solver, version, effective parameters, input identities; cached: its
  key, its manifest digest when foreign, its time offset), and every connection into them with its adapter's
  identity. A member nothing feeds keeps its standalone key (tested: the upstream of a one-way coupling).

**Test solver results** (`SimulationGroupTest`: two oscillators coupled by a spring, exact solution known; the
accepted partner value is its end-of-step position, interpolated linearly over the step):

| Scheme | Error at H = 0.04 s | H/2 | Order | Passes |
|---|---|---|---|---|
| staggered Jacobi | 1.17e-2 | 5.93e-3 | 0.98 | 50 |
| staggered Gauss–Seidel | 1.09e-2 | 5.58e-3 | 0.97 | 50 |
| implicit Gauss–Seidel (tol 1e-7) | 2.86e-4 | 7.07e-5 | 2.01 | 101 |
| implicit Jacobi (tol 1e-7) | 2.86e-4 | 7.07e-5 | 2.01 | 152 |

Strong coupling (kc = 4000, H = 0.05): the plain Gauss–Seidel fixed point diverges (diagnostic tested); Aitken
converges in 3 passes; halving converges after 2 halvings; a floor above H/2 fails. Aitken is effective with
Gauss–Seidel (one relaxed interface); with Jacobi its per-connection factors are not a secant model of the
coupled map and it can stall (measured; use Gauss–Seidel or plain under-relaxation there). Exchanged `field.v1`
samples are binary32: relative tolerances below ~1e-7 cannot be met.

## 4. Port adapters

Factories by id (`CreatePortAdapter(id, parameters, context)`), the context giving the declarations and the
offering/accepting members' cache descriptions. Each adapter is a pure function of its input bytes, states
`Fidelity()` and `Scope()`, and its `Identity()` (SHA-256 of id, version, effective parameters) enters downstream
keys.

| Adapter | Converts | Label | Tested |
|---|---|---|---|
| `unit_scale` (auto) | `field.v1` / `transform.v1` / `grid.v1` to other units: one factor per channel from a fixed table (m cm mm km in ft; m/s cm/s mm/s km/h; kg g; kg/m³ g/m³ g/cm³; N kN mN; N m, N mm; Pa kPa MPa bar; K; rad/s; 1) | predictive | kN→N field, cm→m transforms, cm/s→m/s grid; an oscillator in cm coupled to one in m matches the m run to 1e-6 |
| `field_relabel` (auto) | `field.v1` → `field.v1` on the identical domain, channels renamed (positional or `channels [[a, b]]`) | predictive | values bit for bit; refused across domains |
| `rigid_surface` | `transform.v1` body + its surface (body frame; from the rigid record, or given) → `mesh.v2` world positions, velocities v + ω × r, normals, inline topology | predictive | placement and rigid velocity exact to binary32; another topology refused |
| `aerodynamic_drag` | `grid.v1` gas velocity (and density, or temperature → ρ = pM/(RT), or a constant) + the loaded surface's `surface_state` (auxiliary) → `field.v1` traction ½ρ[C_n |u_n| u_n n̂ + C_t |u_t| u_t] on the velocity relative to the cloth (defaults C_n 1.2, C_t 0.02, M 0.028964 kg/mol, p 101325 Pa) | **preview (a model)** | closed form on a uniform flow, three density sources, cm/s |
| `contact_reaction` | per-collider `force`/`point`/`torque` → `body_load` over a body set, colliders summed onto their bodies with torques moved to a common point | predictive | two colliders on one body |

## 5. Cloth ↔ rigid two-way coupling

**Cloth additions (`ClothSolver.cpp`, localized; reported for the cloth numerics agent).** A mesh collider may take
`motion: {type: port, port: <name>}` (default `collider_surface`). Its accepted surface (`mesh.v2`, the collider's
own topology) is where it must be at the END of the held step; the rigid pose is fitted to it (Horn's quaternion
method) and the collider moves there from the pose it held (rotation at a constant rate about the relative axis,
centroid linearly), through a `TransformTrack` (`PortTrack`) — so `ClothModel`'s collision detection, carry and
kinematic stages treat it as any tracked rigid collider. The cloth then offers `collider_reaction` (`field.v1`,
one element per port-driven collider): the mean force the cloth exerted ON the collider over the held step, the
point (collider centroid at the step's end pose) and the torque about it, integrated from the step's own barrier
and friction forces. State (poses, accumulated impulse) is in `SaveState`/checkpoints (key `port_colliders` only
when such a collider exists). Evidence condition `motion` gains `port`; the scope says the coupling's own evidence
covers it. Version unchanged (no output changes for existing parameters).

**Hooks in the cloth numerics agent's files (please reconcile):**
- `ClothModel.h`: public member `std::function<void(const ContactFrame&, const std::vector<double>&, double)> stepObserver;` (after `portTraction`).
- `ClothModel.cpp` (`StepOnce`, before `xPrev.swap(x)`): `if (stepObserver && p.contact) stepObserver(*p.contact, xNew, h);`.
- `ClothContact.h/.cpp`: `void BarrierContact::MeshForces(const ContactFrame& f, const std::vector<double>& x, std::uint32_t mesh, std::vector<double>& force) const` — the pairs and friction records of one mesh collider on a filtered copy of the frame (candidates rebuilt for x), through the public `Evaluate`; appended at the end of the file.
- `ClothParameterSchema.cpp`: chunk field `collider_port <object> [<port>]`.

**Conservation (the M6 acceptance)** — `SimulationCouplingTest`, zero gravity, no pins: a 0.3 m box (0.81 kg)
moving up at 0.5 m/s and spinning at 2 rad/s into a free 0.6 m cotton sheet (13 × 13, 0.054 kg) 1 cm above it;
backward Euler cloth (its momentum M(v' − v) = h F is exact), no damping, friction 0.3; 0.5 s. Totals from the
published frames (sheet binary32, body binary64), max over every frame (end = last frame):

| Scheme, macro step | |ΔP|/|P₀| max (end) | |ΔL|/|L₀| max (end) | KE rise | Passes |
|---|---|---|---|---|
| staggered Gauss–Seidel, 1/30 s | 2.82e-2 (1.1e-3) | 6.7e-2 (9.9e-4) | 2.3e-2 | 15 |
| staggered Gauss–Seidel, 1/60 s | 1.50e-2 (5.2e-5) | 5.0e-2 (2.4e-4) | 5.9e-3 | 30 |
| staggered Jacobi, 1/30 s | 2.82e-2 (4.2e-8) | 7.3e-2 (2.3e-4) | 2.3e-2 | 15 |
| staggered Jacobi, 1/60 s | 1.52e-2 (1.2e-4) | 5.7e-2 (2.1e-4) | 5.9e-3 | 30 |
| implicit GS, tol 1e-3, 1/30 s | 1.48e-5 | 3.3e-4 | 0 | 39 |
| implicit GS, tol 1e-4, 1/30 s | 2.55e-6 | 3.9e-4 | 0 | 47 |
| implicit GS, tol 1e-6, 1/30 s | 5.2e-8 | 3.9e-4 | 0 | 61 |

- `--extended` adds H = 1/120 s staggered (|ΔP|/|P₀| 1.31e-2, end 2.3e-5; KE rise 5.9e-3) and implicit at 1/60 s
  (7.4e-8): the staggered maximum flattens below 1/60 s (order 0.19 from 1/60 to 1/120; the KE rise is the same at
  1/60 and 1/120) — at those steps the sheet's own substep (1/240 s) and the impact's force peak, not the exchange
  lag, set the in-flight impulse. The first-order statement holds between 1/30 and 1/60 s only.
- Linear momentum crosses the port as an impulse, so it balances exactly once nothing is in flight: the staggered
  error is the impulse a lagging partner has not yet received — first order in the macro step (observed orders
  0.92 GS, 0.90 Jacobi) — and it is delivered after the contact ends (end values). Implicit coupling conserves
  P to the iteration tolerance (1.5e-5, 2.5e-6, 5e-8 for tolerances 1e-3, 1e-4, 1e-6; float32 floor below).
- Angular momentum: the remaining 4e-4 under implicit coupling is backward Euler's own rotational damping of the
  sheet (Σ m h v′ × v ≠ 0), not the port (torque crosses about a fixed point; the kicks apply exactly h(τ + p × F)).
- Energy: implicit coupling never raises the kinetic energy (barrier conservative, friction dissipative);
  staggered coupling injects energy, second order in the macro step (2.3e-2 → 5.9e-3, order 1.98).

**Weak coupling.** A 0.7 m sheet falling under gravity onto a floating box (box gravity off; only the sheet loads
it), 0.4 s; two-way implicit group against the sheet fed one-way from the rigid bake as a CACHED member (through
`CachedPortFeed`; the box does not move):

| Box mass | m_sheet/M | Box position difference | / ratio | Sheet max vertex difference |
|---|---|---|---|---|
| 2 kg | 3.68e-2 | 2.42e-2 m | 0.658 m | 8.3e-2 m |
| 8 kg | 9.19e-3 | 6.18e-3 m | 0.673 m | 3.5e-2 m |

The difference is the mass ratio times a constant (0.66 m, 2% apart). The sheet's difference falls more slowly:
the sheet is the light partner, and where it drapes over the box edge its shape is sensitive.

**Coupling evidence and the group label.** Kind `contact_reaction+rigid_surface` (registered by
`RegisterBuiltinSolvers`): the zero-gravity problem at 9 × 9 with implicit Gauss–Seidel at 1e-6, three
digest-bound cases: linear momentum drift 9.3e-8 (tolerance 5e-7), angular momentum drift 9.3e-5 (5e-4), KE rise
0 (1e-6); conditions `exchange: impulse`, `surface: rigid-fit`.

**Label rule for groups.** A coupled member is `predictive` only if (1) it is predictive itself, (2) every member
upstream of it is (cached: its cache's label), (3) every connection into them is exact (adapters declaring
`predictive`; direct connections), (4) the policy is implicit with 0 < tolerance ≤ 1e-6, and (5) the coupling kind
has registered evidence whose cases pass. The coupling cases then join the member's evidence, with condition
`coupling: implicit-<order>` and `coupling_*` conditions, and the scope gains a coupling sentence (so the scoped
digests change with the coupling). Otherwise `preview`, with `fidelityLimit` and a note saying which rule failed
(staggered coupling; a loose tolerance; a model adapter such as `aerodynamic_drag`; a preview member; no
evidence). Tested: implicit cloth ↔ rigid is predictive; staggered is preview.

**Trampoline** (`TrampolineScenario`, 15 × 15 sheet pinned at its corners, 0.1 kg ball from 0.25 m): staggered
and implicit runs; the ball lands and is caught (lowest centre −0.18 m staggered, −0.13 m implicit after 0.37 s).

## 6. Scene syntax and baking

`rigid_simulation` (generated from the rigid schema: `frames`, `frame_rate`, `substeps`, `gravity`; chunk fields
`body`, `body_object`, `plane`), `simulation_group`, `port_source` on every solver chunk, `collider_port` on the
cloth chunk, `transform_source` on `standard_object`. Syntax and an example: `src/Library/Parsers/README.md`
"Coupling (M6)"; sample: `scenes/Tests/Simulation/rigid_cloth_trampoline.RISEscene`.

```text
simulation_group
{
	name			trampoline
	members			drop
	members			sheet
	connect			drop.body_state -> sheet.collider_surface via rigid_surface body=ball
	connect			sheet.collider_reaction -> drop.body_load via contact_reaction targets=ball
	policy			implicit
	member_order	drop sheet
	tolerance		1e-6
	absolute_tolerance	1e-4
}
cloth_simulation { ... port_source surface_load from fire.volume via aerodynamic_drag temperature_channel=temperature time_offset 10.55 }
```

- **Planning.** A group depends on its members, a `port_source` simulation on its upstream. At the group's turn
  the members' keys and labels are replaced by the group's (`BuildSceneGroup`); a `port_source` simulation is
  keyed through a group of itself and its upstream caches (which must be complete). A group's state is its
  members' (complete only when all are); editing one member makes both members and the group stale (tested);
  rebaking a `port_source` upstream makes the downstream stale (tested: the M4 gap).
- **`rise-sim bake <scene> <group | member>`** runs ONE process with the group host: every live member records its
  own cache (resuming partial caches of the current key when a group checkpoint exists), group checkpoint
  `rise-sim.group.checkpoint` and a status in the group's directory (`caches/<group>`), statuses in every member
  cache; stops at macro boundaries on `rise-sim.stop` (group or member directory) or `--stop-after-frames N`
  (macro steps), and `resume` continues byte-identically (tested). The scene bake equals the same group driven
  programmatically byte for byte (tested).
- **Bake manager.** A group is one job (`rise-sim bake <scene> <group> --no-upstream`); naming a member queues its
  group; members are never jobs of their own; a group's upstreams are its members' external upstreams.
- **Labels in the sample.** The trampoline bakes in 47 s (31 frames, 15 × 15 sheet). The sheet's Newton noise in
  its reaction (~1e-5 relative) is above a 1e-6 relative tolerance; `absolute_tolerance 1e-4` (N) lets the
  iteration end at that noise.

## 7. `transform_source` rendering

`standard_object { transform_source <simulation>.<output>[<body>] }` (`IJob::SetObjectTransformSource`, appended at
the IJob tail; vtable manifest updated). **Design choice:** the body's motion is installed as ordinary keyframes on
the object's `position` and `orientation` at load (scene time = simulation time), so scrubbing, playback, the
viewport and motion blur — the time-indexed multi-threaded path included — treat it exactly as a keyframed
transform; keys at every output time and a quarter interval either side of it, those from the frame's own
velocities (x + v d, exp(ω d) R), so the blur over a shutter of up to half an interval follows the simulated
velocity. The authored transform is the pose at the first frame, moved by the body's displacement since then
(Euler angles of RISE's R = Rx Ry Rz, unwrapped). This replaces the per-frame provider at the prepare seam the brief
named: the keyframe path needs no new renderer code and reuses the verified blur machinery. Scene inputs read a
driven object at its authored pose (its simulation keys neither move it nor enter its identity), so a body made
from the object does not depend on its own output.
**Refresh after an in-app bake (2026-11-26):** the keys are not built once. `Job` keeps, per driven object, the
body it follows, its authored pose and a stamp of the cache generation the keys came from (key, epoch, frames of
the output, sealed or partial). `SceneEditController::RefreshSimulationFrames` (the call the app polls when the
bake manager's frames generation moves) calls `Job::RefreshTransformSources` under the render park, after the mesh
rebind: an object whose stamp changed (a bake advanced or finished, a new epoch, a cache that is gone) has its
`position` and `orientation` timelines dropped (`Animator::RemoveKeyframes`), is put back at the authored pose,
and gets new keys from the cache through the same derivation scene load uses; then the scene is re-posed at the
current time (`Scene::SetSceneTimeForPreview`: the animator evaluated, the top-level structure invalidated, the
per-object runtime state reset, exactly the scrub path). A cache that went away leaves the authored pose. Live
in-process rigid runs still drive nothing (live stepping is meshes only). Test: `SimulationBakeManagerTest` B.

`SimulatedTransformTest`: poses at keys exact (4.4e-16) and between keys exact for this motion; a blurred render of
the simulated body matches the same motion keyframed by `timeline` chunks (0.9610 vs 0.9599) and the time average
of unblurred renders over the shutter (0.9600); blurred frames take the time-indexed path; refusals.
`scripts/simulation_m6_trampoline.sh` bakes the trampoline in a private copy under `~/.cache/rise-m6/` and renders
frames 0, 10, 20, 30 to `rendered/simulation_m6/`.

## 8. Fire → cloth one-way from a cache

No fire solver ran. The source is the M2 acceptance bake (`~/.cache/rise-sim-m2/t8_accept`, finished, opened
read-only): producer `fire.methane`, 3 frames from 10.466 s at 0.0625 s, preview, output `volume` (`grid.v1`, 69 × 69
× 106 cells of ~2 cm; channels carbon, temperature [K], reaction, velocity [m/s] × 3 on 71 × 71 × 108, chem_*). A
legacy-sequence import was not needed and was not attempted (the importer's `FireSequenceCache.cpp` is being edited
by another engineer today), so the banner sees 0.125 s of flow and holds the last frame after that.

- **Model** (`aerodynamic_drag`): gas velocity sampled trilinearly at each banner vertex; density from the
  temperature by the ideal gas law at 101325 Pa with the molar mass of air (0.028964 kg/mol) — an approximation:
  the products' mean molar mass is close to air's for a lean methane flame, and the hot core is where it matters
  least; quadratic drag on the velocity relative to the cloth, C_n 1.2 (flat plate), C_t 0.02. **Preview**, by the
  adapter's own label, whatever the policy (tested: implicit stays preview, the limit names the model).
- **Run** (`SimulationPlumeBannerTest`, skipped when no cache exists): a 0.4 m silk banner, 11 × 11, pinned along
  two edges, at 55% of the grid height (1.14 m) centred on the updraft's momentum-flux centroid at that height;
  `CachedPortFeed` member at time offset 10.466 s; 0.4 s in 1.2 s. Mean vertical drag traction over the run
  +0.008 Pa (silk weighs 0.69 Pa/m²; the fastest updraft at that height is 1.45 m/s, but the banner sees the
  plume's edge in this 3-frame snapshot); the banner's centre ends 6 mm below its still-air sag at 1× drag and
  12 mm above it at 10× — the load is small and not uniformly upward here. This demonstrates the mechanism and
  the keying, not a validated response.
- **Keys (the M4 gap).** The banner's key covers the fire cache's key (and, the cache being foreign, its manifest
  digest), the time offset and the adapter's identity: a different drag coefficient or upstream changes it, and
  it differs from the standalone banner (tested). The scene form is `port_source surface_load from fire.volume via
  aerodynamic_drag temperature_channel=temperature time_offset 10.466`
  (`scenes/Tests/Simulation/plume_banner_from_cache.RISEscene`, cache path a documented placeholder); `rise-sim
  bake <scene> banner` baked it in 2.6 s (31 frames, preview), with `fire` reported as a cache, not baked.

## 9. Limits, and what was not done or verified

- **Momentum balance needs backward Euler for an exact cloth momentum**; with BDF2 the conserved discrete quantity is
  (3pₙ − pₙ₋₁)/2, which equals p only once forces stop. Rayleigh mass damping removes momentum by design (off in
  the conservation case).
- **Staggered coupling** is first order and can inject energy; it is labelled preview.
- **Aitken with Jacobi** order can stall; a channel the accepting solver ignores (a collider velocity) relaxed with
  a negative factor can keep its residual above tolerance — relax only backward Gauss–Seidel links.
- **transform_source:** top-level objects only; partial caches give keys for the frames present (no follow mode);
  live in-process rigid runs do not drive objects (no live attach for transforms); an incremental edit that removes
  the line keeps the keys until a reload; the live-from-scene capture in `SceneEditController` does not mark
  driven objects (only the Job's planner does). The reference to the simulation is a String parameter (not a
  `Reference`), so the editor draws no producer edge for it.
- **Group planning** configures the group on every plan (no memo); a downstream reading a member's output
  through a scene input may be planned before the group and see the member's standalone key.
- **Bake manager** group handling compiled and covered by the existing manager tests only; no GUI check.
- **Fire:** see §8; no live fire ports (its solver cannot honour external macro steps).
- **Not run:** VS2022, Xcode, Android builds (lists edited by script); sanitizers.

## 10. Contract change log (Appendix C entries added)

See [contract Appendix C](SIMULATION_SOLVER_CONTRACT.md#appendix-c-additive-changes-after-the-freeze-contract-version-stays-1),
rows dated 2026-11-25: `IPortAdapter` `Identity`/`Fidelity`/`Scope`/`ConvertWithAuxiliary`; `PortConnection`
`adapter`/`adapterParameters`/`auxiliary`; `GroupPolicy` `relaxation`/`aitken`/`absoluteTolerance`/
`onNonConvergence`/`minimumMacroStep`; the `rise.coupling` key input; codec `transform.v1`; host-side group,
factory and evidence registries, sidecars, cloth port-driven colliders.

## 11. What the liquid solver (M7) can reuse

The group host as is (implicit Gauss–Seidel with per-channel Aitken; halving); `rigid_surface` for rigid boundaries
of a liquid; `contact_reaction`'s shape for a liquid's per-body pressure/viscous reaction (offer `force`, `point`,
`torque` per body and the rigid solver accepts it unchanged); the cloth's `collider_reaction` pattern (an impulse
accumulated over the step from the forces actually applied) for momentum-exact exchange; `aerodynamic_drag`'s grid
sampler for a liquid grid feeding a cloth; the coupling-evidence registry for a liquid ↔ rigid conservation case
(hydrostatic rest of a floating body; momentum of a dropped body).

## 12. Tests

CPU only. Fast tier (non-fire filter, `./run_all_tests.sh --tier fast --timeout 300 --filter
'[!F]*,F[!i]*,Fi[!r]*,Fir[!e]*'`): 429 built warning-free, 427 passed on the first run; the two failures were count
pins this milestone moved — `SceneEditorSuggestionsTest` (184 → 186 keywords: `rigid_simulation`,
`simulation_group`) and `RewireConnectionTest` (repeatable pure-Reference params on unaddressable categories 4 → 5:
`simulation_group.members`) — re-derived; both pass on their own afterwards (333/0 and all passed).

| Test | Checks | Time |
|---|---|---|
| `SimulationTransformTest` | golden bytes (Python `struct`), round trip, refusals, budgets, quaternion helpers, body domain, cache | < 1 s |
| `RigidSolverTest` | mass properties (box mesh = box to 1e-12, tetrahedron, icosphere), evidence and orders, labels, determinism, stop/restore, macro steps, rollback, exact impulse and angular impulse, plane bounce, scene-input body, refusals | < 1 s |
| `SimulationGroupTest` | orders of the four schemes, Aitken, failure diagnostic, halving and its floor, group checkpoint identity, held frames, keys (one-way upstream unchanged, policy keyed), labels, auto `unit_scale`, cached = live, reference solvers = `CachedPortFeed` | < 1 s |
| `SimulationPortAdapterTest` | each adapter (§4), purity, identities, refusals | < 1 s |
| `SimulationCouplingTest` | §5 conservation, labels, weak coupling at two mass ratios, trampoline both schemes, group stop/resume (`--extended`: finer steps) | 40 s |
| `SimulationGroupSceneTest` | scene plan, `rise-sim bake <scene> <group>` = programmatic, naming a member, staleness through the coupling and through `port_source`, stop/resume through the worker, sample round trip | 10 s |
| `SimulatedTransformTest` | poses, blur vs keyframed and vs time average, time-indexed path, refusals | < 1 s |
| `SimulationPlumeBannerTest` | §8 (skipped without a fire cache) | 3 s |

Existing suites touched and passing: `ClothSolverTest`, `ClothValidationTest`, `SimulationSceneTest` (487/0),
`SimulationHostTest`, `SimulationWorkerTest`, `SimulationBakeManagerTest`, `SourceHygieneTest` (five lists; IJob
vtable manifest gained `SetObjectTransformSource`), `CstDeriveGoldenTest`.
