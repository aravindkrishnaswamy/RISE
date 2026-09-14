# DL-81 — padded Sobol' collapsed every bounce-to-bounce decision pair

Closed 2026-09-14 on branch `debt-sobol`.
Ledger row: `docs/DEBT_LEDGER.md` → DL-81.
Round 1: `9bdbbd2e` (sampler) / `e6f2f8d4` (aperture stream).
Round 2, after review: `4383e8f9` (Joe-Kuo data + generator) / `9b756e8b`
(tests) / `c2b69f9b` (sampler) / `e3374705` (bands).

Code: [`src/Library/Sampling/SobolSequence.h`](../src/Library/Sampling/SobolSequence.h),
[`src/Library/Sampling/SobolDirectionNumbers.cpp`](../src/Library/Sampling/SobolDirectionNumbers.cpp)
(generated),
[`src/Library/Utilities/SobolSampler.h`](../src/Library/Utilities/SobolSampler.h),
[`src/Library/Cameras/CameraUtilities.h`](../src/Library/Cameras/CameraUtilities.h),
[`tools/GenerateSobolDirectionNumbers.cpp`](../tools/GenerateSobolDirectionNumbers.cpp).
Tests: `tests/SobolDimensionParityTest.cpp` (sampler level),
`tests/SobolSelectionChannelBiasTest.cpp` (render level),
`tests/SobolDimensionBudgetTest.cpp` (aperture placement, shipped-scene
stream budget).

---

## 1. The defect

`SobolSequence::Sample` computed

```cpp
uint32_t sobolDim = dimension & 1;          // padded Sobol
uint32_t v        = Sobol( sampleIndex, sobolDim );
uint32_t dimSeed  = HashCombine( seed, dimension );
v = OwenScramble( v, dimSeed );
```

Only Sobol' dimensions 0 and 1 were ever generated. Every other requested
dimension reused one of those two and was distinguished **only by its Owen
scramble seed**.

That is not what padding means. Padding — Burley, *Practical Hash-based Owen
Scrambling* (JCGT 2020), and PBRT-v4's `PaddedSobolSampler` — also **permutes
the sample index** per dimension group. The permutation is the part that
decorrelates. Without it the two draws are `Owen_a(v_i)` and `Owen_b(v_i)` for
the *same* base value `v_i`, and Owen scrambling is defined digit by digit: the
leading digit of the output is the leading digit of `v_i` XOR one
per-dimension bit. Two same-parity dimensions were therefore locked together at
the coarsest digit **no matter what their scramble seeds were**.

`SobolSampler::kStreamStride` is 32 — even — and `StartStream( 16 + depth )`
puts eye bounce `d`'s stream at dimension `512 + 32d`. The k-th draw of every
bounce therefore had the same parity as the k-th draw of every other bounce, so
precisely the decisions that must be independent — Fresnel reflect/refract
choice, multi-lobe selection, Russian roulette at successive bounces — were the
locked ones.

### What the row got wrong

The ledger row recorded the collapse as a roughly 5 % departure in a 3-way
joint-occupancy probe, and said an independent scramble seed decorrelated the
pair. Direct measurement over 2^20 samples says otherwise — it is total, and
the seed does not help:

```
same parity   (512,544) same seed    worst 3x3 rel dev 1.24999   dyadic 2x2 x4 = [2.0000 0.0000 0.0000 2.0000]
diff parity   (512,545) same seed    worst 3x3 rel dev 0.00002   dyadic 2x2 x4 = [1.0000 1.0000 1.0000 1.0000]
same parity   (512,544) indep seed   worst 3x3 rel dev 1.81250   dyadic 2x2 x4 = [0.0000 2.0000 2.0000 0.0000]
```

Two of the four dyadic 2×2 boxes hold everything, at double density; the other
two are empty. The `indep seed` row is collapsed exactly as hard as the
`same seed` one — the relationship is between the digits, not between the seeds.

---

## 2. The fix

Two draw shapes, deliberately different.

**`Sample()` — the `Get1D` path.** Every dimension draws from its **own**
Sobol' dimension: its own primitive polynomial over GF(2), its own initial
direction numbers, its own Owen scramble seed. Nothing is reduced modulo 2.

**`SamplePair()` — the `Get2D` path.** Genuine padding, index permutation
included: Sobol' dimensions 0 and 1 — a perfect (0,2)-net — evaluated at a
sample index permuted per dimension group, each coordinate then Owen-scrambled
with its own seed. Owen scrambling applied per coordinate preserves the net
(Owen 1995), so every `Get2D` a renderer makes is a (0,m,2)-net for every m at
once.

> **Round 1 got the Get2D half wrong and the index permutation wrong.** It sent
> `Get2D` to two *consecutive rows of the per-dimension table*, whose joint
> projection is a net only by accident: measured on round 1's own table, the
> 2310 consecutive pairs have mean t = 2.51 at m = 8 and max t = 7, and the
> eight dimension groups this doc's §4b tests average t = 3.00 with the
> thin-lens aperture pair (dimensions 2309, 2310) at t = 6 — where dimensions
> 0 and 1 are t = 0 by construction. Under the ORIGINAL
> padding those two consecutive dimensions had been Sobol' 0 and 1, so round 1
> silently dropped a property the pre-DL-81 code had. And it justified that by
> claiming the index permutation "needs the samples-per-pixel count, which a
> progressive renderer does not have at draw time". That is true of PBRT-v4's
> `PermutationElement(sampleIndex, samplesPerPixel, hash)`; it is **not** true
> of an Owen scramble of the index, which needs no parameter at all.

### Why an Owen-scrambled index is a legitimate permutation

`ScrambleIndex` is the same Laine-Karras sandwich the value side uses, applied
to the index. It is unitriangular *from the top*: output bit `j` depends only on
input bits `>= j` (the reverse/mix/reverse structure does that, the mix itself
being lower-triangular — every step is `x ^= x * even`, `x += c` or `x *= odd`).
So for every input below `2^M`, the output's bits `>= M` depend only on input
bits `>= M`, which are all zero: they take one **constant** value `c`. The map
is a bijection, hence

```
ScrambleIndex( {0 … 2^M - 1} )  =  {c·2^M … c·2^M + 2^M - 1}
```

simultaneously for **every** M — a dyadic prefix goes to a dyadic *block* of the
same length, aligned at a multiple of its length. Every such block of a
(0,2)-sequence is itself a (0,M,2)-net, so a padded group is as well stratified
as the unpermuted prefix at every sample count at once, and two groups land on
different blocks. That is the decorrelation.

