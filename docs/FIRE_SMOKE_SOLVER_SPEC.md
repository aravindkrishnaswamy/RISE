# Fire & Smoke Solver — Phase-C Implementation Specification

**Status:** DRAFT, extracted 2026-07-28 from `FIRE_SMOKE_DESIGN.md` §3.7.
**Audience:** whoever implements the Phase-C simulator. This is a *pinned
implementation contract*, not a design rationale — the design decisions and
their justifications stay in
[FIRE_SMOKE_DESIGN.md](FIRE_SMOKE_DESIGN.md) §3.7, which this document
elaborates and must not contradict.

## Why this is separate

§3.7 states the numerical scheme's decisions: source-packet freezing,
projected Heun with paired low/high fluxes differing only in advection, a
shared FCT limiter over the combined update, nullspace projection for elemental
closure, and MAC projection against the stored face density. Those are design
choices with consequences, and they belong in the design.

What follows is the exact operator schedule, tableau, stencil rules,
basis-qualification format, and RED fixtures that implement them. It is
deliberately over-specified — it exists so two implementers produce the same
solver, and so a reviewer can check an implementation against something
precise. That precision is a liability inside a design document (it buries the
decisions) and an asset here.

## Conformance

An implementation conforms if it satisfies every rule below **and** the
invariants restated in `FIRE_SMOKE_DESIGN.md` §3.7. Where this document and
the design disagree, the design's *decision* governs and this document is in
error and must be corrected.

---

## Operator schedule and tableau

