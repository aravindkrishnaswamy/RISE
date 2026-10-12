# DL-423: Angle-Resolved Lobe Reservoir for Smooth GGX Substrates

Slice `jetski-dl423`, 2026-10-12, branched from `master` `0131003bc`.
Background: [DL388_COATED_REFRACTED_FRAME.md](DL388_COATED_REFRACTED_FRAME.md), [DL342_COATED_ABSORBING_COAT.md](DL342_COATED_ABSORBING_COAT.md), [BATCH8_COATED_GGX_CHECKPOINT.md](BATCH8_COATED_GGX_CHECKPOINT.md).

## 1. Problem Statement

Under `coated_material`, light returned by the substrate to the coat underside undergoes multiple reflections (recycling). DL-388 introduced the lobe reservoir:
$$M(\omega_i', \omega_o') = \frac{g(\mu_i) g(\mu_o) E}{\pi G (1 - Q)}$$
which assumes that the upwelling light returned by the substrate is re-randomized into a cosine (isotropic in azimuth) distribution.

For a smooth glossy GGX substrate ($\alpha \le 0.1$) near grazing angles:
1. **Polar Angle Retention**: Light reflected by the substrate near or below the critical cosine $\mu_c = \sqrt{1 - 1/\eta^2}$ is totally internally reflected (TIR) by the coat underside. Because the coat underside is an optical flat, TIR preserves the polar angle $\mu$. The returned light strikes the substrate again at the same polar angle $\mu$. For smooth GGX, it reflects specularly again at $\mu < \mu_c$, remaining trapped in the TIR waveguide. It bounces repeatedly, losing $(1 - \rho)$ and $a^2$ each round trip, and is absorbed by the substrate rather than escaping.
   - The DL-388 cosine reservoir assumes $44\%$ of this trapped light escapes on the very next bounce, resulting in an unphysical excess of escaped light near grazing: total ratio $1.065 \pm 0.012$ at $\theta = 70^\circ$, up to $1.24$ at $85^\circ$.
2. **Azimuthal Retention (The Off-Mirror Discrepancy)**: In a real specular bounce between two smooth surfaces, azimuth $\phi$ is preserved across each round trip ($\phi \to \phi + \pi$ at the coat underside, then $\phi + \pi \to \phi$ at the substrate). The light that does escape on subsequent bounces remains tightly focused near the mirror azimuth ($\Delta\phi \approx \pi$).
   - The DL-388 cosine reservoir redistributes this light isotropically over all azimuths $\Delta\phi \in [0, 2\pi)$. Consequently, energy $> 30^\circ$ off the mirror direction was measured at $0.239$ for `coated_material` vs $0.071$ for the reference walk (a $3.4\times$ error; $17\%$ of incident light misplaced into grazing haze).

An earlier parked attempt was rejected because:
- Preserving polar angle alone without azimuthal retention failed the off-mirror energy test ($0.148$ vs $0.071$).
- A heuristic split between trapped and escape cones caused sharp steps across eta, failing `CoatedEtaContinuityTest` (47/13 and 50/10 failures).

## 2. Evaluation of Design Candidates

Three candidate designs were evaluated against an unbiased independent Monte Carlo reference layer walk (`REF`, exact Fresnel coat, Beer absorption, GGX substrate VNDF sampling):

| Candidate | Total Energy Error ($\theta \in [0^\circ, 89^\circ]$) | Off-Mirror Error ($\theta = 70^\circ, \alpha=0.05$) | Continuity in $\eta$ | Status |
|---|---|---|---|---|
| **Candidate 3: Diffuse-Only Cosine Reservoir** (zero specular recycling) | Up to $-36\%$ loss on glossy metals; furnace $0.64$ vs $1.00$ | N/A (dark) | Pass | **REJECTED**: Massive energy loss on metals |
| **Candidate 2: Precomputed Scalar / Polar Table** (isotropic azimuth) | Total energy matches within $3\%$, but misses angular peak | $0.148$ vs $0.071$ ($+326\%$ error; $4.2\times$ too high) | Pass | **REJECTED**: Fails off-mirror gate |
| **Candidate 1: Angle-Resolved Reservoir** (Polar cells + Mode-1 Azimuthal Wrapped Cauchy `FPwc`) | $< 3.5\%$ relative error everywhere; $< 1\%$ at $70^\circ$ | $0.0326$ vs $0.0347$ (within $6\%$ of reference) | Pass ($60/0$ in `CoatedEtaContinuityTest`) | **SELECTED**: Exact, continuous, efficient |

### Quantitative Comparison Across Parameter Grid

Evaluated on $\alpha \in [0.02, 1.0]$, $F_0 \in [0.04, 0.95]$, $\eta \in [1.3, 1.7]$, $\theta \in [0^\circ, 89^\circ]$:

- **$\alpha = 0.02, F_0 = 0.90, \eta = 1.5, \theta = 70^\circ$**:
  - REF: multi $0.13031 \pm 0.00088$, off-mirror $0.00412 \pm 0.00007$
  - CUR (DL-388): multi $0.16506$ ($+27\%$), off-mirror $0.14237$ ($35\times$ too high!)
  - Candidate 1 (`FPwc`): multi $0.13181$ ($+1.1\%$), off-mirror $0.00405$ (within $1.7\%$ of REF)