It is seeded from the **dimension group alone**, never the pixel seed: all
pixels must permute the index identically or `ZSobolSampler`'s Morton-ordered
global index — the entire mechanism behind its screen-space blue noise — would
be shuffled away. Per-pixel decorrelation remains the value-side Owen
scramble's job. Group 0, the film/primary pair, is drawn at the **unpermuted**
index, so it is bit-identical to every previous version of this class.

### Direction numbers

Joe & Kuo's searched tables, `new-joe-kuo-6.21201` (criterion D(6), the authors'
recommended set, covering 21201 dimensions; SIAM J. Sci. Comput. 30, 2635-2654,
2008), fetched from
<https://web.maths.unsw.edu.au/~fkuo/sobol/new-joe-kuo-6.21201> and converted by
`tools/GenerateSobolDirectionNumbers.cpp` into
`src/Library/Sampling/SobolDirectionNumbers.cpp`. That generated file carries
their BSD-style licence verbatim (as its redistribution terms require), the
source URL, the criterion, and the exact regeneration command.

Only the **initial** direction numbers are embedded — 142066 values, 555 KiB —
not the expanded 32-per-dimension table, which would be 1 MiB of read-only data;
the expansion is one pass of XORs at first use, so first-use construction is the
smaller binary cost. The data lives in a `.cpp`, not a header, so exactly one
translation unit parses it.

The generator refuses to emit anything unless, on that run: every `m_i` is
admissible (odd, `m_i < 2^i`), every `(s, a)` is genuinely primitive over GF(2)
(by walking the powers of `x` until they return to 1), and the polynomials are
in the canonical Sobol' order — increasing degree, then increasing `a` — with no
gaps, which is what lets the emitted table be flat variable-length records with
no index. `SobolDimensionParityTest` section E re-checks all three at test time
against independently written code, and E1b additionally requires every row of
the *library's* table to equal the recurrence expanded from those records.

> **Round 1 hashed the initial numbers.** That is a valid admissible choice —
> Sobol's theorem makes the t-value depend only on the polynomial degrees — but
> it leaves the pairwise 2-dimensional projections to chance, and the residual
> landed on production pairs. Over streams 0..24 × slots 0..7 (19900 pairs), the
> hashed set collapses 163 pairs at 256 samples per pixel and 40 at 1024;
> Joe-Kuo's collapse 131 and 23, and **0 of 1536 adjacent-bounce pairs at 1024
> spp against the hashed set's 3**. Round 1's header also asserted that Joe &
> Kuo's tables "could not claim" the remaining headroom; §3 below is the
> corrected version of that argument.
>
> Round 1's one genuinely load-bearing observation stands: the initial numbers
> must **not** all be 1. With unit initial numbers every dimension of degree
> `>= d` shares the same first `d` direction numbers (`2^31, 2^30, …`), so two
> dimensions of equal degree are bit-identical for every sample index below
> `2^d` — degree 13 covers dimensions 481…1110, the whole eye-bounce range, so
> that choice reproduces the DL-81 collapse one degree class at a time at any
> production sample count.

**Dimensions 0 and 1 are bit-identical to the previous `SobolDim0` /
`SobolDim1`** — dimension 0's direction numbers are the powers of two (van der
Corput) and Joe-Kuo dimension 2, which is RISE dimension 1, is `s = 1, a = 0,
m_1 = 1`, exactly the `x+1` recurrence `SobolDim1` implements. Pinned by the
parity test's section E2, which reads the TABLE rows: round 1's E2 compared
`Sobol(i, 0/1)` against `SobolDim0/1`, and `Sobol` short-circuits both
dimensions to those same closed forms before it touches the table, so the check
passed whatever the table held.

### Table size, and what happens past the end

`kNumDimensions` = **8192** = 256 × `kStreamStride`, i.e. stream indices 0..255
each get 32 dimensions of their own. 1 MiB expanded, **2.45 ms** one-time
construction.