- **Time stepping and exact operator schedule:** a local finite-step map is
  evaluated once, but its result is consumed as a frozen average source inside
  the same conservative projected advance that supplies expansion. A
  fixed-volume accepted source update followed by projection is RED.

  1. From accepted Uⁿ, build one **provisional scratch trajectory** over Δt.
     Run the timestep-independent ignition/CFT trial, form the exponential
     primary and pre-existing-soot oxidation candidates, allocate their shared
     O₂, and update products/inventories/ℋ_s in scratch. Newly formed soot is
     not an oxidation candidate. Invert ℋ_s→T in scratch (no EOS gate is
     applied to this deliberately unexpanded local trajectory), then apply the
     finite-step phase map and invert T, then the exact §3.5 radiation map.
  2. Difference the final scratch state from Uⁿ to obtain one conservative
     source packet ΔU_src: every Δq_k, Δc_i, Δℋ_s, and the diagnostic accepted
     q̇‴_gas/q̇‴_sox/q̇‴_lat/q̇‴_rad. The packet must pass the algebraic mass,
     element, chemical-potential, and energy ledgers. Define the frozen average
     S̄_src=ΔU_src/Δt; never rerun a finite-step map at an RK stage and never
     apply ΔU_src outside the coupled advance.
  3. Advance the scalar/energy vector Q=(ρ_totZ,{q_k},{c_i},ℋ_s) and conservative
     momentum M with this exact projected-Heun tableau. Let f^{L/H}(Q,u) be the
     paired low/high **oriented physical face-flux vectors** for conservative
     advection+diffusion/J_h+conduction (no local source). For cell c, outward
     incidence σ_cf, face area A_f, and cell volume V_c, define once

     > L_c(f)=−V_c⁻¹Σ_{f∈∂c}σ_cf A_f f_f.

     Every scalar update below uses L(f); bare addition of a face flux to a
     cell state is dimensionally invalid. The pair differs **only in
     advection**. Define one centered physical nonadvective face flux f_N(Q)
     containing the ρ_totZ diffusion flux J_Z, every constituent diffusion flux
     J_j, J_h, and conduction, and
     set

     > f^L=f_adv^donor+f_N,
     > f^H=f_adv^MC-MUSCL+f_N.

     On the uniform orthogonal baseline, every diffusive/conductive normal flux
     uses the centered two-cell difference divided by center spacing and the
     harmonic mean of its positive cell transport coefficient (ρ_totD for
     constituent diffusion, k_eff for conduction); prescribed boundary fluxes
     use the same one-sided ledger value in both candidates. Form the raw
     mass/Z diffusion vector `J_tilde=(J_Z_tilde,{J_j_tilde})` with those same
     centered stencils, then apply the pinned
     `nonadvective_flux_projection_v1`: the qualified record carries a complete,
     rank-certified orthonormal basis N_C for

     > C=[A; (0,1,...,1)],

     in the same canonical format and with the same dimension, exact-rank-
     factorization, exact rational nullspace/projector certificate, pivot-minor,
     null-residual, and orthonormality checks as N_A. Set
     `J=N_C N_C^T J_tilde`; no sequential species correction or
     implementation-chosen constrained solve is permitted. **This projection
     realizes §3.3's structural identity under fp64** — with exact
     cell-centre states it is a no-op (§3.3), so it corrects rounding and
     limiter-tolerance residual only, never the flux model. Consequently every
     primal face satisfies, within the same checked fp64 forward-error envelope,
     both Σ_jJ_j=0 and the full affine tangent identity

     > E J_j - b(0)Σ_jJ_j - [b(1)-b(0)]J_Z = 0.

     (Here `E J_j` denotes E applied to the ordered constituent-flux vector.)
     T_f is the arithmetic face temperature, and
     J_h,f=Σ_j h_s,j(T_f)J_j,f. These are the complete
     face averaging/stencil rules; no alternative low/high diffusion stencil is
     permitted. The explicit diffusion CFL must make the resulting donor+
     physical-flux low-order state feasible, otherwise Δt is rejected/reduced.

     Thus α multiplies only f_adv^H−f_adv^L; J_j^L=J_j^H=J_j and the accepted
     nonadvective flux is exactly f_N for every α. Preserve the gas advective
     subflux Φ_g^{L/H}=Σ_{k∈gas}f_adv,k^{L/H} and the single physical
     J_g=Σ_{k∈gas}J_k. After the scalar limiter accepts Φ_g, the pinned I_i
     operator forms K; it forms G directly from J_g. Let A_hat(Q,u) be the remaining nonpressure
     momentum RHS including stress, buoyancy, and the packet-derived common-velocity phase
     source **u S̄_ρ,phase** required by §3.2 (boundary injection momentum remains
     a boundary flux and uses the same prescribed gas mass flux and velocity
     in K). On the MAC grid this source has the mandatory componentwise form
     **(A_phase,r)_i=u_r,i I_ρ,i(S̄_ρ,phase)** using the same boundary-aware
     density restriction as stored momentum; an unstaggered interpolation is
     forbidden. Momentum advection is not also present in A_hat. Every projection
     below uses the same staggered density as momentum storage—never
     I_ρ(1/ρ), a harmonic density, or another coefficient placement. With
     ρ_g,i=I_ρ,iρ_g, boundary-aware MAC gradient G_π and its conservative
     divergence D,

     > u_i†=M_i†/ρ_g,i,
     > D diag_i(1/ρ_g,i) G_ππ=(D u†−S_div)/Δt,
     > M_i=M_i†−Δt(G_ππ)_i,
     > u_i=M_i/ρ_g,i.

     D/G_π use §3.6's pinned ghost/open-face boundary classes and are the
     adjoint finite-volume pair used by the multigrid operator. A
     discontinuous-density test distinguishes inverse-of-I_ρρ from
     I_ρ(1/ρ) and harmonic alternatives.

     with §3.6's converged open-face active set for R0/R1 and its fixed
     indicator-averaged boundary plus endpoint-class rule for π₂. π₀ and π₁ are auxiliary stage
     multipliers and discarded. π₂ is the pressure impulse divided by Δt for
     the accepted Heun advance: a second-order approximation to the
     **step-average dynamic pressure**
     p̃_bar^{n+1/2}=Δt⁻¹∫_{t_n}^{t_{n+1}}p̃(t)dt, not endpoint p̃ⁿ⁺¹.

     - **R0/predictor:** coupled-project Mⁿ to u₀ at Qⁿ; assemble
       f₀ with its Φ_g,0/J_g,0 subfluxes and A_hat,0. Apply the shared FCT limiter to the full Euler face flux
       over Δt with S̄_src and form
       Q*=Qⁿ+Δt[L(f₀,accepted)+S̄_src]. Form the unprojected momentum
       **M*†=Mⁿ+Δt[A_hat,0−D_i(K₀+G₀)]** componentwise (not a
       ρ_g(Qⁿ)u₀ base), where K₀/G₀ are built by applying α₀ to their primal
       advective Φ_g correction for K₀, using the physical J_g,0 directly for
       G₀, then I_i and the u₀ arithmetic factor. Invert Q*→T/EOS and reject
       infeasibility.
     - **R1/corrector sample:** coupled-project M*† at Q* to u₁; assemble
       f₁ with its Φ_g,1/J_g,1 subfluxes and A_hat,1=A_hat(Q*,u₁).
     - **Heun commit:** form the combined low/high face-flux pair
       (f₀+f₁)/2 and run a **new** shared FCT solve on that combined flux—not
       the predictor α values—to commit

       > Qⁿ⁺¹=Qⁿ+Δt[L(((f₀+f₁)/2)_accepted)+S̄_src],
       Apply that same primal α_H separately to each stage subflux, e.g.

       > Φ̂_g,r(α_H)=Φ_g,r^L+α_H(Φ_g,r^H−Φ_g,r^L), r∈{0,1},

       Only then define the accepted Heun dual fluxes

       > K_H,i=½Σ_{r=0}¹ ū_r,i I_i[Φ̂_g,r(α_H)],
       > G_H,i=½Σ_{r=0}¹ ū_r,i I_i[J_g,r].

       Thus scalar and momentum Heun fluxes share the exact accepted primal
       gas-mass correction even when α_H varies across the interpolation
       stencil. Then

       > Mⁿ⁺¹†=Mⁿ+(Δt/2)(A_hat,0+A_hat,1)−Δt D_i(K_H+G_H).

       Finally coupled-project Mⁿ⁺¹† at Qⁿ⁺¹ to u₂ and store componentwise
       M_iⁿ⁺¹=I_ρ,i[ρ_g(Qⁿ⁺¹)]u₂,i and p̃_bar^{n+1/2}=π₂. Thus π₀/π₁ affect only their
       stage velocities and are not retained in the committed momentum; π₂ is
       the sole committed pressure impulse. No endpoint pressure is stored
       or inferred from this multiplier. This final projection targets the
       endpoint S_div,2 defined by the R2 solve below; the next step's R0
       reprojects against its newly built packet. Q* and Qⁿ⁺¹ are separately
       T/EOS-inverted and gated. The source appears once in each scalar formula,
       so its net scalar increment is exactly ΔU_src. Momentum receives the
       Heun average ½Δt(u₀+u₁)I_ρ(S̄_ρ,phase) in A_hat plus the exact accepted K and
       diffusive G fluxes above. Omitting any one changes velocity under gas
       advection, gas/aerosol transfer, or constituent diffusion.

     “Coupled-project” is a specific Picard solve because the physical
     diffusion/enthalpy/conduction flux f_N affects S_div while S_div affects
     velocity-dependent SGS coefficients and the advecting velocity. **R0
     loop:** project u₀, build the predictor (f₀,A_hat,0)
     pair/RHS, solve its Euler α₀, recompute S_div,0 from exactly the physical
     f_N plus packet increments, and repeat all four; α₀ still affects
     the predictor's accepted advection but never f_N. **R1 loop:** at fixed
     converged Q*, project u₁, build f_N,1 and A_hat,1, recompute S_div,1 directly
     from f_N,1 plus S̄_src, and repeat. No α₁ exists: the corrector-stage
     advective candidate is needed later by the combined Heun FCT solve, but it
     is irrelevant to the instantaneous divergence target. R0 converges when
     cellwise S_div, every projected face mass flux, and α₀ change below the
     projection/EOS tolerances; R1 uses the same criteria without α.

     After both stage loops converge, the Heun commit performs the new combined
     (f₀+f₁)/2 FCT solve once, forms Qⁿ⁺¹, and derives **S_div,Heun only from
     the Heun-averaged physical nonadvective f_N components (which are identical
     in the low/high pair and independent of α_H), plus S̄_src**. Advective constituent
     or enthalpy flux is never inserted into S_k or H. S_div,Heun closes the
     integrated scalar/EOS ledger but is **not** an endpoint projection target.

     The final projection is an **R2 endpoint diagnostic Picard solve** at fixed
     committed Qⁿ⁺¹ and fixed Mⁿ⁺¹†. For each candidate u₂ it assembles a fresh
     endpoint physical nonadvective flux f_N,2(Qⁿ⁺¹,u₂) and derives S_div,2
     directly from that flux plus S̄_src. No α₂ or virtual FCT update exists.
     The source term is necessarily
     the frozen step-average packet—the declared first-order source-split
     approximation—but transport coefficients and fluxes are endpoint values.
     Project Mⁿ⁺¹† against S_div,2 with §3.6's π₂ boundary rule and repeat until
     S_div,2, every projected face mass flux, and the open-face active set
     converge. f_N,2 is diagnostic: it never alters Qⁿ⁺¹, K_H, G_H, or
     Mⁿ⁺¹†. Thus u₂ is the constrained accepted endpoint and starting momentum
     for the next step, while π₂ remains the accepted step-average pressure
     impulse; neither quantity is mislabelled as the other. Feeding f_N,2 back
     into the Heun commit would define a different tableau and is forbidden.
     A fixed iteration count with an unconverged R0/R1/R2 or open-boundary solve is
     forbidden; failure rejects/reduces Δt. If the frozen packet makes either
     low-order state infeasible after fluxing, likewise reject/recompute rather
     than clamp.

     V2's reacting manufactured case records every Picard residual and includes
     a limiter-active, varying-S_div nonzero heat/species source whose analytic expansion
     distinguishes R0 advection by u₀ from advection by the unprojected Uⁿ or
     M*† velocities. It separately asserts R0 and R1 residual convergence and
     formal order, then verifies S_div,Heun against the combined accepted
     flux. Transport-only cases retain Heun's order; swapping the
     named velocities, using ρ_gu₀ rather than Mⁿ as either unprojected momentum
     base, or omitting π₂ is RED. Its analytic pressure comparison
     uses the exact interval average, including a linearly time-varying gradient
     whose endpoint differs by 2× from its average; interpreting π₂ as endpoint
     is RED. A periodic Galilean phase-transfer fixture uses smooth zero-mean
     condensation/evaporation, constructs a compatible zero-mean S_div and
     analytic expansion velocity (for example S_div=S₀sin(kx),
     u_exp=−(S₀/k)cos(kx)e_x), then runs a Galilean-transformed copy with
     S_U(x,t)=S(x−Ut,t). Compare at corresponding coordinates:
     u_U(x,t)−U against u(x−Ut,t), and every scalar likewise, requiring the
     difference to converge at the scheme's stated order under Δx/Δt
     refinement—not pointwise equality on the unshifted grid. A separate local
     packet unit fixture, with no advection/projection, applies Δq and momentum
     uΔq to M=qu and requires (M+uΔq)/(q+Δq)=u to roundoff; omitting
     uS̄_ρ,phase is its RED control. Its multidimensional staggered companion
     uses spatially nonuniform phase transfer and verifies every component of
     M_i−U I_ρ,iρ_g remains roundoff-zero; restricting the packet with any
     operator other than I_ρ,i is RED. A second periodic fixture starts with
     uniform velocity and a nonuniform gas/aerosol loading gradient under pure
     constituent diffusion; M/ρ_g must remain uniform, and its Galilean-shifted
     copy must converge at the same order. A limiter-active multidimensional
     periodic free-stream fixture advects a nonuniform gas density/composition
     blob at uniform U and requires M−Uρ_g to remain roundoff-zero regardless
     of α; its Galilean-boosted copy must converge identically. An independent
     central momentum flux is its RED control. Omitting u⊗J_g or accepting it
     from any J_g other than the single physical constituent flux is also RED.
     A limiter-active mixed advection/diffusion case varies α spatially while
     requiring f_N, J_g, S_div, and G to remain identical to the pinned centered
     physical flux; inventing distinct low/high diffusion stencils is RED. A time-varying
     pure-diffusion manufactured case makes endpoint S_div,2 differ from
     S_div,Heun and requires second-order endpoint divergence; projecting
     against S_div,Heun is RED. A stiff exact source-packet case proves R1/R2
     do not apply any scalar update or consume ΔU_src a second time.
     The free-stream case exercises all three staggered velocity components,
     boundary-adjacent stencils, and spatially varying α, and directly checks
     D_iI_i=I_ρ,iD. A limiter-active record whose fuel/ambient elemental vectors
     overlap separately forces the lower and upper ρ_totZ rows; accepting Z<0
     or Z>1 is RED even when every q_j and element equality passes. Basis tests
     load the exact recorded bytes/hash/rank/order, verify the exact upper-rank
     factorization, pivot-minor lower-rank certificate, and both residual bounds,
     and show a rotated valid nullspace would produce a different MC slope; the
     near-rank-deficient `diag(1,δ)`/declared-rank-1 counterexample and the
     correct-rank wrong-subspace fixture
     `A=[[1,0,-δ,-1],[0,0,δ,0],[-1,0,0,1]]`, `δ=2^-60`, `r=2`,
     `N=[e_2,e_3]`; both must reject, as must deleting one valid basis column,
     recomputation, or accepting a changed hash is RED. A multielement
     pure-diffusion fixture requires both C J=0 and nonzero J_Z at every face;
     omitting J_Z or applying a ΣJ-only correction that is not tangent to A is
     RED. The same fixture uses nonzero aerosol loading and checks the analytic
     flux magnitude, so substituting ρ_gD for the required ρ_totD is RED even
     if a later projection makes C J=0. Spatially uniform
     nonzero S_div on a periodic domain is explicitly invalid because
     ∫Ω∇·u dV=0; that case must be rejected, not used as a preservation test.

  This source freezing is explicitly first-order in noncommuting local/transport
  operators; the existing Δt-halving gates must show first-order source-split
  convergence while transport-only manufactured cases retain Heun's order. An
  analytic homogeneous y′=−y/τ **source-packet** fixture at Δt/τ≫1 must produce
  ye^{−Δt/τ}, not the ½y stiff-limit left by reevaluating an exponential map as
  an RK derivative. A constant-pressure heated control-volume fixture must
  simultaneously conserve the packet plus face fluxes and close the EOS.
  Step size: min of advective CFL, the
  buoyant-acceleration limit Δt ≲ √(2δx/g′₊) (g′₊ = max(g′, 0) per §3.3 —
  the signed form goes non-real over cold dense fuel; the limit is simply
  inactive where g′₊ = 0), and
  the explicit diffusion limit Δt ∝ δx²/ν. All molecular/SGS diffusion and
  conduction are explicit in this arc; subcycling or an IMEX replacement is a
  separately designed optimization, not an implementation choice hidden under
  “Heun.” Radiative cooling alone uses §3.5's local backward-Euler source map.
  Sim Δt is
  decoupled from frame write cadence (§8).
