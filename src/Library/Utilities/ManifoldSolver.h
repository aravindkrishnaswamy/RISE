//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolver.h - Specular Manifold Sampling solver for BDPT.
//
//    Implements the Newton iteration method from Zeltner et al. 2020
//    ("Specular Manifold Sampling") to find valid specular paths
//    connecting two non-specular endpoints through an arbitrary chain
//    of specular (delta distribution) surfaces.
//
//    The solver works by expressing the specular constraint at each
//    vertex as a 2D equation C_i = 0 (the angle-difference between
//    the actual outgoing direction and the specularly scattered
//    direction), then using Newton's method with block-tridiagonal
//    Jacobian structure to iteratively adjust vertex positions until
//    the constraint is satisfied.
//
//    Supports both refraction and reflection at each vertex, with
//    material-specific IOR via IMaterial::GetSpecularInfo().
//
//    SMS and BDPT occupy disjoint path spaces for delta materials:
//    BDPT skips delta vertices in its MIS walk, so it cannot generate
//    the caustic paths that SMS finds.  Simple addition of SMS and
//    BDPT contributions is correct without cross-strategy MIS.
//    See docs/SMS.md for the full analysis and glossy-extension notes.
//
//  References:
//    - Zeltner, Georgiev, Jakob. "Specular Manifold Sampling."
//      SIGGRAPH 2020.
//    - Hanika, Droske, Fascione. "Manifold Next Event Estimation."
//      CGF 2015.
//    - Fan et al. "Manifold Path Guiding." SIGGRAPH Asia 2023.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MANIFOLD_SOLVER_
#define MANIFOLD_SOLVER_

#include "../Interfaces/IReference.h"
#include "../Interfaces/IGeometry.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IObject.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/ISampler.h"
#include "../Utilities/IORStack.h"
#include <vector>

namespace RISE
{
	class IScene;
	class IRayCaster;
	class IBSDF;

	namespace Implementation
	{
		/// Data stored at each specular vertex during the manifold walk.
		struct ManifoldVertex
		{
			Point3				position;		///< World-space position on specular surface
			Vector3				normal;			///< Surface normal (world space) — Phong-interpolated SHADING normal on triangle meshes; on analytical primitives it's the same as `geomNormal`.  Used by Newton's Jacobian and the chain-throughput math (which want the smooth shading normal so derivatives are well-defined across triangle edges).
			Vector3				geomNormal;		///< Geometric (face) normal in world space.  On analytical primitives identical to `normal`.  On triangle meshes this is the actual flat-triangle face normal — INDEPENDENT of Phong vertex-normal interpolation.  Used by `ValidateChainPhysics` to test wi/wo against the actual surface (not the shading approximation), so chains aren't spuriously rejected when the Phong-tilted shading normal disagrees with the real geometry — empirically this was the dominant source of `physicsFail` rejections on smooth-displaced caustics (see docs/SMS_LEVENBERG_MARQUARDT.md).
			Vector3				dpdu;			///< Position derivative w.r.t. first surface param (world space)
			Vector3				dpdv;			///< Position derivative w.r.t. second surface param (world space)
			Vector3				dndu;			///< Normal derivative w.r.t. first surface param (world space)
			Vector3				dndv;			///< Normal derivative w.r.t. second surface param (world space)
			Point2				uv;				///< Surface parameters
			Scalar				eta;			///< Surface MATERIAL'S IOR at this vertex (e.g. 1.5 for typical glass).  Kept for backward compatibility with callers that just want "the dielectric's nominal IOR".  For half-vector / Snell / Fresnel math, prefer (etaI, etaT) below — they encode the actual interface, including nested-dielectric cases where neither side is air.
			Scalar				etaI;			///< IOR on the INCOMING (`wi`) side of the interface — Walter et al. 2007 notation η_i.  This is the medium the ray is travelling FROM as it hits this vertex.  Default 1.0 (air).
			Scalar				etaT;			///< IOR on the OUTGOING (`wo`) side of the interface — Walter et al. 2007 notation η_t.  This is the medium the ray is travelling INTO as it leaves this vertex.  Default 1.0 (air).
			///<
			///< Together (etaI, etaT) replace the old `eta_eff = isExiting ? 1/eta : eta` derivation, which silently assumed the OPPOSITE side of every interface was air (IOR=1.0).  That assumption holds for a single dielectric in air (the typical SMS test case) but BREAKS for nested dielectrics — e.g. an air-cavity sphere (IOR=1.0) inside a glass shell (IOR=1.5), where the inner sphere's interface has glass on one side and air on the other.  Without (etaI, etaT), Newton would solve the half-vector constraint h ∝ -(1·wi + 1·wo) at the air-cavity vertex (which is the REFLECTION constraint) instead of the actual Snell refraction h ∝ -(1.5·wi + 1.0·wo).  Convergence at the wrong constraint root explains the SMS rejection cliff on Veach Egg / luminous orb scenes.
			///<
			///< Populated by BuildSeedChain (RGB and NM variants) at the time of each hit, using the same `currentIOR` and IOR-stack the seed-trace already maintains.  Single-IOR scenes (the existing test corpus) get etaI=1.0 (entering) or etaT=1.0 (exiting), matching the old hardcoded defaults — so unchanged behaviour for those scenes.  ValidateChainPhysics may also fall back to `eta` when (etaI, etaT) are at default-1.0 for back-compat with hand-constructed chains.
			RISEPel				attenuation;	///< Color attenuation at this vertex (e.g., colored glass refractance, mirror reflectance)
			bool				isReflection;	///< True if the chain ray bounces off (mirror, Fresnel reflection on glass, or TIR); false if it refracts through.
			bool				canRefract;		///< True if the underlying material can refract (dielectric).  False for pure mirrors / conductors.  Selects the throughput law: dielectrics use Fresnel(cosI, η_i, η_t) (covers reflection, refraction, and TIR); mirrors take full reflectance from the painter without an angle-dependent Fresnel factor.  Default true so hand-constructed test chains and pre-existing back-compat callers behave as dielectrics — the prior implicit assumption.
			bool				isExiting;		///< True if ray EXITS the object at this vertex (glass→air).  Set at seed-build time via IOR-stack object tracking.  Refraction-direction code uses this (NOT a local dot test) because a double-sided thin-sheet mesh can be crossed twice with the normal pointing in the same direction at both hits.
			const IObject*		pObject;		///< Object this vertex lies on
			const IMaterial*	pMaterial;		///< Material at this vertex
			bool				valid;			///< True if vertex data is complete