That number comes from the streams shipped content can actually reach.
`SobolDimensionBudgetTest` Test G recomputes it from the scene files rather than
trusting this paragraph: it reads every scene's `max_eye_depth` /
`max_light_depth` / `max_recursion` and `max_volume_bounce`, applies the walk
bounds (light `1 + d`, eye `16 + d`, BDPT 47, MLT 48, VCM `48 + i` over the eye
vertices the walk produced, doubled where the scene declares subsurface
scattering, saturated at the loops' own 1024), and requires the result to be
inside the table. Over the 459 shipped scenes the deepest is **stream 241**, in
`scenes/FeatureBased/Combined/diamond_teapot_pour.RISEscene`
(`vcm_pel_rasterizer`, `max_eye_depth 128`, default `max_volume_bounce 64`).

> **Round 1's 2311 was too small and its wrap was an alias.** 2311 covers 72
> streams, so that one scene drove VCM's per-eye-vertex NEE stream 241 — and
> `dimension mod 2311` re-aliased WHOLE STREAMS onto each other (stream 72 onto
> dimensions 2304-2310 then 0-24; stream 73 onto 25-56), which is precisely the
> DL-81 collapse for the aliased pair. **Round-2-review correction (P3-4): the
> account below this line previously had the scrambling backwards** — round 1's
> Owen seed was `HashCombine(seed, dimension)` using the *unwrapped* dimension,
> so the two aliased draws (e.g. stream 72's dimension 2304 and the wrapped
> dimension 0) WERE differently scrambled; what they shared was only the same
> pre-scramble BASE VALUE (`Sobol(index, dimension mod 2311)`), which is exactly
> the DL-81 shape — two Owen-scrambled copies of one value, not two independent
> sequences. Round 1's own justification for the constant — "the smallest prime
> above 50 × kStreamStride" — was also arithmetically wrong about primes: 1601
> is prime, so 2311 is not the smallest prime above 1600.

A dimension past the table is now Owen-permuted **by its wrap count** before the
draw, so it reads a *different* dyadic block of the same Sobol' dimension. Two
streams that wrap onto each other are decorrelated; they are not a joint net,
which is why the table is sized to keep shipped content off that path. Beyond
the table is a quality boundary, not a correctness cliff. `Sobol(index, dim)` is
public and was unguarded past the table end; it now reduces.

### The aperture stream

`BDPTCameraUtilities::kApertureSamplerStream` was 8192, justified explicitly by
the padding: *"has no dimension capacity to exhaust … dimension 8192×32 =
262144, which is just another hash seed."* Round 1 made that premise false and
moved the constant to **3322**, the smallest stream above the worst-case walk
stream (3121) whose dimension then wrapped to the end of the 2311-entry table.

The constant stays at 3322, but that wrap argument is retired: `DrawApertureSample`
draws with `Get2D`, and `Get2D` is now padded and keyed by the **raw** dimension
index (3322 × 32 = 106304). No table row is read, nothing is reduced, and no
walk stream can key the same group because none reaches stream 3322. What
survives is the requirement that the constant sit above every walk stream, which
`SobolDimensionBudgetTest` Test F asserts; Test G separately covers where the
wrap still matters, which is `Get1D` draws on deep walk streams.

---

## 3. What this does **not** fix, and why no direction numbers could

The review asked for a certification that the production dimension set is
pairwise distinct at 256 samples per pixel, with zero fully-collapsed 2×2
occupancies. **That is impossible for any direction numbers, Joe-Kuo's
included**, and the test now prints the proof beside every measurement.

Over the first `2^M` samples only index bits `0 … M-1` vary, so a dimension's
generator matrix is truncated to M columns and its **leading row** takes one of
only `2^(M-1)` values (`m_1 = 1` pins the top bit). Two dimensions with equal
leading rows have a fully collapsed dyadic 2×2 occupancy — half the boxes empty,
half at double density — for as long as the render stays below `2^M` samples.

The production set is streams 0..24 × slots 0..7 = **200 dimensions**. At M = 6
(64 spp) there are 32 leading rows to go round; at M = 8 (256 spp), 128. The
most balanced assignment there is therefore still collides:

| samples/pixel | leading rows | 200 dims: floor | measured, Joe-Kuo | measured, round 1's hash |
|---|---|---|---|---|
| 64   | 32  | **528** | 616 | 605 |
| 256  | 128 | **72**  | 131 | 163 |
| 1024 | 512 | 0       | 23  | 40  |

and on the adjacent-bounce cross-slot subset (1536 pairs — streams `(s, s+1)`,
all 8 × 8 slot combinations), Joe-Kuo gives 53 / 6 / **0** against the hash's
50 / 10 / 3. The 1024-spp row is the one that matters most: the searched tables
are what make it clean.

Two ways out were measured and rejected:

* **Relaying the dimension layout.** Mapping `(stream, slot)` to
  `slot × 256 + stream` instead of `stream × 32 + slot` moves the production set
  to a different part of the table but not a better one: 151 collided pairs at
  M = 8 against the strided layout's 131.
* **Compaction.** Dimensions 0..199 taken *consecutively* collapse only 75 pairs
  at M = 8 — close to, but not exactly, the counting floor of 72 (round-2-review
  correction, P3-6: consecutive Joe-Kuo dimensions are near-optimal, not
  floor-exact) — and **none** from 512 spp, because Joe & Kuo's search
  optimises nearby-dimension projections. A progressive renderer cannot compact:
  it does not know at draw time which `(stream, slot)` pairs the walk will reach.

`SobolDimensionParityTest` section C sweeps all of this exhaustively, by t-value
read off the generator matrices (exact, not sampled), and prints the counting
floor beside each measured count. Section C4 additionally re-derives Joe & Kuo's
own published quality claim from the table this build expands — consecutive-pair
`t <= 9` at m = 10 and `t <= 11` at m = 12 up to dimension 21201 — and measures
exactly 9 (at d = 477) and 11 (at d = 2754).

The pre-DL-81 padding had the collapse on **100 %** of same-parity pairs, which
was every bounce-to-bounce pair, at **every** sample count. Against that, 0.66 %
of production pairs at 256 spp and 0.12 % at 1024 is the change.

> Round 1's version of this section reported "0.783 % of all 2311 dimension
> pairs at 256 spp against 0.738 % for a balanced assignment, so at most ~6 % of
> headroom, and Joe & Kuo's searched tables could not claim it either". The
> percentage was over the whole table rather than the production set, the
> balanced-assignment figure was quoted inconsistently (0.734 % and 0.738 % in
> two places; the value is 0.7384 %), and the conclusion about Joe & Kuo was
> wrong on the measurement that matters — they take the adjacent-bounce subset
> at 1024 spp from 3 collapses to 0.

---

## 4. Measurements

Everything below is re-measured on round 2. Where a round-1 figure is quoted it
is labelled as such.

### 4a. Render fixture — `SobolSelectionChannelBiasTest`, 1024 spp

A lossless (`tau 1.0`, `scattering 1000000`) 2-deep dielectric box, white
Lambertian luminaire (exitance 1) directly behind, pinhole on axis at fov 10,
24×24, `pathtracing_pel_rasterizer`, `oidn_denoise FALSE`, `pixel_filter box`.
The index triple gives **channels 0 and 1 the identical index**, so `R == G`
needs no closed form at all.

Closed form per channel: `L·T²/(1-R²)`, `L = 1/π`, `R = R0(n) = ((n-1)/(n+1))²`,
`T = 1-R` — `T²` for the straight-through path times the geometric series over
the paths that internally reflect off both faces and exit, which at normal
incidence all land in the same pixel. (The DL-81 row's closed form omitted the
series. It is worth 0.81 % on the shared index, and it changes the reading of
the row's own control: the pre-fix non-dispersive control was not 0.26 % *above*
the right answer, it was 0.52 % *below* it.)

| row | | before (pre-DL-81) | round 1 | round 2 | closed form |
|---|---|---|---|---|---|
| A  dielectric, dispersive | R | 0.280992 | 0.265585 | 0.265930 | 0.266053 |
| | G | 0.250685 | 0.266349 | 0.266310 | 0.266053 |
| | B | 0.241632 | 0.244792 | 0.245642 | 0.245270 |
| | **R/G − 1** | **+12.09 %** | −0.287 % | **−0.143 %** | 0 |
| B  dielectric, non-dispersive control | R | 0.264661 (−0.523 %) | 0.266111 (+0.022 %) | 0.266040 (**−0.0047 %**) | 0.266053 |
| C  perfectrefractor, dispersive | R | 0.263745 | 0.265114 | 0.265915 | 0.266053 |
| | G | 0.252972 | 0.266981 | 0.266165 | 0.266053 |
| | B | 0.227891 | 0.246584 | 0.245598 | 0.245270 |
| | **R/G − 1** | **+4.259 %** | −0.699 % | **−0.094 %** | 0 |
| D  polished, dispersive mirror coat | R/G − 1 | +0.0060 % | −0.012 % | −0.023 % | 0 |

Counters: **12 passed / 7 failed → 19 passed / 0 failed.**

> The round-1 record said 11 / 8. It is 12 / 7, verified here by rebuilding the
> pre-DL-81 library and running the suite against it: row A's B channel reads
> −1.483 %, which is *inside* its own 1.5 % band, so that check passed.

The old numbers are a **bias**, not a truncation error: row A's R/G departure
was 11.94 % at 256 spp and 12.09 % at 1024 — it did not converge. The new
numbers do:

| spp | R/G − 1 | R dev | G dev | B dev |
|---|---|---|---|---|
| 64   | +1.286 % | +1.240 % | −0.045 % | −2.602 % |
| 256  | +0.566 % | −0.104 % | −0.666 % | +0.725 % |
| 1024 | −0.143 % | −0.046 % | +0.097 % | +0.152 % |
| 4096 | −0.082 % | +0.002 % | +0.084 % | +0.006 % |

Row D is a **consistency pin**, not a red-proof: a mirror coat at normal
incidence makes one stochastic lobe selection, and the defect needs two
selections at successive bounces to express, so it was green before and after.
Its `R/G − 1` reads +0.0060 %, −0.012 % and −0.023 % across the three builds —
three noise values around zero, which is the point; it is not evidence either
way, and "unchanged to 0.01 %" (as round 1 and the CLAUDE.md bullet put it) was
already false of round 1's own number.

### 4b. Joint occupancy and t-values — `SobolDimensionParityTest`

Joint occupancy, 2^20 samples, dimensions 512+k (eye bounce 0) against 544+k
(eye bounce 1) — the DL-81 shape:

| pair | pre-DL-81: worst 3×3 dev | dyadic 2×2 ×4 | round 2: worst 3×3 dev | dyadic 2×2 ×4 |
|---|---|---|---|---|
| (512,544) same seed | 1.24999 | [2, 0, 0, 2] | 0.00013 | [1, 1, 1, 1] |
| (512,545) diff parity | 0.00002 | [1, 1, 1, 1] | 0.00012 | [1, 1, 1, 1] |
| (512,544) indep seed | 1.81250 | [0, 2, 2, 0] | 0.00007 | [1, 1, 1, 1] |

Exhaustive t-value sweep of the production dimension set (§3), read off the
generator matrices. "Collapsed" means `t = m-1`: the two leading generator rows
are equal, so the dyadic 2×2 occupancy is half-empty at double density.

| samples/pixel | adjacent-bounce cross-slot (1536 pairs) | production set (19900 pairs) | counting floor | dims 0/1 vs production (398 pairs) |
|---|---|---|---|---|
| 64   | 53 | 616 | 528 | 12 |
| 256  |  6 | 131 |  72 |  2 |
| 1024 |  **0** |  23 |   0 |  0 |

with round 1's hashed direction numbers reading 50 / 10 / 3 and 605 / 163 / 40
on the same sweeps. Consecutive-pair t-values over the whole table match Joe &
Kuo's published D(6) claim exactly: max `t = 9` at m = 10 (first at d = 477) and
`t = 11` at m = 12 (first at d = 2754).

Every `Get2D` group tested — film, light bounce 0, four eye-bounce slots, BDPT
strategy select and the thin-lens aperture — is a (0,m,2)-net for every
m ≤ 12, against seven of those eight failing on round 1 (as low as m = 0).
Worst cross-group dyadic 2×2 deviation over 2^16 samples: 0.0119.

Counters: **52 passed / 11 failed → 63 passed / 0 failed** (round 1 vs round 2;
against the pre-DL-81 library the same suite was 17 / 18 on its round-1 form).
A subsequent round-2-review fix-up pass (P2-1/P3-1, this section) added section G's
Get2D index-scramble-collapse guard and an E3 consistency pin, with no change to
sampler behavior: **70 passed / 0 failed**.

### 4c. Convergence — two scenes, against a reference that belongs to neither

Both candidate builds are Sobol'. A reference rendered with *either* of them is
a reference rendered with the sampler under test, so round 1's "independent
truth" — the fixed build at 8192 spp with `blue_noise_sampler TRUE` — was not
independent at all: ZSobol only remaps the sample index. Measured here, the two
builds' own 8192-spp ZSobol renders of the cornell box disagree with each other
by RMSE **0.0332**, and each sits within 0.006 of its own build's 4096-spp
render. On that evidence alone *both* samplers "converge", to two different
images.

So the reference used here is a **third build**: `SobolSequence::Sample` and
`SamplePair` replaced by a three-way avalanche hash of
`(sampleIndex, dimension, seed)`, which makes the estimator plain independent
Monte Carlo and therefore **unbiased by construction**. Cornell at 32768 spp,
envmap at 16384 spp. (Scratch build, never committed.)

Both scenes: 128×128 / 400×200, `oidn_denoise FALSE`, `pixel_filter box`, EXR
`color_space Rec709RGB_Linear`, RMSE over linear RGB, **mean of 4 independent
runs ± sample standard deviation** — renders are NOT deterministic run to run
(`BlockRasterizeSequence` shuffles from `std::random_device`, and nothing calls
`srand`, so the libc-`rand`-seeded parts differ per process; two renders of the
same scene at the same spp differ in every byte).

`scenes/Tests/Samplers/cornell_pt_16spp_sobol.RISEscene`:

| spp | RMSE, pre-DL-81 | RMSE, round 2 | delta |
|---|---|---|---|
| 4    | 0.25638 ± 0.00903 (n=16) | 0.25599 ± 0.00761 (n=16) | −0.15 % ± 1.15 pp |
| 8    | 0.13982 ± 0.00428 (n=16) | 0.13718 ± 0.00515 (n=16) | −1.89 % ± 1.19 pp |
| 16   | 0.09217 ± 0.00200 (n=16) | 0.09487 ± 0.00255 (n=16) | **+2.93 % ± 0.89 pp** |
| 64   | 0.04900 ± 0.00138 | **0.04436 ± 0.00137** | −9.5 % |
| 256  | 0.03650 ± 0.00023 | **0.02280 ± 0.00068** | −37.5 % |
| 1024 | 0.03380 ± 0.00009 | **0.01296 ± 0.00017** | −61.7 % |
| 4096 | 0.03333 (n = 1) | **0.00673 (n = 1)** | −80 % |

**The old sampler stops converging on this scene**: 256 → 4096 spp is a 16×
sample increase that moves its RMSE only 0.0365 → 0.0333, because it has
converged to the wrong image; its own 8192-spp ZSobol render is 0.0332 from
truth, the new build's is 0.0054. Round 2's numbers keep falling at
roughly `N^-0.6`.

Round 1 regressed the 64-spp row by +6.6 %; round 2 turns that into −9.5 %,
which is the padded `Get2D` (§2) doing its job.

> **Round-2-review correction (P2-2): the 4/8/16-spp rows above are a
> RE-DERIVATION at n = 16 (not the n = 4 this doc originally used), because the
> review's own n = 8 re-measurement disagreed with what was here before ("16 spp
> is still +5.5 % ± 2 pp regression … the §3 counting bound is why") badly
> enough to need resolving with more repeats rather than trusting either draw.
> At n = 16 the picture is**: 4 and 8 spp show **no significant delta** (z =
> −0.13 and −1.59 against their own standard errors — the review's separately
> reported "4 spp +5.66 % ± 1.59 pp (3.6σ)" does not reproduce here, and neither
> does its "16 spp −0.33 % (no regression)"; both are single n = 8 draws from a
> non-deterministic renderer, and this scene's per-run RMSE spread is wide
> enough at these sample counts that n = 8 is not always enough to resolve an
> effect this size — see the `variance-measurement` skill's "K too small"
> pitfall), while **16 spp shows a real, reproducible regression, z = 3.3**
> (+2.93 % ± 0.89 pp at n = 16, agreeing in sign and rough magnitude with both
> the original n = 4 measurement, +5.5 %, and an independent n = 8 draw taken
> during this pass, +3.94 %). **The attribution needs softening to two
> contributors, not one**: (a) the §3 counting-floor bound on Get1D dimensions
> (already documented) and (b) the LOSS of the pre-DL-81 padding's perfect
> (0,2)-net between two CONSECUTIVE Get1D draws — pre-fix, every pair of
> back-to-back Get1D calls shared Sobol' dimensions 0/1 (correlated, but
> perfectly stratified as a PAIR); post-fix each draws its own, generally
> unrelated, high dimension, which is only well-stratified against its
> IMMEDIATE neighbours (section C's adjacent-bounce sweep), not against every
> other dimension it might land near at 16 samples per pixel. Neither
> contributor predicts the 4/8-spp result in isolation: the production Get2D
> group set's OWN index-scramble collapse (§3's new subsection, P2-1) is at its
> WORST at 4 spp (50 % of pairs) and its render-level effect there is
> statistically indistinguishable from zero on this scene, which is itself
> informative — collapse COUNT alone does not predict low-spp RMSE; the
> pre-DL-81 sampler had 100 % Get2D cross-group collapse (every draw padded to
> dimensions 0/1) and still WINS at 4 and 8 spp. This is consistent with the
> render's own variance being dominated by other noise sources at 4-8 spp on
> this particular (dielectric-free, NEE-heavy) scene, with the two-contributor
> effect only surfacing clearly once other noise has fallen enough (16 spp) to
> expose it, and presumably averaging out again by 64 spp where round 2 already
> wins decisively.