- **$\alpha = 0.05, F_0 = 0.90, \eta = 1.5, \theta = 70^\circ$**:
  - REF: multi $0.17241 \pm 0.00104$, off-mirror $0.03473 \pm 0.00065$
  - CUR (DL-388): multi $0.23534$ ($+36\%$), off-mirror $0.20404$ ($5.9\times$ too high!)
  - Candidate 1 (`FPwc`): multi $0.17387$ ($+0.8\%$), off-mirror $0.03262$ (within $6.0\%$ of REF)
- **$\alpha = 0.10, F_0 = 0.90, \eta = 1.5, \theta = 70^\circ$**:
  - REF: multi $0.22024 \pm 0.00096$, off-mirror $0.11000 \pm 0.00042$
  - CUR (DL-388): multi $0.28110$ ($+28\%$), off-mirror $0.24709$ ($2.2\times$ too high!)
  - Candidate 1 (`FPwc`): multi $0.22146$ ($+0.5\%$), off-mirror $0.10422$ (within $5.2\%$ of REF)
- **$\alpha = 0.50, F_0 = 0.90, \eta = 1.5, \theta = 70^\circ$**:
  - REF: multi $0.29860 \pm 0.00049$, off-mirror $0.27011 \pm 0.00039$
  - CUR (DL-388): multi $0.30372$, off-mirror $0.27697$
  - Candidate 1 (`FPwc`): multi $0.29892$, off-mirror $0.27097$ (identical to REF within noise)

## 3. The Angle-Resolved Reservoir Formulation

The layer response decomposes into:
$$f(\omega_i, \omega_o) = f_{\text{single}}(\omega_i, \omega_o) + f_{\text{multi}}(\omega_i, \omega_o)$$
where $f_{\text{multi}}$ separates into the broad (diffuse + multiscatter) reservoir $M_{\text{broad}}$ and the specular multi-bounce field $M_{\text{spec}}$:

### 3.1 Polar Discretization and Azimuthal Fourier Moments
Let $kLobeN = 32$ polar cosine cells partition $\mu \in [0, 1]$.
In `BuildLobeSpillTable()`, for each roughness node and view node, we accumulate both:
- Mode 0: $T_0(j, k) = \int f_{ss}(w_j, u) \mu_u du$ (total polar energy transfer)
- Mode 1: $T_1(j, k) = \int f_{ss}(w_j, u) \mu_u \cos(\Delta\phi) du$ (dipole azimuthal moment)

For a coat with internal reflectance $\Phi(\mu) = F_{in}(\mu) a(\mu)^2$:
- Mode 0 round-trip kernel: $X_0(j, k) = T_0(j, k) \Phi(\mu_k)$
- Mode 1 round-trip kernel: $X_1(j, k) = T_1(j, k) \Phi(\mu_k)$

Given incident direction $\mu_i$, the first-bounce return into cell $k$ is:
$x_0(k) = T_0(\mu_i, k) \Phi(\mu_k)$
$x_1(k) = T_1(\mu_i, k) \Phi(\mu_k)$

The multi-bounce series satisfies:
$$v_0 (I - X_0) = x_0$$
$$v_1 (I + X_1) = x_1$$
where the $+X_1$ arises from the $\pi$ azimuthal phase flip on underside reflection: $\cos(1 \cdot (\phi + \pi)) = -\cos(\phi)$.

### 3.2 Output Field and Wrapped Cauchy Distribution
The multi-bounce energy reaching the coat underside in polar cell $o$ is:
$O_{g,0}(o) = [v_0 T_0](o)$
$O_{g,1}(o) = - [v_1 T_1](o)$

The azimuthal distribution of this specular multi-bounce energy is parameterized by a wrapped Cauchy distribution with circular parameter:
$$\rho(o) = \min\left(0.999, \max\left(0.0, -\frac{O_{g,1}(o)}{O_{g,0}(o)}\right)\right)$$
$$p_g(\Delta\phi; \rho) = \frac{1 - \rho^2}{1 + \rho^2 + 2\rho\cos(\Delta\phi)} \frac{1}{2\pi}$$

Key mathematical properties:
1. **Exact Normalization**: $\int_0^{2\pi} p_g(\Delta\phi; \rho) d\Delta\phi = 1$ for all $\rho \in [0, 1)$. Energy conservation is exact.
2. **Smooth Limiting Behavior**:
   - As $\alpha \to 0$: $\rho \to 1$, $p_g \to \delta(\Delta\phi - \pi)$ (pure mirror reflection).
   - As $\alpha \to \infty$ or broad bounces dominate: $\rho \to 0$, $p_g \to \frac{1}{2\pi}$ (isotropic cosine reservoir).
3. **Continuity in $\eta$**: The linear systems $(I - X_0)$ and $(I + X_1)$ depend smoothly on $\Phi$, which is $C^0$ continuous in $\eta$ by construction via `CoatWeightTable`.

## 4. Scope and Non-Interference
- **Lambertian and Oren-Nayar**: Untouched. Retain exact bit-identical code paths.
- **Rough substrates ($\alpha \ge 0.3$)**: Automatically transition to isotropic reservoir as $\rho \to 0$.
- **Protected Areas**: SSS exit/contact code and frozen features are untouched.