			ManifoldVertex() :
			position( Point3(0,0,0) ),
			normal( Vector3(0,0,0) ),
			geomNormal( Vector3(0,0,0) ),
			dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
			dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
			uv( Point2(0,0) ),
			eta( 1.0 ),
			etaI( 1.0 ),
			etaT( 1.0 ),
			attenuation( 1.0, 1.0, 1.0 ),
			isReflection( false ),
			canRefract( true ),
			isExiting( false ),
			pObject( 0 ),
			pMaterial( 0 ),
			valid( false )
			{
			}
		};

		/// Configuration for the manifold solver.
		struct ManifoldSolverConfig
		{
			bool			enabled;				///< Master switch: when false, no ManifoldSolver is created
			unsigned int	maxIterations;			///< Newton iteration limit
			Scalar			solverThreshold;		///< Convergence threshold on ||C||
			Scalar			uniquenessThreshold;	///< Threshold to distinguish solutions
			unsigned int	maxBernoulliTrials;		///< Max trials for unbiased PDF estimation
			bool			biased;					///< Skip Bernoulli PDF estimation (biased but fast)
			unsigned int	maxChainDepth;			///< Maximum number of specular vertices in chain
			Scalar			maxGeometricTerm;		///< Clamp for manifold geometric term

			/// Number of independent Newton solves per `EvaluateAtShadingPoint`
			/// call.  Trial 0 uses the deterministic Snell-traced seed chain
			/// (for backward compatibility and as a reliable first seed);
			/// trials 1..N-1 use photon-aided seeds (when photonCount>0) or
			/// skip otherwise.  Converged chains are deduped by first-vertex
			/// world-space position and the unique contributions are summed.
			/// On smooth specular surfaces every trial converges to the same
			/// root so N>1 is wasted work; on bumpy / displaced surfaces each
			/// trial can land in a DIFFERENT basin of attraction, uncovering
			/// caustic paths the single Snell-traced seed misses entirely.
			/// Default 1 preserves the pre-multi-trial single-solve behavior.
			unsigned int	multiTrials;

			/// Number of photons to emit at scene-prep time to build the
			/// photon-aided seed map.  0 disables the photon pass; trials
			/// 1..N-1 then have nothing to draw from and the solver behaves
			/// as if multiTrials==1 for those pixels (trial 0's deterministic
			/// Snell seed is the only contribution).
			///
			/// When > 0 the rasterizer owns an SMSPhotonMap built in
			/// PreRenderSetup; photons are deposited at their first
			/// diffuse-after-specular landing and each stored record
			/// remembers the FIRST specular-caster entry point as a known-
			/// good Newton seed.  Multi-trial queries the map by shading-
			/// point position to pull only seeds whose photons landed in
			/// the same neighborhood (fixed-radius kd-tree query).
			unsigned int	photonCount;

			/// World-space search radius for photon-seed lookup.  0 lets
			/// the photon map auto-compute a sensible default from the
			/// bounding box of photon landing positions (≈ 0.01 × bbox
			/// diagonal — mirrors VCM's auto merge radius heuristic).
			Scalar			photonSearchRadius;

			/// Per-shading-point cap on the number of photon seeds fed to
			/// Newton.  After QuerySeeds returns the kd-tree neighbours,
			/// the consumer randomly subsamples down to this count before
			/// running Newton on each — so dense regions of the photon
			/// map don't blow up per-pixel Newton-solve cost.  0 disables
			/// the cap (consume every queried photon — useful for
			/// convergence studies and unbiased-mode references).
			///
			/// Default 16 matches Weisstein 2024 PMS's typical
			/// `M_photon ∈ [8, 32]` and keeps the per-pixel cost
			/// proportional to the existing `multiTrials` budget rather
			/// than to the kd-tree photon density.  Without the cap, a
			/// shading point near a focused caustic can pull hundreds of
			/// photons out of the kd-tree and pay 100×+ Newton-solve
			/// overhead for no measurable variance reduction (the extra
			/// photons mostly converge to roots already deduped against
			/// earlier seeds).
			unsigned int	maxPhotonSeedsPerShadingPoint;

			/// Two-stage Newton solver (Zeltner 2020 §5).  When enabled,
			/// `Solve` first runs Newton on a smoothed reference surface
			/// (smoothing = 1: the underlying analytical base, no displacement
			/// or normal-map detail), then refines on the actual surface
			/// (smoothing = 0).  Gets Newton out of the C1-discontinuity-
			/// induced plateau on Phong-shaded triangle meshes.  No-op for
			/// scenes whose specular geometry doesn't expose a smoothing-
			/// aware analytical query (`IGeometry::ComputeAnalyticalDerivatives`).
			/// Default false — opt-in via the rasterizer's `sms_two_stage`
			/// parameter.  See `docs/SMS_TWO_STAGE_SOLVER.md`.
			bool			twoStage;