`scenes/Tests/UnifiedLighting/envmap_nee_test_pt.RISEscene` — the control, a
scene with no repeated selection to bias:

| spp | RMSE, pre-DL-81 | RMSE, round 2 | delta |
|---|---|---|---|
| 16   | 1.25397 ± 0.02751 | 1.28508 ± 0.04166 | +2.5 % |
| 64   | 0.65864 ± 0.01250 | 0.63681 ± 0.00529 | −3.3 % |
| 256  | 0.32724 ± 0.00524 | 0.31791 ± 0.00166 | −2.9 % |
| 1024 | 0.16937 ± 0.00150 | 0.19039 ± 0.00107 | +12.4 % |

Neither build shows a floor: both fall at `N^-0.47` … `N^-0.51` all the way
through, which is the expected rate for a scene this noisy, and the ±3 %
differences at 64 and 256 spp are at the edge of the measurement. The 1024 row
is **not interpretable** — the white-noise reference's own residual noise is
about 0.093 RMSE at 16384 spp here, comparable to the candidates' own error, so
that row is measuring the reference. A reference-free statistic (per-pixel
run-to-run σ over the same 4 runs, which needs no truth at all) says the same
thing:

| spp | envmap σ, pre-DL-81 | envmap σ, round 2 | cornell σ, pre-DL-81 | cornell σ, round 2 |
|---|---|---|---|---|
| 16   | 0.4840 | 0.4933 | 0.05238 | 0.05700 |
| 64   | 0.2526 | 0.2215 | 0.02290 | 0.02438 |
| 256  | 0.1176 | 0.1121 | 0.01195 | 0.01265 |
| 1024 | 0.0557 | 0.0554 | 0.005947 | 0.005717 |

