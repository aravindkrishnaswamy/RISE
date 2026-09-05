# r203 — producer-kernel decomposition and the actual EOS refusal

Status: EOS repair and a scheduling experiment; **not** an affordable-producer,
resume, fixed-k, onset, focusing, window, or tier-10 performance claim.
The sealed replay and golden checkpoint are unchanged. The historical r202
evidence is not rewritten. This work is isolated on `codex/r203-producers`.

## EOS: inputs first, then the rounding proof

The retained step-44 checkpoint has SHA-256
`4a5d1c2f5bac9b23bf149f5e700668314e52658c83e22f15eef597294ca8300a`.
Its next R0 bootstrap producer refuses cell 179246, `(53,44,37)`, with bitmap
33565184 and term bitmap 3. The diagnostic copies the actual failed candidate,
its immediate Picard input, the beginning state, and source dose (nine binary32
components each), **after refusal**. It does not substitute a CPU candidate or
publish a state. The independent pressure witness consumes the original private
candidate and thermochemistry buffers. CPU mirror evaluation is diagnostic only.

The actual candidate's component bits, in canonical conservative order, are:

```
2911411995 2921228213 1049346736 1063658519 776277514
776624364 772830492 2906723385 772006681
```

The fp64 accepted-state mirror gives T = 300 K and represented-pressure ratio
0.99999997019767695, whose binary32 projection is 0.99999994039535522. The upper
midpoint of that bin is 0.99999997019767761. This is not a physical-bound
refusal. The pressure certificate's actual device expansion is:

| Component (all dimensionless pressure-ratio units) | Value |
|---|---:|
| hi | 1 |
| lo | -2.9802322387695312e-8 |
| tail | -5.1471439347565255e-16 |
| outward bound | 4.6725329510620597e-22 |

The first two components land exactly on the midpoint; the tail proves the
lower side by more than a million times the propagated bound. The old helper
only tested the bin centered on `hi`. Its own interval comparison returned
`lower_order=+1`, proving that bin is wrong, rather than reporting ambiguous
order (`2`). Renormalization can round the leading-component tie before the
trailing component decides the side. This is a **bin-search defect**, not
insufficient precision, an ambiguous predicate, or a reason to enlarge a bound.

`eos_unique_binary32_round` now tests the leading float's bin and its immediate
neighbors with the same strict interval-in-bin proof. Exact midpoint or
straddling intervals still refuse every bin. Arithmetic, pressure functional,
feasibility factors, and the r170 bound are unchanged. The r199 regression uses
64 copies of the captured candidate through the authenticated FCT/EOS surface,
asserts the candidate itself is byte-identical, then checks T and pressure
per cell against the original fp64 record. The old executable's captured
refusal is the old-bin-only mutant evidence.

### Qualification-domain accounting

T = 300 K is the first qualified lattice point; the ratio is within the earlier
0.7505–1.2495 corner scale range. The captured composition is admitted by the
unchanged affine feasibility envelope, including its tiny signed residuals.
The r199 log midpoint sweep was a sweep of **log arguments**, not an exhaustive
qualification of every composition's pressure-rounding midpoint. It could not
catch this bin-selection defect. Existing exact-pressure-midpoint REDs remain
necessary but did not cover a leading-float tie with a decisive nonzero tail.

There is also a separate coverage gap: the live inversion evaluates its 2300 K
upper endpoint even when the answer clamps to 300 K, whereas r199's log sweep
ended at 2200 K. Qualification now derives both endpoints from the sealed
case and exhausts the entire binary32 lattice and every adjacent midpoint.
The existing four accepted arithmetic corners and upper-energy-envelope REDs
remain. An attempted fifth *positive* fixture at the last temperature float
below 2300 K failed; that is not an admissible positive corner because the
upper energy test includes the existing feasibility margin. Its rejected
transcript is retained, not counted as a passing sweep. This extends log
coverage, **not** a numerical tolerance or physical limit.
The final kernel-sweep transcript records the new input count, local maximum
residual/bound, and complete Metal source/compiler/runtime identity.

### Continuation disposition