			/// Levenberg-Marquardt damping in `NewtonSolve`.  Damps the
			/// Jacobian's diagonal by `λ × mean(|J_ii|)` between Newton
			/// iterations: λ shrinks on accepted line-search steps (toward
			/// pure Newton, quadratic convergence near a root), grows on
			/// rejected ones (toward gradient descent, escapes plateaus
			/// where Newton's J⁻¹·C direction is unreliable).  Variant:
			/// damped Newton on the original `J`, not full Marquardt-style
			/// `(JᵀJ + λ·diag(JᵀJ))Δ = JᵀC` normal equations — keeps the
			/// existing block-tridiagonal solver path intact.
			///
			/// Default false (opt-in).  Recovers ~5pp Newton-fail rate
			/// on the displaced Veach egg sweep at the cost of ~50-100%
			/// more solver work per shading point on heavy-displacement
			/// scenes — turn on only when you've confirmed the scene
			/// benefits.  Negligible cost on smooth geometry.  See
			/// `docs/SMS_LEVENBERG_MARQUARDT.md`.
			bool			useLevenbergMarquardt;

			/// Seeding strategy for `EvaluateAtShadingPoint`.
			///
			///   `Snell` (default): trace a ray from the shading point toward
			///   the sampled light, refracting at every specular surface
			///   (RISE legacy).  Chain length emerges naturally from the
			///   trace; the seed pdf is unknown analytically.
			///
			///   `Uniform`: iterate the cached `mSpecularCasters` list and
			///   draw a uniform-area sample on each caster's surface;
			///   continue the chain via SnellContinueChain.  Matches
			///   Mitsuba's manifold_ss / manifold_ms.  Required for
			///   principled geometric Bernoulli `1/p` estimation
			///   (Zeltner 2020 §4.3 Algorithm 2 / §4.2).
			///
			/// Default `Snell` preserves backward compatibility.  Opt in
			/// via the rasterizer's `sms_seeding "uniform"` parameter.
			/// See `docs/SMS_UNIFORM_SEEDING_PLAN.md`.
			enum SeedingMode {
				eSeedingSnell   = 0,
				eSeedingUniform = 1
			};
			SeedingMode		seedingMode;

			/// Normalized-throughput gate for Fresnel branching at sub-
			/// critical dielectric vertices during seed-chain construction.
			/// Reuses the path-tracer's `StabilityConfig::branchingThreshold`
			/// semantics (CLAUDE.md High-Value Fact): at each dielectric
			/// hit, if the running chain throughput exceeds the threshold,
			/// SPLIT the chain into both Fresnel-reflection and refraction
			/// continuations (each becomes its own seed chain that runs
			/// Newton independently and contributes its own throughput).
			/// Below the threshold, Russian-roulette pick one branch
			/// weighted by Fr; the trial contribution is divided by the
			/// pick probability to remain unbiased.
			///
			///   `0.0` — always branch at every dielectric Fresnel decision
			///           point (exhaustive 2^k chain enumeration).
			///   `1.0` — never branch (legacy refraction-only seed; reflection
			///           caustics on dielectric only found via TIR, photon-aided
			///           seeds, or mirror materials).  **Default in this
			///           struct's literal init below**, so direct constructor
			///           callers preserve legacy behaviour; the rasterizer
			///           layer overrides this with `stabilityConfig.branching-
			///           Threshold` (default 0.5) at scene-prep so production
			///           scenes get branching automatically.
			Scalar			branchingThreshold;

			/// Mitsuba `m_config.bounces` analogue: REQUIRED chain length.
			/// When > 0, the seed-builder traces EXACTLY this many specular
			/// hits and rejects seeds that don't reach the target.  Active
			/// in BOTH snell and uniform modes — they share the same
			/// length-cap rule.  Default 0 = "no target": snell mode
			/// discovers chain length via the emitter-projection cap,
			/// uniform mode caps at `maxChainDepth`.  Recommended for
			/// `sms_seeding Uniform`, where without it the seed-chain
			/// length is variable and produces wildly different topology
			/// than the natural caustic (k=1, 3, 4 instead of k=2 for
			/// a glass shell).  See `SMSConfig::targetBounces`.
			unsigned int	targetBounces;

			ManifoldSolverConfig() :
			enabled( false ),
			maxIterations( 15 ),
			solverThreshold( 1e-4 ),
			uniquenessThreshold( 1e-2 ),
			maxBernoulliTrials( 100 ),
			biased( true ),
			maxChainDepth( 30 ),
			maxGeometricTerm( 10.0 ),
			multiTrials( 1 ),
			photonCount( 0 ),
			photonSearchRadius( 0 ),
			maxPhotonSeedsPerShadingPoint( 16 ),
			twoStage( false ),
			useLevenbergMarquardt( false ),
			seedingMode( eSeedingSnell ),
			branchingThreshold( 1.0 ),
			targetBounces( 0 )
			{
			}
		};

		/// Result of a manifold solve attempt.
		struct ManifoldResult
		{
			std::vector<ManifoldVertex>	specularChain;	///< Converged specular vertices
			RISEPel						contribution;	///< Path contribution (RGB)
			Scalar						contributionNM;	///< Path contribution (spectral)
			Scalar						pdf;			///< SMS PDF (1/p_k or 1.0 for biased)
			Scalar						jacobianDet;	///< |det(∂C/∂x_⊥)| of constraint Jacobian
			bool						valid;			///< True if Newton converged

			ManifoldResult() :
			contribution( RISEPel(0,0,0) ),
			contributionNM( 0 ),
			pdf( 1.0 ),
			jacobianDet( 1.0 ),
			valid( false )
			{
			}
		};

		/// Specular Manifold Sampling solver.
		///
		/// Given two non-specular endpoints (a shading point and an emitter),
		/// finds a valid path through a chain of specular surfaces using Newton
		/// iteration on the specular constraint manifold.
		class LightSampler;
		struct LightSample;
		class SMSPhotonMap;
		struct SMSPhoton;

		class ManifoldSolver :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			ManifoldSolverConfig config;
			LightSampler* pLightSampler;

			/// Optional photon-aided seeding pass.  Set once by the
			/// rasterizer in PreRenderSetup (pointer borrowed, not owned
			/// — the rasterizer keeps the storage alive for the full
			/// render).  Null means no photon pass was run; multi-trial
			/// falls back to the deterministic Snell seed only.
			const SMSPhotonMap* pPhotonMap;