so the two builds are within a few per cent of each other in run-to-run noise on
both scenes. **The cornell result is therefore a bias result, not a variance
result** — the old build's per-pixel noise there falls perfectly normally while
its distance from truth plateaus.

**"The old sampler stops converging" is specific to selection-heavy scenes.**
It is the cornell box with a dielectric-free but selection-rich path structure,
and it is the dispersion fixture of §4a; it is NOT a general property. On the
envmap control, the old sampler converges at the normal rate and the two builds
are indistinguishable.

### 4d. Cost

Isolated per-draw cost, 2×10^6 draws over a deliberately cache-hostile
2000-dimension sweep, best of 3, same harness for all three builds:

| | one-time construction | `Sample` (Get1D) | `SamplePair` (Get2D) |
|---|---|---|---|
| pre-DL-81 (padded) | — | 4.28 ns | 8.48 ns |
| DL-81 round 1 | 4.85 ms | 30.51 ns | 62.28 ns |
| DL-81 round 2 | **2.45 ms** | 32.43 ns | **20.60 ns** |

Get2D is 3× faster than round 1 because it no longer reads the table at all;
Get1D is 6 % slower than round 1 (a 1 MiB table against 289 KiB). Round 1's
published "1.81 → 7.06 ns" pair came from a different harness and is superseded
by these, which are same-harness.

End to end, the cornell-box PT render, ns per sample, mean of 4 runs (n = 1 at
4096):

| spp | pre-DL-81 | round 2 | delta |
|---|---|---|---|
| 16   | 455.9 ± 30.2 | 474.9 ± 8.0 | +4.2 % |
| 64   | 417.0 ± 4.2 | 470.4 ± 9.0 | +12.8 % |
| 256  | 421.9 ± 6.6 | 448.6 ± 5.0 | +6.3 % |
| 1024 | 418.1 ± 5.3 | 460.0 ± 7.7 | +10.0 % |
| 4096 | 425.8 | 489.9 | +15.0 % |