r78 requires the old and new executables to accept at least eight steps from
the same checkpoint. The old executable refuses its first candidate here;
therefore no r78 certificate can be issued for this checkpoint. The isolated
input extraction explicitly says `migration_authority=false`, never writes
continuation checkpoints or frames, and checks the original file SHA afterward.
The repaired publication accepts a formerly refused candidate. A later verdict
leg must start from zero and be newly sealed; no migration bypass was added.

## Primary cost instrument

The M4 Max supports encoder/stage-boundary timestamp sampling, not
dispatch-boundary sampling. The opt-in diagnostic therefore labels one
dispatch per encoder inside each producer command. It fails on missing names,
multiple dispatches, unsupported counters, incomplete samples, and invalid
clock intervals. Correlated CPU timestamps are already nanoseconds; no Mach
timebase multiplier belongs in the conversion. See Apple's
[timestamp conversion contract](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time).

The raw artifact retains both correlated clock endpoints and every encoder's
integer timestamps. Encoders can overlap: the analyzer calculates their union
and never calls their summed inclusive durations an exclusive decomposition.
Force-RHS encoders in another translation unit, blits, and gaps are explicitly
uncaptured. Their combined remainder was 31.01 ms across 42 baseline producer
commands. Counter observation adds 16,384 allocated bytes at the measured owner
peak; it is off by default and forbidden in the sealed verdict-run entry.
Refusal-input staging and diagnostic compilation are also opt-in, never part
of an accepted production step.

### Exploratory matched three-step measurements

These are two three-step cold-prefix experiments, not three independent full
runs or steady-state speed qualifications. Their executable SHAs and immutable
logs are preserved; they were measured before the final EOS repair. The first
tiny counter prototype used an erroneous Mach conversion and is deliberately
excluded from the evidence set. All numbers below use corrected correlated
timestamps, in milliseconds, and **inclusive kernel scope**.

| Baseline kernel | Calls | Mean ms |
|---|---:|---:|
| physical-flux payload identity | 42 | 3238.940 |
| averaged-flux payload binding | 14 | 1127.937 |
| EOS evaluation | 42 | 604.781 |
| transport payload identity | 42 | 590.189 |
| candidate payload binding | 42 | 441.907 |
| target terms | 42 | 124.608 |
| donor/MC advective pair | 42 | 7.555 |
| physical flux arithmetic | 42 | 1.278 |
| transport arithmetic | 42 | 0.631 |

The dominant pathology is the single-lane, full-payload FNV dependency chain,
not host checks or an expensive physical-flux stencil. EOS and target interval
arithmetic remain substantial independent costs; their internal arithmetic vs
obligation-recording split is **not yet measured**. No claim that all their
cost is removable tracing is supported by this profile.

The first scheduling experiment coalesces physical-flux hash reads across one
SIMD group. Every lane replays the **same original FNV word order**, and only
lane zero publishes. It does not replace payload binding with a causal label,
drop fields, combine hashes by XOR, or change identity semantics. The helper
handles the final partial group. Kernel mean falls to 2330.597 ms (1.39x);
three-step outer prefix wall falls from 255.077 to 213.185 seconds. Measured
resident-attempt device/wall p95 changes from 88192.160/89739.186 to
73268.468/74796.925 ms. Counter-enabled A/B rows match all 22 non-timing fields,
including owner identity, iteration counts, and working set. Relative to the
original counter-disabled replay, the only non-timing difference is the
explicit 16,384-byte counter allocation.

This is nowhere near the requested tens-of-ms producer class. Coalescing does
not remove the digest's serial arithmetic dependency. A versioned parallel
payload digest could preserve numerical bits and full payload binding, but
would change seal IDs. That is a separate identity-semantics decision, not an
unannounced class-A substitution; owner direction has been requested. The
previously rejected unbound causal-seal shortcut remains rejected.

The current owner gate, complete kernel sweep, local field bounds, strict
midpoint REDs, payload/parent-lineage REDs, and residency certificates must all
remain green. The full case-domain sweep and final code are sealed separately
from exploratory timing. No fixed-k variant is selected: no crossing-state
momentum-settling evidence exists yet. The next performance work must make
producers affordable before spending another verdict run on this serial cost.