			/// Cached list of objects whose material reports `isSpecular`.
			/// Populated once at scene-prep time by the rasterizer (calls
			/// EnumerateSpecularCasters on the scene, then SetSpecular-
			/// Casters here).  Read-only during rendering — concurrent
			/// render workers traverse it without locking.  Consumed by
			/// uniform-on-shape seeding (Mitsuba-style; opt-in via
			/// `ManifoldSolverConfig::seedingMode`).  Empty if the
			/// rasterizer hasn't populated it.
			std::vector<const IObject*> mSpecularCasters;

			virtual ~ManifoldSolver();

		public:
			ManifoldSolver( const ManifoldSolverConfig& cfg );

			/// Attach a photon-aided seed map.  Must be called AFTER the
			/// map's Build() has completed (the map is read-only from
			/// then on, so concurrent render workers are safe).  Pass
			/// nullptr to detach.
			void SetPhotonMap( const SMSPhotonMap* pm ) { pPhotonMap = pm; }
			const SMSPhotonMap* GetPhotonMap() const { return pPhotonMap; }

			/// Attach the cached specular-caster list.  Call once at
			/// scene-prep (after EnumerateSpecularCasters).  Pass an
			/// empty vector to clear.
			void SetSpecularCasters( std::vector<const IObject*> list ) {
				mSpecularCasters = std::move( list );
			}
			const std::vector<const IObject*>& GetSpecularCasters() const {
				return mSpecularCasters;
			}

			/// Scan the scene's object manager and collect every object
			/// whose material reports `isSpecular`.  Each object is
			/// queried via UniformRandomPoint(prand=(0.5,0.5,0.5))
			/// to obtain a valid `RayIntersectionGeometric` for the
			/// material's GetSpecularInfo call.  Result is appended to
			/// `out` (not cleared) — callers manage the vector's lifetime.
			static void EnumerateSpecularCasters(
				const IScene& scene,
				std::vector<const IObject*>& out
				);

			/// Main entry point: solve for a specular path connecting
			/// shadingPoint to emitterPoint through the given chain of
			/// specular objects.
			///
			/// \param shadingPoint   Non-specular endpoint (e.g. diffuse surface hit)
			/// \param shadingNormal  Normal at shading point
			/// \param emitterPoint   Light sample point
			/// \param emitterNormal  Normal at light sample
			/// \param specularChain  Initial seed vertices on specular surfaces
			/// \param sampler            Sampler with dimensional management (for Bernoulli trials)
			/// \return ManifoldResult with converged chain and contribution info
			ManifoldResult Solve(
				const Point3& shadingPoint,
				const Vector3& shadingNormal,
				const Point3& emitterPoint,
				const Vector3& emitterNormal,
				std::vector<ManifoldVertex>& specularChain,
				ISampler& sampler
				) const;

			/// Traces a seed ray from start toward end, collecting intersections
			/// with specular objects to build the initial chain.
			///
			/// \param start        Start point
			/// \param end          End point
			/// \param scene        Scene for ray casting
			/// \param caster       Ray caster
			/// \param chain        [out] Populated with seed ManifoldVertices
			/// \return Number of specular vertices found
			unsigned int BuildSeedChain(
				const Point3& start,
				const Point3& end,
				const IScene& scene,
				const IRayCaster& caster,
				std::vector<ManifoldVertex>& chain,
				bool applyEmitterStop = true       ///< Snell mode: true (stop at emitter projection).  Uniform mode: false (sp is a direction probe, not the emitter).
				) const;

			/// One result of `BuildSeedChainBranching`: a complete seed
			/// chain plus the accumulated proposal pdf (product of per-
			/// vertex Russian-roulette pick probabilities).  The caller
			/// runs Newton on `chain` and divides the converged trial's
			/// contribution by `proposalPdf` to keep the estimator
			/// unbiased (the BSDF Fresnel factor already in
			/// `EvaluateChainThroughput` cancels the proposal pdf for
			/// RR-picked vertices, so the division is essential).
			struct SeedChainResult {
				std::vector<ManifoldVertex> chain;
				Scalar proposalPdf = 1.0;
			};

			/// Branching seed-chain builder (Option C — reuses the path-
			/// tracer's `branchingThreshold` semantics for SMS seed
			/// construction).  At each sub-critical dielectric vertex:
			///   - If running chain throughput > `config.branchingThreshold`,
			///     SPLIT the chain into both Fresnel-reflection and
			///     refraction continuations.  Each becomes a separate
			///     output chain (full Newton-eligible seed) with the
			///     same `proposalPdf` (no RR factor).
			///   - Else, Russian-roulette pick one branch weighted by Fr;
			///     multiply `proposalPdf` by the pick probability.
			/// TIR is forced reflection (deterministic, no branch / RR).
			/// Mirror materials are deterministic (single reflection
			/// continuation).
			///
			/// Caller MUST divide each trial's contribution by
			/// `proposalPdf` after running Newton — the contribution
			/// formula in `EvaluateChainThroughput` includes the per-
			/// vertex Fresnel factor, which cancels the proposal pdf for
			/// RR'd vertices.
			///
			/// When `config.branchingThreshold == 1.0` (legacy default),
			/// no branching ever fires; behaves like a stochastic-
			/// Fresnel BuildSeedChain (Russian-roulette pick at every
			/// dielectric vertex weighted by Fr).
			///
			/// When `config.branchingThreshold == 0.0`, every dielectric
			/// vertex branches; total chain count is bounded by 2^k where
			/// k is the chain length.
			unsigned int BuildSeedChainBranching(
				const Point3& start,
				const Point3& end,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<SeedChainResult>& out,
				bool applyEmitterStop = true       ///< Snell mode: true.  Uniform mode: false (see BuildSeedChain).
				) const;