So **+4 to +15 % wall clock per sample**, against 1.6× lower RMSE at 256 spp and
2.6× at 1024 — a net win on equal-time as well as equal-sample terms on this
scene. The spread between rows is machine noise (the runs were sequential on a
loaded machine); take the figure as "roughly +10 %", not as a per-spp curve.

## 5. What changed for users

* **Every `SobolSampler` / `ZSobolSampler` render's noise pattern changes.**
  That is every `pathtracing_*`, `bdpt_*`, `vcm_*`, `pixelpel_*` and
  `pixelspectral_*` rasterizer. Images are not bit-comparable with pre-fix
  renders. (Round 2 changes them again relative to round 1 — the direction
  numbers, the Get2D pair and the table size all moved.)
* **Means change only where selection correlation was biasing them.** The
  affected shape is *repeated stochastic selection at successive bounces*:
  RGB dispersion in `DielectricSPF` / `PolishedSPF` / `PerfectRefractorSPF`
  (the confirmed instance, up to 12 % per channel), and Fresnel
  reflect/refract choice, multi-lobe selection and Russian roulette wherever
  they stack across bounces. A render with one selection event per path — the
  row-D mirror coat — moves by 0.03 % or less (measured: `R/G - 1` +0.006 %
  pre-fix, −0.012 % after round 1, −0.023 % after round 2; all three are noise
  around zero, none is evidence either way, which is why that row is labelled
  a consistency pin).
* **Every 2D draw is better stratified**, and cheaper: `Get2D` is a padded
  (0,2)-net pair again rather than two rows of the dimension table, which on
  the cornell-box fixture removes round 1's 64-spp RMSE regression outright
  (round 1 was +6.6 % there; round 2 is −9.7 %).
* **Thin-lens renders** additionally move because the aperture stream changed
  from 8192 to 3322 in round 1 (§2).
* No scene-file, API or ABI change. **One new source file**,
  `src/Library/Sampling/SobolDirectionNumbers.cpp` (generated; five build
  projects updated), and one new tool,
  `tools/GenerateSobolDirectionNumbers.cpp`.
* **Memory**: 1 MiB of expanded direction-number table per process (was
  289 KiB after round 1, nothing before DL-81) plus 555 KiB of read-only
  embedded initial numbers in the binary.

## 6. Sibling audit

| candidate | verdict |
|---|---|
| `ZSobolSampler` | **Same defect, fixed by the same change** — it derives from `SobolSampler` and only remaps the sample index. Round 2 is deliberately careful with it twice over: the `Get2D` index permutation is seeded from the dimension group ONLY (never the pixel seed), and group 0 — the film pair, which is what its screen-space blue noise is about — is drawn at the unpermuted index. |
| The legacy `PixelBasedPelRasterizer` / `PixelBasedSpectralIntegratingRasterizer` samplers | **Same defect, fixed by the same change** — both construct `SobolSampler` / `ZSobolSampler` directly. |
| BDPT / VCM / MLT light- and eye-subpath streams | **Same defect, fixed by the same change** — all go through `SobolSampler::StartStream`. VCM's `StartStream(48 + i)` is also what sizes the dimension table (§2). |
| `SobolSampling2D` | **Refuted.** Draws dimensions 0 and 1 only, which are bit-identical before and after — and `SamplePair` now draws that same pair for every `Get2D`. |
| `PSSMLTSampler` (primary sample space) | **Refuted — structurally different.** `idx = streamIndex + kNumStreams * sampleIndex` indexes a vector of independent uniforms; there is no base-dimension reuse to collapse. Its own known stream-aliasing overrun (`kNumStreams` = 49 vs BDPT reaching stream 48 at eye depth 32) is a separate, already-recorded debt, unaffected by this change. |
| `IndependentSampler` | **Refuted.** A plain RNG; no dimensions. |
| Photon tracers (`GlobalPel`, `GlobalSpectral`, `CausticPel`, `CausticSpectral`, `TranslucentPel`, `ShadowPhotonTracer`) | **Refuted.** Every one wraps a `RandomNumberGenerator` in `IndependentSampler`; none consumes a Sobol dimension. |
| `BDPTCameraUtilities::kApertureSamplerStream` | **Downstream consumer, fixed here** — it relied on the padding's unbounded dimension capacity. Round 2 retires the wrap half of that argument entirely: the aperture draws with `Get2D`, which reads no table row. See §2. |
| `PolishedSPF` ~:187, `PerfectRefractorSPF` ~:227 per-channel dispersion loops | **Healthy after the fix**, verified by closed-form fixture rows C and D. Both are downstream victims, not independent defects: their loops emit per-channel rays correctly, and the channel-dependent error came entirely from the selection draws. |
| Every other `ISampler::Get2D` consumer (light-surface sampling, hemisphere sampling, lens sampling, volume equiangular sampling) | **Improved, not fixed** — they were never the DL-81 defect, but round 1 had silently degraded their 2D stratification from a (0,2)-net to two arbitrary table rows, and round 2 restores it. |

---

## 7. Gate, and the bands

Run on round 2, in the `debt-sobol` worktree. Clean rebuild, zero warnings.
| suite | counter |
|---|---|
| `SobolDimensionParityTest` | 70 passed / 0 failed (63 / 0 after round 2, 52 / 11 on round 1; the round-2-review pass added section G + an E3 pin, no sampler behavior change) |
| `SobolSelectionChannelBiasTest` | 19 passed / 0 failed (12 / 7 pre-DL-81) |
| `SobolDimensionBudgetTest` | all passed — 459 scenes scanned, deepest stream 241, table covers 0..255 |
| `TranslucentSamplerDimensionCountTest` | 65544 checks / 0 failures |
| `ZSobolSamplerTest` | all passed |
| `PTGuidedSelectProbTest` | all passed |
| `MISWeightsTest` | 59 / 0 |
| `RayCasterEnvEscapeMISTest` | 91 / 0 |
| `RefractiveRadianceScalingTest` | 38 / 0 |
| `BDPTStrategyBalanceTest` ×3 | 66 / 0, 66 / 0, 66 / 0 |
| `VCMStrategyBalanceTest` | 55 / 0 |
| `EnvLightBalanceTest` ×3 | 116 / 0, 116 / 0, 116 / 0 |
| `GGXWhiteFurnaceTest` | all passed |
| `LayeredWhiteFurnaceTest` | 0 of 57 configurations failed |
| `FabricRenderTest` | 57 / 0 |
| `HairRenderTest` | 26 / 0 |
| `SSSRadianceScalingTest` | 574017 guards / 0 failed |
| `CstDeriveGoldenTest` | 452 MATCH / 0 DRIFT of 452 golden scenes; 459 corpus, 0 UNCOVERED, 0 STALE |
| `SourceHygieneTest` | 165 passed / 0 failed (scanned 338 test files) |