			/// Continues a manifold-seed chain by Snell-tracing a ray from
			/// `currentOrigin` along `dir`, picking up specular hits as
			/// ManifoldVertex entries in `chain`.  Stops at the first
			/// non-specular hit, when no further intersection is found,
			/// when total traced distance exceeds `maxDist * 3`, or when
			/// the chain reaches `config.maxChainDepth`.
			///
			/// State arguments are mutated in-place — `currentIOR` and
			/// `seedIor` reflect the post-trace medium / IOR-stack.
			///
			/// Used by both `BuildSeedChain` (the legacy "Snell-trace from
			/// shading point toward light" entry) and the Mitsuba-style
			/// uniform-on-shape SMS seeding path (where the first vertex
			/// is sampled uniformly on a caster and subsequent vertices
			/// are discovered by this routine).
			///
			/// \param currentOrigin  [in/out] Starting world-space point;
			///                                set to the last hit on return.
			/// \param dir            [in/out] Initial unit direction;
			///                                updated to the post-Snell-trace
			///                                direction after the last hit.
			/// \param maxDist        Total expected distance (for the safety
			///                       cutoff `range > maxDist*3`).
			/// \param currentIOR     [in/out] Incident-medium IOR;
			///                                updated as the trace pushes/pops
			///                                IOR boundaries.
			/// \param seedIor        [in/out] Per-object IOR stack;
			///                                updated as above.
			/// \param scene          Scene for ray casting.
			/// \param caster         Ray caster (currently unused — reserved
			///                       for visibility queries through occluders).
			/// \param chain          [in/out] Vertices appended (not cleared).
			/// \return Number of vertices appended in this call.
			unsigned int SnellContinueChain(
				Point3& currentOrigin,
				Vector3& dir,
				Scalar maxDist,
				Scalar& currentIOR,
				IORStack& seedIor,
				const IScene& scene,
				const IRayCaster& caster,
				std::vector<ManifoldVertex>& chain,
				bool applyEmitterStop = true       ///< false ⇒ ignore the projection cap; trace continues through every specular hit until maxChainDepth or non-specular hit.
				) const;

			/// Computes the geometric coupling factor through a specular
			/// chain for the path integral (incoming cosines / dist² at
			/// each specular vertex, plus 1/dist² for the last segment).
			/// Caller multiplies by cosAtShading and cosAtLight separately.
			/// NOTE: the 1/dist² terms overlap with the Jacobian determinant's
			/// internal direction derivatives.  Prefer EvaluateChainCosineProduct
			/// for the SMS contribution formula to avoid double-counting.
			Scalar EvaluateChainGeometry(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain
				) const;

			/// Computes the product of incoming cosines at each specular
			/// vertex in the chain, WITHOUT distance terms.  The distance
			/// factors (1/dist²) are already encoded in the Jacobian
			/// determinant via its direction derivatives (dwi/du ∝ 1/dist_i).
			/// Used in the SMS contribution formula: the weight is
			///   cos(θ_x) × cos(θ_y) × ∏cos(θ_in_j) / |det(∂C/∂x)|
			/// where only endpoint and vertex cosines are explicit.
			Scalar EvaluateChainCosineProduct(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain
				) const;

			/// Computes Fresnel-weighted transmittance/reflectance product
			/// along a converged specular chain, including Beer's law attenuation.
			///
			/// \return Per-channel attenuation factor for the chain
			RISEPel EvaluateChainThroughput(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain
				) const;

			/// Scalar (spectral) variant of EvaluateChainThroughput.
			Scalar EvaluateChainThroughputNM(
				const Point3& startPoint,
				const Point3& endPoint,
				const std::vector<ManifoldVertex>& chain,
				const Scalar nm
				) const;

			const ManifoldSolverConfig& GetConfig() const { return config; }

			/// Result of a single SMS evaluation at a shading point.
			struct SMSContribution
			{
				RISEPel		contribution;	///< Total SMS contribution (BSDF * G * throughput * Le / pdf)
				Scalar		misWeight;		///< MIS weight for this contribution
				bool		valid;			///< True if a valid specular path was found

				SMSContribution() : contribution( RISEPel(0,0,0) ), misWeight( 1.0 ), valid( false ) {}
			};

			/// Standalone SMS evaluation at a single shading point.
			///
			/// Samples a light, builds a seed chain toward it, solves the
			/// manifold, evaluates the BSDF at the shading point, and
			/// assembles the full caustic contribution.
			///
			/// This is the reusable core that both BDPTIntegrator and
			/// SMSShaderOp call.
			///
			/// The receiver-frame normal is supplied as a SHADING/GEOMETRIC
			/// pair so the BSDF eval and BSDF-cosine factor stay in the
			/// shading frame (Veach §5.3.6) while probe-direction fallback
			/// and chain-topology decisions use the actual face orientation
			/// (PBRT 4e §10.1.1).
			///
			/// \param pos              Shading point position (non-specular surface)
			/// \param geomNormal       Geometric (flat-face) normal at shading point
			/// \param shadingNormal    Shading normal at shading point (BSDF frame)
			/// \param onb              Orthonormal basis at shading point (built from shading)
			/// \param pMaterial        Material at shading point (must have a BSDF)
			/// \param woOutgoing       Direction toward the viewer/previous vertex
			/// \param scene            Scene for ray casting and light access
			/// \param caster           Ray caster
			/// \param sampler          Sampler with proper dimensional seperation
			SMSContribution EvaluateAtShadingPoint(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler
				) const;

			/// Uniform-on-shape SMS evaluator (Mitsuba-faithful single- /
			/// multi-scatter; matches `manifold_ss::specular_manifold_sampling`
			/// for k=1 and `manifold_ms::specular_manifold_sampling` via
			/// SnellContinueChain for k>=2).
			///
			/// Iterates the cached `mSpecularCasters`.  For each caster:
			///   1. Uniform-area sample on the caster surface.
			///   2. Snell-traced seed chain shading-point→sampled-point
			///      (this naturally produces k>=2 chains by following any
			///      subsequent specular hits, and trips the
			///      caster-mismatch-rejection when the visibility ray
			///      lands somewhere unexpected).
			///   3. Newton solve toward the same `lightSample` as the
			///      Snell-mode path.
			///   4. Per-caster contribution accumulated unweighted
			///      (Mitsuba sums one independent estimate per caster
			///      shape; no MIS over caster choice — see manifold_ss.cpp
			///      lines 32-42).
			///
			/// Selected at runtime by `config.seedingMode == eSeedingUniform`.
			/// Geometric Bernoulli `1/p` (Phase 5) and photon-aided trial
			/// integration (Phase 7) layer on top of this scaffold.
			SMSContribution EvaluateAtShadingPointUniform(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler
				) const;

			/// Spectral variant of SMS evaluation.
			struct SMSContributionNM
			{
				Scalar		contribution;
				Scalar		misWeight;
				bool		valid;

				SMSContributionNM() : contribution( 0 ), misWeight( 1.0 ), valid( false ) {}
			};

			/// Spectral counterpart of `EvaluateAtShadingPointUniform`.
			/// Iterates `mSpecularCasters` per-shading-point, runs M-trial
			/// biased or geometric-Bernoulli unbiased Newton solves with
			/// per-wavelength throughput, sums per-caster contributions
			/// (no MIS over caster choice — Mitsuba pattern).  Selected
			/// at runtime by `config.seedingMode == eSeedingUniform`.
			SMSContributionNM EvaluateAtShadingPointNMUniform(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const Scalar nm
				) const;

			SMSContributionNM EvaluateAtShadingPointNM(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const IMaterial* pMaterial,
				const Vector3& woOutgoing,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				const Scalar nm
				) const;

			/// Tests whether the external segments of an SMS specular
			/// chain are unoccluded.  Checks two segments:
			///   1. shading point -> first specular vertex
			///   2. last specular vertex -> light source
			///
			/// LIMITATION: Inter-specular segments (vertices inside
			/// the glass body) are NOT tested.  CastShadowRay uses
			/// the scene's acceleration structure which includes the
			/// glass geometry itself, so any ray between two glass
			/// vertices would report a false self-intersection.
			/// Filtering specular objects from shadow tests would
			/// require per-object exclusion lists, which is a more
			/// invasive change.  This means an opaque object placed
			/// entirely inside a glass body (between two specular
			/// vertices) would not be caught.  In practice this is
			/// rare; the common occlusion case (wall between the
			/// floor and the glass, or between the glass and the
			/// light) is handled by the external-segment checks.
			///
			/// \return True if both external segments are unoccluded.
			bool CheckChainVisibility(
				const Point3& shadingPoint,
				const Point3& lightPoint,
				const std::vector<ManifoldVertex>& chain,
				const IRayCaster& caster
				) const;

			/// Reverses a photon's recorded specular chain (stored in
			/// photon-direction order: v[0] nearest light, v[k-1] nearest
			/// diffuse) into an SMS seed chain (receiver-direction order:
			/// v[0] nearest receiver, v[k-1] nearest light).  Flips
			/// `isExiting` on refraction-only vertices; reflection-only
			/// vertices are direction-symmetric so the flag is preserved.
			///
			/// Re-queries each vertex's material for `attenuation` /
			/// `canRefract` (avoids relying on stale photon-deposit data).
			/// Sets `mv.valid = false` so `Solve` recomputes derivatives.
			///
			/// `mv.etaI` / `mv.etaT` stay at the default 1.0 — RISE's
			/// SMSPhoton storage doesn't carry IOR-stack snapshots, so
			/// downstream half-vector / Fresnel math falls back to
			/// air-on-other-side via `GetEffectiveEtas` (correct for
			/// single-dielectric-in-air photons; wrong for nested
			/// dielectrics — see `EvaluateAtShadingPointNM` for the
			/// matching limitation).
			///
			/// \return Number of vertices written into `chain`.  Returns 0
			///         when the photon's chain length is invalid.
			unsigned int ReversePhotonChainForSeed(
				const SMSPhoton& photon,
				std::vector<ManifoldVertex>& chain
				) const;

			/// Computes a single converged trial's contribution at the
			/// shading point.  Performs the visibility check, BSDF eval,
			/// shading-point cosine, light-side cosine + Le, and the SMS
			/// measure-conversion factor (G_x_v1 * |det dv/dy|).
			///
			/// Returns false (and `outContribution = 0`, `outDir = (0,0,0)`)
			/// for any of the bail-out conditions:
			///   - Distance to first specular vertex < 1e-8.
			///   - External-segment visibility blocked.
			///   - BSDF max channel <= 0 at the shading point.
			///   - cos at shading point <= 0.
			///   - Distance from last specular to light < 1e-8 / cos at
			///     light <= 0 for area lights.
			///
			/// Used by both `EvaluateAtShadingPoint` (snell mode) and
			/// `EvaluateAtShadingPointUniform` (Mitsuba-faithful uniform
			/// mode), and by both their photon-aided extension paths.
			/// The spectral counterpart `ComputeTrialContributionNM`
			/// performs the same logic on `Scalar` per-wavelength.
			bool ComputeTrialContribution(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const Vector3& woOutgoing,
				const IBSDF* pBSDF,
				const LightSample& lightSample,
				const ManifoldResult& mResult,
				const IRayCaster& caster,
				Vector3& outDir,
				RISEPel& outContribution
				) const;

			/// Spectral counterpart of `ComputeTrialContribution`.
			bool ComputeTrialContributionNM(
				const Point3& pos,
				const Vector3& geomNormal,
				const Vector3& shadingNormal,
				const OrthonormalBasis3D& onb,
				const Vector3& woOutgoing,
				const IBSDF* pBSDF,
				const LightSample& lightSample,
				const ManifoldResult& mResult,
				const IRayCaster& caster,
				const Scalar nm,
				Vector3& outDir,
				Scalar& outContribution
				) const;

			// ============================================================
			// Static utility methods (2x2 block arithmetic, Fresnel, etc.)
			// ============================================================