### The bands the review asked for, re-derived

Round 1 broke three band checks and left them for whoever next touched those
suites. Round 2 was re-measured against all three, and against the pre-DL-81
library, by rebuilding it in place (`git checkout ab65f0a6 -- SobolSequence.h
SobolSampler.h CameraUtilities.h`, rebuild, run, restore). **Two of the three
no longer move at all, and a fourth check moved instead.** Each number below is
this session's own measurement, not a quoted one.

**1. `BDPTStrategyBalanceTest`, "BDPT p99 within 60 % of PT: submerged
Lambertian floor" — NO CHANGE NEEDED, band untouched.**

| | PT p99 | BDPT p99 | relative diff | verdict |
|---|---|---|---|---|
| pre-DL-81 | 0.0131185 | 0.0163976 | 25.0 % | pass |
| round 1 | 0.00983884 | 0.0163978 | 66.7 % | **fail** (band 60 %) |
| round 2 | 0.0131184 | 0.0131184 | **0.0 %** | pass |

Round 1 tightened PT's tail by 25 % and left BDPT's alone. Round 2 restores
PT's p99 to its pre-DL-81 value to six digits (0.0131185 → 0.0131184) and
tightens BDPT's by 20 % (0.0163976 → 0.0131184) — the two now agree. Means are
unchanged throughout (PT 0.00458274 → 0.00455872, BDPT 0.00463507 →
0.00461425, both under 0.6 %) and the medians agree to five digits, so this is
a tail statistic in both directions. The reviewer's suggested `p99Tol` of 0.85
is therefore NOT applied: at 0.60 the check still catches a 2.4× tail
discrepancy, and widening it would only lose resolution.

**2. `EnvLightBalanceTest`, "VCM p99 ratio-to-PT within 5 %: env + mesh
emitter" (centre 1.5223) — NO CHANGE NEEDED, band untouched.**

| | PT p99 | VCM p99 | ratio | verdict |
|---|---|---|---|---|
| pre-DL-81 | 0.525845 | 0.800811 | 1.5229 | pass |
| round 1 | 0.527257 | 1.02090 | 1.9363 | **fail** |
| round 2 | 0.526498 | 0.799926 | **1.5193** | pass |

**3. `EnvLightBalanceTest`, "VCM p99 ratio-to-PT within 12 %: env-only
Lambertian (spectral, hwss=true)" — RE-CENTRED**, (1.4135, 1.4953, 1.4293) →
**(1.0207, 1.0722, 1.0266)**, tolerance unchanged at 0.12.

| | ch0 | ch1 | ch2 |
|---|---|---|---|
| VCM mean, pre-DL-81 → round 2 | 0.603802 → 0.605365 (+0.26 %) | 0.602260 → 0.603749 (+0.25 %) | 0.597455 → 0.599776 (+0.39 %) |
| VCM p99 | 0.885956 → 0.638116 (**−27.97 %**) | 0.887997 → 0.635582 (−28.43 %) | 0.886408 → 0.641908 (−27.58 %) |
| VCM max | 0.909480 → 0.657996 (**−27.65 %**) | 0.903084 → 0.654627 (−27.51 %) | 0.958314 → 0.678060 (−29.25 %) |
| PT mean | 0.480815 → 0.482088 (+0.26 %) | 0.496063 → 0.496028 (−0.01 %) | 0.513027 → 0.514332 (+0.25 %) |

**Reshape, not bias**: VCM's max fell 28 % while its mean rose 0.3 %. That is
fewer bright outliers carrying the same energy — a firefly reduction. The MEAN
band on the same row (centres 1.2477, 1.2105, 1.1670 ± 6 %), which is the
statistic that pins VCM's transport bias, passes untouched in both builds.

**4. `EnvLightBalanceTest`, "BDPT p99 ratio-to-PT within 5 %: env-only
Lambertian" (RGB) — RE-CENTRED**, 1.5347 → **1.4508**, tolerance unchanged at
0.05. This one round 1 did not break; round 2 does.

| | mean | p99 | max |
|---|---|---|---|
| PT, pre-DL-81 → round 2 | 0.500023 → 0.500001 (**−0.004 %**) | 0.503460 → 0.524910 (+4.26 %) | 0.504749 → 0.527494 (+4.51 %) |
| BDPT | 0.642473 → 0.641854 (−0.10 %) | 0.773977 → 0.761689 (−1.59 %) | 0.803207 → 0.793176 (−1.25 %) |

**Reshape, not bias**: both means are unchanged to a tenth of a per cent, and
both mean bands pass. What moved is PT's own 99th percentile, up 4.3 % — this
row renders at 8 samples per pixel, which is where round 2 is measurably (and
only slightly) noisier, exactly as §4c's 16-spp cornell row shows. BDPT's
+28.5 % mean bias against a truth-referenced PT, which is what this row exists
to pin, did not move.

### How the centres were derived

Per this suite's own recipe, recorded in the `kBiasEnvOnlySpectralHWSS`
comment: **the mean of 4 independent runs**, with the tolerance required to be
at least 3× the run-to-run spread. Measured spreads on round 2: 0.10 % for the
BDPT p99 ratio (tolerance 5 %, rule wants ≥ 0.3 %) and ≤ 2.95 % for the three
VCM p99 ratios (tolerance 12 %, rule wants ≥ 8.9 %). Both tolerances are left
as they were. After re-centring, `EnvLightBalanceTest` was run three more
times: **116 / 0, 116 / 0, 116 / 0**.

### A note on the two suites' pre-fix state

Both are fully green on the pre-DL-81 library — `EnvLightBalanceTest` 116 / 0,
`BDPTStrategyBalanceTest` 66 / 0, measured this session on this branch with
`SobolSequence.h`, `SobolSampler.h` and `CameraUtilities.h` reverted to
`ab65f0a6` and the library rebuilt. Every band this slice touches is a p99
tail statistic; no mean check moved in either suite, in either round.

---

## 8. Round-2-review fix-up (this pass) — P2/P3, no sampler behavior change

The round-2 review found no P1s. This pass addresses its P2/P3 findings.
**Nothing in `SobolSequence.h`'s runtime code changed** — every render this
class produces is bit-identical to round 2's (`822006ce`). The changes are:
documentation corrections, one new comment-level design note, and two new
test guards (one of which is a documented non-fix).