			/// Derivative of a normalized vector: d/dx [v/|v|].
			/// Given h = v/|v|, dv = derivative of unnormalized v, and
			/// vLen = |v|, returns the derivative of h.
			static Vector3 DeriveNormalized(
				const Vector3& h, const Vector3& dv, Scalar vLen );

			/// Inverts a 2x2 block stored as [a,b,c,d] row-major.
			/// Returns false if the block is singular.
			static bool Invert2x2( const Scalar* m, Scalar* inv );

			/// Multiplies 2x2 blocks: C = A * B  (all row-major).
			static void Mul2x2( const Scalar* A, const Scalar* B, Scalar* C );

			/// Multiplies 2x2 block by 2-vector: r = A * v.
			static void Mul2x2Vec( const Scalar* A, const Scalar* v, Scalar* r );

			/// Subtracts 2x2 blocks: C = A - B.
			static void Sub2x2( const Scalar* A, const Scalar* B, Scalar* C );

			/// Exact dielectric Fresnel reflectance (unpolarized average).
			/// \param cosI     Cosine of incidence angle (positive)
			/// \param eta_i    IOR of the incoming medium
			/// \param eta_t    IOR of the transmitted medium
			/// \return Reflectance F in [0, 1].  Returns 1.0 for TIR.
			static Scalar ComputeDielectricFresnel(
				Scalar cosI, Scalar eta_i, Scalar eta_t );

			/// Computes world-space gradients of spherical coordinates
			/// (theta, phi) with respect to the direction vector.
			/// \param dir       Direction vector (world space)
			/// \param s         Normalized tangent u (= normalize(dpdu))
			/// \param t         Normalized tangent v (= normalize(dpdv))
			/// \param normal    Surface normal
			/// \param dTheta_dDir  [out] World-space gradient of theta
			/// \param dPhi_dDir    [out] World-space gradient of phi
			static void ComputeSphericalDerivatives(
				const Vector3& dir,
				const Vector3& s,
				const Vector3& t,
				const Vector3& normal,
				Vector3& dTheta_dDir,
				Vector3& dPhi_dDir );

			/// Computes 3x3 Jacobian d(wo)/d(wi) for the specular
			/// direction (reflection or refraction).  Assumes unnormalized
			/// wo; apply DeriveNormalized correction if chaining with
			/// the normalized output of ComputeSpecularDirection.
			static void ComputeSpecularDirectionDerivativeWrtWi(
				const Vector3& wi,
				const Vector3& normal,
				Scalar eta,
				bool isReflection,
				Scalar dwo_dwi[9] );

		protected:
			/// Builds the block-tridiagonal Jacobian for the
			/// angle-difference constraint via analytical derivatives.
			/// Uses ComputeSphericalDerivatives, ComputeSpecularDirection
			/// DerivativeWrtWi, and ComputeSpecularDirectionDerivative
			/// WrtNormal for the chain rule through the constraint.
			void BuildJacobianAngleDiff(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower
				) const;

			/// Numerical Jacobian for the angle-difference constraint.
			/// Finite-differences EvaluateConstraintAtVertex.
			void BuildJacobianAngleDiffNumerical(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower
				) const;

		protected:
			/// Validates that converged specular chain vertices have
			/// physically consistent ray geometry.  Refraction vertices
			/// must have wi and wo on opposite sides of the surface;
			/// reflection vertices must have both on the same side.
			/// \return True if all vertices pass the check.
			bool ValidateChainPhysics(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd
				) const;

			/// Computes the determinant of a block-tridiagonal matrix
			/// via LU forward elimination.
			/// \return det(J) = product of det(Dp[i]) diagonal blocks.
			Scalar ComputeBlockTridiagonalDeterminant(
				const std::vector<Scalar>& diag,
				const std::vector<Scalar>& upper,
				const std::vector<Scalar>& lower,
				unsigned int k
				) const;

			/// Runs Newton iteration to solve C(x) = 0.
			/// Modifies chain in-place. Returns true if converged.
			///
			/// `smoothing` ∈ [0, 1] is forwarded to `UpdateVertexOnSurface`
			/// — > 0 walks on a smoothed reference surface (Stage 1 of the
			/// SMS two-stage solver), 0 (default) walks on the actual surface.
			bool NewtonSolve(
				std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				Scalar smoothing = 0.0
				) const;

			/// Evaluates the 2k-dimensional constraint vector.
			/// C[2*i] and C[2*i+1] are the angle-difference constraint
			/// at specular vertex i.
			void EvaluateConstraint(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& C
				) const;

			/// Builds the block-tridiagonal Jacobian dC/dx.
			/// Stored as arrays of 2x2 blocks: diag[k], upper[k-1], lower[k-1].
			/// Each block is stored as 4 scalars in row-major order.
			///
			/// `includeCurvature`: if true (default), include the ds_du
			/// tangent-frame-rotation terms — required for the measure-
			/// conversion |det| to reflect surface curvature, hence for
			/// correct caustic focusing in the contribution formula.  If
			/// false, skip them — useful for Newton iteration stability
			/// on meshes where the derivatives jump discontinuously
			/// across triangle boundaries.  The resulting Jacobian is
			/// APPROXIMATE w.r.t. the true dC/du (treats the surface as
			/// locally flat) but converges more reliably under line
			/// search.
			void BuildJacobian(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower,
				bool includeCurvature = true
				) const;

			/// Solves block-tridiagonal system J * delta = rhs.
			/// Returns false if system is singular.
			bool SolveBlockTridiagonal(
				std::vector<Scalar>& diag,
				const std::vector<Scalar>& upper,
				const std::vector<Scalar>& lower,
				const std::vector<Scalar>& rhs,
				unsigned int k,
				std::vector<Scalar>& delta
				) const;

			/// Computes the generalized geometric term (Jacobian determinant
			/// ratio) for MIS weighting.
			Scalar ComputeManifoldGeometricTerm(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd
				) const;

			/// Computes the 2x2 block ∂C_at_vk / ∂y_tangent  — i.e., how the
			/// constraint at the LAST specular vertex v_k changes when the
			/// light endpoint y moves in its own tangent plane (y_s, y_t).
			/// Output `Jy` is row-major: [∂Cs/∂ys, ∂Cs/∂yt; ∂Ct/∂ys, ∂Ct/∂yt].
			void ComputeLastBlockLightJacobian(
				const ManifoldVertex& vk,
				const Point3& prevPos,
				const Point3& lightPos,
				const Vector3& lightNormal,
				Scalar Jy[4]
				) const;

			/// Solves the block-tridiagonal implicit-function-theorem problem
			/// to compute |det(δv_1_⊥ / δy_⊥)| — how the FIRST specular
			/// vertex's tangent-plane position responds to infinitesimal
			/// motion of the light endpoint y in its tangent plane.
			/// Returns 0 on singular Jacobian.  Sign is discarded (abs value).
			Scalar ComputeLightToFirstVertexJacobianDet(
				const std::vector<ManifoldVertex>& chain,
				const Point3& shadingPoint,
				const Point3& lightPos,
				const Vector3& lightNormal
				) const;

			/// Updates a vertex position on its surface by stepping along
			/// the surface parameterization by (du, dv), then recomputes
			/// all derivative data via ray intersection.
			///
			/// `smoothing` ∈ [0, 1] — when > 0 and the vertex's object
			/// supports `ComputeAnalyticalDerivatives`, takes the step in
			/// (u, v) space directly and re-evaluates the smoothing-aware
			/// analytical surface, bypassing the mesh ray-cast snap.
			/// `smoothing = 0` (default) preserves the legacy mesh-snap
			/// behaviour.  Used by SMS two-stage solver — see
			/// `docs/SMS_TWO_STAGE_SOLVER.md`.
			bool UpdateVertexOnSurface(
				ManifoldVertex& vertex,
				Scalar du,
				Scalar dv,
				Scalar smoothing = 0.0
				) const;

			/// Fills in surface derivative data (dpdu, dpdv, dndu, dndv)
			/// for a vertex by querying its object's geometry with proper
			/// world/object space transforms.
			///
			/// `smoothing` ∈ [0, 1] — when > 0 and the vertex's object
			/// supports `ComputeAnalyticalDerivatives`, queries the
			/// smoothing-aware analytical path (overwriting position,
			/// normal, and derivatives at `vertex.uv`).  `smoothing = 0`
			/// (default) preserves the legacy mesh-FD-probe behaviour.
			bool ComputeVertexDerivatives(
				ManifoldVertex& vertex,
				Scalar smoothing = 0.0
				) const;

			/// Make dpdu, dpdv an orthonormal basis of the tangent plane
			/// at vertex.normal (Gram-Schmidt + unit-length normalization).
			/// Scales dndu, dndv consistently so dn/du remains the rate of
			/// change per unit displacement in the (new, unit) dpdu direction.
			/// Required because geometry-supplied dpdu/dpdv (e.g. triangle
			/// edges) need not be orthogonal or unit — if we fed those into
			/// BuildJacobian unchanged, |det| would vary with the arbitrary
			/// choice of parameterization instead of tracking the physical
			/// caustic geometry.
			void OrthonormalizeTangentFrame(
				ManifoldVertex& vertex
				) const;

			/// Computes the specularly scattered direction at a vertex.
			/// For refraction: Snell's law. For reflection: mirror law.
			/// Returns false on total internal reflection (refraction only).
			bool ComputeSpecularDirection(
				const Vector3& wi,
				const Vector3& normal,
				Scalar eta,
				bool isReflection,
				Vector3& wo
				) const;

			/// Computes the derivative of the specular direction w.r.t.
			/// the surface normal. Returns a 3x3 matrix stored as 9 scalars
			/// (row-major).
			void ComputeSpecularDirectionDerivativeWrtNormal(
				const Vector3& wi,
				const Vector3& normal,
				Scalar eta,
				bool isReflection,
				Scalar dwo_dn[9]
				) const;

			/// Converts a direction to local spherical coordinates (theta, phi)
			/// in the tangent frame defined by dpdu, dpdv, normal.
			Point2 DirectionToSpherical(
				const Vector3& dir,
				const Vector3& dpdu,
				const Vector3& dpdv,
				const Vector3& normal
				) const;

			/// Evaluates the angle-difference constraint at a single specular
			/// vertex (Zeltner et al. 2020).  The constraint measures the
			/// angular deviation between the actual outgoing direction and the
			/// specularly scattered direction in the local tangent frame:
			///   C0 = theta_actual - theta_specular
			///   C1 = wrapToPi(phi_actual - phi_specular)
			void EvaluateConstraintAtVertex(
				const Point3& vertexPos,
				const Vector3& vertexNormal,
				const Vector3& vertexDpdu,
				const Vector3& vertexDpdv,
				Scalar vertexEta,
				bool vertexIsReflection,
				const Point3& prevPos,
				const Point3& nextPos,
				Scalar& C0,
				Scalar& C1
				) const;

			/// Builds the block-tridiagonal Jacobian via central finite
			/// differences on the constraint function.  Matches whichever
			/// constraint formulation EvaluateConstraint uses.
			void BuildJacobianNumerical(
				const std::vector<ManifoldVertex>& chain,
				const Point3& fixedStart,
				const Point3& fixedEnd,
				std::vector<Scalar>& diag,
				std::vector<Scalar>& upper,
				std::vector<Scalar>& lower
				) const;

			/// Bernoulli trial estimator for unbiased PDF of solution k.
			Scalar EstimatePDF(
				const ManifoldResult& solution,
				const Point3& shadingPoint,
				const Point3& emitterPoint,
				const std::vector<ManifoldVertex>& seedTemplate,
				ISampler& sampler
				) const;
		};
	}
}

#endif