**P2-1 (index-scramble entropy at low spp) — measured, NOT fixed, and here is
why.** `ScrambleIndex`'s own collapse at low M has the SAME shape as the
direction-number floor in §3, just one level up (Get2D GROUPS instead of
Get1D dimensions): over the production Get2D group set (streams 0..24 x slots
0..7 plus the BDPT strategy-select group, 201 groups, 80400 (group,
coordinate) pair combinations), measured collapse is 50.000 % at 4 spp,
12.400 % at 8, 0.776 % at 16, ~0.02 % at 32, 0 % at 64+
(`SobolDimensionParityTest` section G). These are, to three decimal places,
the pigeonhole floor for a family of 2^(2^(M-1)) possible leading-digit
functions (50 % / 12.5 % / 0.78 % at M = 2/3/4) — **the current `ScrambleIndex`
is already at the floor**, not merely close to it. A two-independent-round
composition of `ScrambleIndex` was implemented as a scratch candidate and
measured against the same sweep: M=2 unchanged (50.000 %), M=3 slightly WORSE
(12.517 % vs 12.400 %), M=4 unchanged (0.776 %), M=5 measured 0/80400 against
the baseline's 14/80400 — compatible with Poisson noise at an expected count
of ~2.45, not a reliable win. No change was kept.

Render-level: re-measured the cornell-box fixture (§4c) at n = 16 rather than
trusting either this doc's original n = 4 or the review's own n = 8 draw,
because the two disagreed on WHICH spp regresses. See §4c's inline correction
for the full table and reasoning; short version: 4 and 8 spp show no
significant delta at n = 16, 16 spp shows a real +2.9 % regression (z = 3.3),
and collapse count alone does not predict this — the pre-DL-81 sampler had
100 % Get2D collapse and still wins at 4-8 spp.

Per SobolSequence.h's new "Get2D's index-scramble collapse at low spp"
section: the bound is now stated with its 2/2^(2^(M-1)) form (was
unqualified), section G guards the measured counts against regression, and
`SobolDimensionParityTest` section E gained an E3 pin (see P3-1 below).

**P2-2 (§4c wrong spp / wrong attribution)** — corrected in §4c directly: the
16-spp row is re-measured at n = 16 (was n = 4), a 4-spp and 8-spp row were
added (also n = 16), and the attribution is now two contributors (the §3
Get1D counting floor AND the loss of the pre-fix padding's Get1D pairwise net)
rather than one, with the evidence that collapse count alone does not predict
low-spp RMSE (pre-DL-81's 100 % Get2D collapse still wins at 4-8 spp).

**P3-1 (`Sample`/`SamplePair` dimension-0 coincidence)** — confirmed exactly
as reported (`SamplePair(i,0,seed).outU == Sample(i,0,seed)` and
`.outV == Sample(i,1,seed)`, bit-for-bit, dimension 0 only). **Not fixed**:
the suggested tag (`HashCombine(seed, dim, kPairTag)`) would also retag group
0's own seed, breaking the documented "bit-identical to every version of this
class" guarantee for the film/primary pair — the one place that guarantee is
explicit and load-bearing (every `SobolSampler` render's primary-sample jitter
would shift for a purely dormant, "not live today" coincidence). Documented
instead, in `SamplePair`'s header comment and as a new pin,
`SobolDimensionParityTest` section E3, so a future change to either function
makes a conscious choice.

**P3-2 (DL-82 stale citations)** — DL-82's row now cites round 2's commits
(`4383e8f9`/`9b756e8b`/`c2b69f9b`/`e3374705`) instead of round 1's, and its
residual-percentage figure is corrected from 0.78 % to the actual measured
production-set count, 0.66 % (131/19900 at 256 spp).

**P3-3 (568 KiB → 555 KiB)** — 142066 values x 4 bytes = 568264 bytes =
554.95 KiB, which rounds to 555 KiB, not 568. Fixed everywhere it appeared:
this doc (twice), `SobolSequence.h` (three places), `docs/DEBT_LEDGER.md`
(DL-81 row), `CLAUDE.md`.

**P3-4 (wrap-scramble sentence backwards)** — §2's account of round 1's
`dimension mod 2311` wrap said the two aliased draws "were not even
differently scrambled"; they WERE differently scrambled (round 1's Owen seed
used the unwrapped dimension) — what they shared was the same pre-scramble
BASE VALUE, which is the actual DL-81 shape. Corrected in place.

**P3-5 (`SobolDimensionBudgetTest` two bugs)** — (a) `SceneDepthBound`'s
no-declared-depth default was always `BDPTPelDefaults::maxEyeDepth` (8), even
for a `pixelpel_rasterizer` scene (whose own default, `maxRecursion`, is 10)
or an `mlt_*` scene (`maxEyeDepth` 10); now keyed to the rasterizer chunk the
scene actually declares, falling back to `PathTracingIntegrator`'s own
runtime default (128 — PT has no scene-level depth-cap parameter at all) when
none of the depth-declaring rasterizers is recognised. (b) the
`max_volume_bounce` line match required a literal trailing SPACE
(`compare(0, 18, "max_volume_bounce ")`), silently missing every real,
tab-separated scene file and always falling back to `StabilityConfig`'s
default instead of reading the scene's own override; now matches on the key
alone, same convention as the other three depth keys, letting `strtoul`'s own
whitespace skip handle the separator. Neither bug moved
`SobolDimensionBudgetTest`'s Test G result (still 459 scenes scanned, deepest
stream 241 in `diamond_teapot_pour.RISEscene`, which declares its depths
explicitly) — both were silent-default bugs on scenes this specific test
happens not to depend on for its worst case, not a correctness regression in
the shipped table-size bound.

**P3-6 ("75 is the floor")** — §3's compaction bullet called 75 (the measured
consecutive-dimension collapse count at M = 8) "the floor"; the actual
counting floor for 200 dimensions in 128 boxes is 72. Consecutive Joe-Kuo
dimensions are near-optimal, not floor-exact. Corrected in §3 and in
`SobolSequence.h`'s mirroring comment.

### Gate (this pass)

Same suites as §7, re-run after every fix above; clean rebuild, zero
warnings. Since no sampler runtime code changed, every render-level suite's
counters are IDENTICAL to §7's round-2 numbers (re-confirmed, not assumed):
`SobolDimensionParityTest` 70/0 (was 63/0; see above), `SobolDimensionBudgetTest`
all passed (Test G unchanged: 459 scenes, deepest stream 241), and every
other suite in §7's table unchanged.
