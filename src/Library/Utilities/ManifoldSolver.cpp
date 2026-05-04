//////////////////////////////////////////////////////////////////////
//
//  ManifoldSolver.cpp - Specular Manifold Sampling solver
//
//    Implements the Newton iteration method from Zeltner et al. 2020
//    for finding valid specular paths connecting two non-specular
//    endpoints through a chain of specular surfaces.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ManifoldSolver.h"
#include "SMSPhotonMap.h"
#include "Optics.h"
#include "BDPTUtilities.h"

// File-scope diagnostic gate.  Set to 1 to enable targeted per-pixel
// SMS/Solve/BuildSeedChain trace logging (used while debugging the
// torus-intersection accuracy issue that led to the OQS replacement).
// Leave at 0 for production — the instrumentation stays in-source as
// a regression aid but is compiled out.
#define SMS_TRACE_DIAGNOSTIC 0

// Lightweight Solve()-failure-mode counters.  When enabled, dumps an
// [SMS-SOLVE-DIAG] + [SMS-NEWTONFAIL-RES] pair at process exit so the
// share of solves rejected by each early-out path is visible alongside
// PathTracingIntegrator's per-evaluation SMS-DIAG line.  Used to attribute
// the energy drop on heavy-displacement scenes to Newton plateau-stalling
// rather than seed quality or iteration budget — see the displaced-Veach-
// egg sweep results.  Leave at 0 in production; flip to 1 when re-
// auditing SMS energy ratios across disp / multi-trial / photon configs.
#define SMS_SOLVE_DIAG 0

// Newton step-norm cap.  Bounds each Newton iter's max world-space
// vertex displacement to <kNewtonStepNormCapFrac> × (mean inter-vertex
// segment length).  Default 0 disables the cap (the existing 10-halving
// backtracking line search already handles oversize Newton steps —
// measured: at disp=0 the cap fires 76 % of the time on raw Newton
// steps that average 262× oversize, but the line search converges to
// the same end point; Newton-fail rate is unchanged within MC noise).
//
// Code retained as inactive instrumentation.  Flip to a positive value
// (e.g. 0.25) only when investigating perf — fewer line-search attempts
// per iter — not for convergence on heavy displacement (the failure
// mode there is wrong step DIRECTION, not wrong step SIZE; LM/trust-
// region damping is the relevant fix).  See
// docs/SMS_PHOTON_DECOUPLING_AND_DISPLACEMENT_LIMIT.md for the full
// measurement.
namespace { constexpr RISE::Scalar kNewtonStepNormCapFrac = RISE::Scalar( 0.0 ); }

// Levenberg-Marquardt damping for the Newton solver.  When enabled,
// `NewtonSolve` damps the Jacobian's diagonal by `λ × mean(|J_ii|)`
// before each solve, and adapts `λ` between iterations: shrink on
// accepted steps (back toward pure Newton, fast convergence near a
// root), grow on rejected line-search attempts (more gradient-descent-
// like, escape from plateaus where the Newton direction is unreliable).
//
// Variant: damped Newton on the original J, not the full Marquardt-style
// `(JᵀJ + λ·diag(JᵀJ))Δ = JᵀC` normal equations.  We modify diag(J)
// in place and reuse the existing block-tridiagonal solver, which keeps
// the bandwidth structure intact.  Full LM via normal equations would
// double the bandwidth (block-tridiag × block-tridiag = block-pentadiag)
// and need a different solver — escalate to that variant only if this
// damped-Newton form is insufficient.
//
// Runtime-toggleable via `ManifoldSolverConfig::useLevenbergMarquardt`
// (default FALSE — opt-in).  LM is ~50-100% slower than pure Newton on
// heavy-displacement scenes (escalation iterations consume the iter
// budget) for a 4-10 percentage-point Newton-fail-rate improvement
// across the displaced-egg sweep.  Off by default because the cost is
// substantial relative to the gain; enable explicitly when a scene's
// caustics on a heavily-displaced mesh need the extra robustness.
// See docs/SMS_LEVENBERG_MARQUARDT.md for the full A/B measurement.
// Edge-aware Newton step.  The Jacobian's `dndu`/`dndv` predict how the
// normal rotates for a step (du, dv).  Within one triangle, Phong-
// interpolated normals are linear in barycentric coords so the
// prediction is accurate.  ACROSS a triangle edge, the per-triangle
// linearization changes — so a Newton step that crosses an edge
// produces an actual normal rotation that differs from what the saved
// Jacobian predicted.  Reject the step when actual rotation exceeds a
// relative threshold and let the line search halve.
//
// Measured to NOT help on the displaced Veach egg: rejection rate
// climbs cleanly with displacement (0% at disp=0 → 12% at disp=10),
// confirming the check correctly identifies "untrusted linearization,"
// but Newton-fail rate is unchanged or marginally worse (-0.0 to
// -0.4 pp ok rate).  The C¹ defect manifests as "predicted descent
// direction is noisy" rather than "Newton overshoots into bad
// territory" — the line search's ||C|| decrease test was already
// accepting the cross-edge steps that decreased residual; pre-
// rejecting on linearization-trust just blocks useful moves.
//
// Code retained as inactive instrumentation (gate at 0 = compiled
// out).  See docs/SMS_LEVENBERG_MARQUARDT.md for the negative-result
// measurement.
#define SMS_EDGE_AWARE_NEWTON 0

// Smooth-base Jacobian throughout the solve.  When enabled, even at
// `smoothing == 0` (the default solve path) ComputeVertexDerivatives
// queries the underlying analytical surface for `dpdu/dpdv/dndu/dndv`,
// while keeping the bumpy mesh position and normal from the ray-cast.
// The intent: Newton's descent direction comes from a C∞ smooth
// curvature model rather than the per-triangle Phong-interpolated J,
// while the residual `C` is still evaluated against the real bumpy
// normals.
//
// Compile-time toggle, default 0 (legacy mesh-J path).  Flip to 1 to
// reproduce the smooth-J experiment on the displaced Veach egg.
#define SMS_SMOOTH_BASE_JACOBIAN 0
#if SMS_EDGE_AWARE_NEWTON
namespace { constexpr RISE::Scalar kEdgeTrustAbsFloor = RISE::Scalar( 0.05 ); }	///< below this absolute rotation magnitude, no test (any change OK)
namespace { constexpr RISE::Scalar kEdgeTrustRelRatio = RISE::Scalar( 3.0  ); }	///< actual must not exceed predicted × this when both > floor
#endif
// LM damping schedule (used when `config.useLevenbergMarquardt` is true).
namespace { constexpr RISE::Scalar kLM_LambdaInit = RISE::Scalar( 1e-3 ); }
namespace { constexpr RISE::Scalar kLM_LambdaMin  = RISE::Scalar( 1e-9 ); }
namespace { constexpr RISE::Scalar kLM_LambdaMax  = RISE::Scalar( 1e9  ); }
namespace { constexpr RISE::Scalar kLM_LambdaUp   = RISE::Scalar( 10.0 ); }
namespace { constexpr RISE::Scalar kLM_LambdaDown = RISE::Scalar( 0.1  ); }
#if SMS_SOLVE_DIAG
#include <atomic>
#include <cstdio>
namespace {
	std::atomic<uint64_t> g_solveDiag_calls{0};
	std::atomic<uint64_t> g_solveDiag_seedTooFar{0};
	std::atomic<uint64_t> g_solveDiag_derivFail{0};
	std::atomic<uint64_t> g_solveDiag_newtonFail{0};
	std::atomic<uint64_t> g_solveDiag_physicsFail{0};
	std::atomic<uint64_t> g_solveDiag_shortSeg{0};
	std::atomic<uint64_t> g_solveDiag_ok{0};
	// Newton-fail final ||C|| residual buckets — distinguishes "stuck very
	// close" (basin-too-narrow / line-search starved) from "diverged far"
	// (seed in wrong basin or line-search exits early).
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e3{0};	///< ||C|| ∈ [threshold, 1e-3)
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e2{0};	///< [1e-3, 1e-2)
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e1{0};	///< [1e-2, 1e-1)
	std::atomic<uint64_t> g_solveDiag_newtonFail_lt1e0{0};	///< [1e-1, 1)
	std::atomic<uint64_t> g_solveDiag_newtonFail_ge1e0{0};	///< [1, ∞)
	// Step-norm cap diagnostics: how often the cap fires, and the
	// average pre-cap step ratio (max_step / cap) — indicates whether
	// the cap actually intercepts Newton steps or sits idle.
	std::atomic<uint64_t> g_solveDiag_capChecks{0};
	std::atomic<uint64_t> g_solveDiag_capFired{0};
	std::atomic<uint64_t> g_solveDiag_capRatioSum{0};	///< Σ (max_step/cap × 1000), only when fired

	// Levenberg-Marquardt diagnostics.
	std::atomic<uint64_t> g_solveDiag_lmTotalIters{0};      ///< Newton iterations across all Solve calls
	std::atomic<uint64_t> g_solveDiag_lmDamped{0};          ///< Iters where λ > 0 was applied
	std::atomic<uint64_t> g_solveDiag_lmEscalated{0};       ///< Iters where line search failed → λ increased
	std::atomic<uint64_t> g_solveDiag_lmRecovered{0};       ///< Solve calls where LM rescued an iter that pure Newton would have failed

	// Edge-aware Newton-step diagnostics.
	std::atomic<uint64_t> g_solveDiag_edgeChecks{0};        ///< Line-search attempts where the trust check ran
	std::atomic<uint64_t> g_solveDiag_edgeRejects{0};       ///< Line-search attempts rejected due to untrusted linearization

	struct SolveDiagAtExitInstaller {
		SolveDiagAtExitInstaller() {
			std::atexit([](){
				const uint64_t calls   = g_solveDiag_calls.load();
				const uint64_t farSeed = g_solveDiag_seedTooFar.load();
				const uint64_t deriv   = g_solveDiag_derivFail.load();
				const uint64_t newton  = g_solveDiag_newtonFail.load();
				const uint64_t physics = g_solveDiag_physicsFail.load();
				const uint64_t shrt    = g_solveDiag_shortSeg.load();
				const uint64_t ok      = g_solveDiag_ok.load();
				std::fprintf( stderr,
					"[SMS-SOLVE-DIAG] calls=%llu  ok=%llu  seedTooFar=%llu  derivFail=%llu  newtonFail=%llu  physicsFail=%llu  shortSeg=%llu",
					(unsigned long long)calls, (unsigned long long)ok,
					(unsigned long long)farSeed, (unsigned long long)deriv,
					(unsigned long long)newton, (unsigned long long)physics,
					(unsigned long long)shrt );
				if( calls > 0 ) {
					std::fprintf( stderr,
						"  pcts: ok=%.1f%% farSeed=%.1f%% newton=%.1f%% physics=%.1f%% short=%.1f%%",
						100.0 * double(ok)      / double(calls),
						100.0 * double(farSeed) / double(calls),
						100.0 * double(newton)  / double(calls),
						100.0 * double(physics) / double(calls),
						100.0 * double(shrt)    / double(calls) );
				}
				std::fprintf( stderr, "\n" );
				const uint64_t b1 = g_solveDiag_newtonFail_lt1e3.load();
				const uint64_t b2 = g_solveDiag_newtonFail_lt1e2.load();
				const uint64_t b3 = g_solveDiag_newtonFail_lt1e1.load();
				const uint64_t b4 = g_solveDiag_newtonFail_lt1e0.load();
				const uint64_t b5 = g_solveDiag_newtonFail_ge1e0.load();
				const uint64_t btot = b1 + b2 + b3 + b4 + b5;
				if( btot > 0 ) {
					std::fprintf( stderr,
						"[SMS-NEWTONFAIL-RES] ||C|| histogram: <1e-3=%llu(%.1f%%) 1e-3..1e-2=%llu(%.1f%%) 1e-2..1e-1=%llu(%.1f%%) 1e-1..1=%llu(%.1f%%) >=1=%llu(%.1f%%)\n",
						(unsigned long long)b1, 100.0*double(b1)/double(btot),
						(unsigned long long)b2, 100.0*double(b2)/double(btot),
						(unsigned long long)b3, 100.0*double(b3)/double(btot),
						(unsigned long long)b4, 100.0*double(b4)/double(btot),
						(unsigned long long)b5, 100.0*double(b5)/double(btot) );
				}
				const uint64_t capChecks = g_solveDiag_capChecks.load();
				const uint64_t capFired  = g_solveDiag_capFired.load();
				if( capChecks > 0 ) {
					const double avgRatio = capFired > 0
						? double( g_solveDiag_capRatioSum.load() ) / 1000.0 / double( capFired )
						: 0.0;
					std::fprintf( stderr,
						"[SMS-STEPCAP] checks=%llu fired=%llu (%.2f%%)  avg(max_step/cap when fired)=%.2f\n",
						(unsigned long long)capChecks,
						(unsigned long long)capFired,
						100.0 * double( capFired ) / double( capChecks ),
						avgRatio );
				}
				const uint64_t lmIters     = g_solveDiag_lmTotalIters.load();
				const uint64_t lmDamped    = g_solveDiag_lmDamped.load();
				const uint64_t lmEscalated = g_solveDiag_lmEscalated.load();
				const uint64_t lmRecovered = g_solveDiag_lmRecovered.load();
				if( lmIters > 0 ) {
					std::fprintf( stderr,
						"[SMS-LM] iters=%llu damped=%llu(%.2f%%) escalations=%llu rescues=%llu\n",
						(unsigned long long)lmIters,
						(unsigned long long)lmDamped,
						100.0 * double( lmDamped ) / double( lmIters ),
						(unsigned long long)lmEscalated,
						(unsigned long long)lmRecovered );
				}
				const uint64_t edgeChecks  = g_solveDiag_edgeChecks.load();
				const uint64_t edgeRejects = g_solveDiag_edgeRejects.load();
				if( edgeChecks > 0 ) {
					std::fprintf( stderr,
						"[SMS-EDGE] checks=%llu rejects=%llu (%.2f%%)\n",
						(unsigned long long)edgeChecks,
						(unsigned long long)edgeRejects,
						100.0 * double( edgeRejects ) / double( edgeChecks ) );
				}
			});
		}
	};
	SolveDiagAtExitInstaller g_solveDiag_installer;
}
#endif
#include "../Interfaces/ILog.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IObjectManager.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IEnumCallback.h"
#include "IndependentSampler.h"
#include "RandomNumbers.h"
#include "../Intersection/RayIntersection.h"
#include "../Lights/LightSampler.h"
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// Construction / Destruction
//////////////////////////////////////////////////////////////////////

ManifoldSolver::ManifoldSolver( const ManifoldSolverConfig& cfg ) :
config( cfg ),
pLightSampler( 0 ),
pPhotonMap( 0 )
{
	pLightSampler = new LightSampler();
}

ManifoldSolver::~ManifoldSolver()
{
	safe_release( pLightSampler );
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirection
//
//   Convention for the manifold solver:
//     wi = unit direction from vertex TOWARD the previous vertex
//          (pointing away from surface on the incoming side)
//     wo = unit direction from vertex TOWARD the next vertex
//          (pointing away from surface on the outgoing side)
//     normal = outward surface normal
//////////////////////////////////////////////////////////////////////

namespace
{
	// Resolve the (η_i, η_t) pair to use for a vertex's half-vector /
	// Snell / Fresnel math.  Two paths:
	//
	//   1. Modern: BuildSeedChain (RGB and NM) populates v.etaI and
	//      v.etaT explicitly using the IOR stack.  This works for both
	//      single dielectric in air AND nested dielectric scenes.
	//
	//   2. Back-compat: hand-constructed chains in the existing test
	//      corpus (ManifoldSolverTest.cpp) often only set v.eta and
	//      leave (etaI, etaT) at their default (1.0, 1.0).  Those
	//      tests assume "the other side of every interface is air"
	//      — which is exactly what the OLD `eta_eff = isExiting ?
	//      1/eta : eta` formula computed.  When we detect this case
	//      (both etaI and etaT at default 1.0 BUT v.eta != 1.0), we
	//      fall back to the air-as-other-side assumption so no
	//      pre-existing test breaks.
	//
	// A consequence: a test that explicitly wants etaI=1.0 AND
	// etaT=1.0 AND eta != 1.0 (a degenerate / pathological case)
	// would get the back-compat path instead, but no such test
	// exists in the current corpus.  If one is added, populate
	// (etaI, etaT) explicitly.
	inline void GetEffectiveEtas(
		const RISE::Implementation::ManifoldVertex& v,
		Scalar& eta_i,
		Scalar& eta_t )
	{
		const bool backCompat =
			( v.etaI == Scalar( 1.0 ) ) &&
			( v.etaT == Scalar( 1.0 ) ) &&
			( v.eta  != Scalar( 1.0 ) );
		if( backCompat ) {
			if( v.isExiting ) {
				eta_i = v.eta;
				eta_t = Scalar( 1.0 );
			} else {
				eta_i = Scalar( 1.0 );
				eta_t = v.eta;
			}
		} else {
			eta_i = v.etaI;
			eta_t = v.etaT;
		}
	}
}

bool ManifoldSolver::ComputeSpecularDirection(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Vector3& wo
	) const
{
	if( isReflection )
	{
		// Reflection: wo = wi - 2*(wi.n)*n
		// Using Optics::CalculateReflectedRay expects vIn pointing toward
		// the surface.  Our wi points away from surface, so negate it.
		// reflected = vIn - 2*(vIn.n)*n
		// We want: wo = (-wi) - 2*((-wi).n)*n = -wi + 2*(wi.n)*n
		const Scalar d = Vector3Ops::Dot( wi, normal );
		wo = Vector3(
			-wi.x + 2.0 * d * normal.x,
			-wi.y + 2.0 * d * normal.y,
			-wi.z + 2.0 * d * normal.z
			);
		wo = Vector3Ops::Normalize( wo );
		return true;
	}
	else
	{
		// Refraction via Snell's law
		// wi points away from surface toward incoming side
		// We compute the refracted direction on the other side
		//
		// The standard formula uses eta_ratio = n_incoming / n_transmitted.
		// When entering glass (cos_i > 0, wi on normal side):
		//   n_incoming = 1 (air), n_transmitted = eta (glass)
		//   eta_ratio = 1/eta
		// When exiting glass (cos_i < 0, wi opposite to normal):
		//   n_incoming = eta (glass), n_transmitted = 1 (air)
		//   eta_ratio = eta
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );

		Vector3 n = normal;
		Scalar eta_ratio = 1.0 / eta;   // Default: entering glass
		Scalar ci = cos_i;

		if( ci < 0.0 )
		{
			// wi is on the opposite side of the normal — exiting glass
			n = Vector3( -normal.x, -normal.y, -normal.z );
			ci = -ci;
			eta_ratio = eta;  // glass → air: n_glass / n_air
		}

		const Scalar sin2_t = eta_ratio * eta_ratio * (1.0 - ci * ci);

		if( sin2_t > 1.0 )
		{
			// Total internal reflection
			return false;
		}

		const Scalar cos_t = sqrt( 1.0 - sin2_t );

		// Refracted direction: wt = -eta_ratio * wi + (eta_ratio * ci - cos_t) * n
		// This gives a direction pointing away from the surface on the transmitted side
		wo = Vector3(
			-eta_ratio * wi.x + (eta_ratio * ci - cos_t) * n.x,
			-eta_ratio * wi.y + (eta_ratio * ci - cos_t) * n.y,
			-eta_ratio * wi.z + (eta_ratio * ci - cos_t) * n.z
			);
		wo = Vector3Ops::Normalize( wo );
		return true;
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirectionDerivativeWrtNormal
//
//   Computes the 3x3 Jacobian d(wo)/d(n), stored row-major.
//
//   Currently unused: the half-vector Jacobian (BuildJacobian)
//   captures surface curvature via the Weingarten map (ds/du from
//   dndu/dndv) without needing this derivative.  It would be needed
//   for an analytical Jacobian of the angle-difference constraint,
//   which explicitly computes d(wo_specular)/d(n) in its chain rule.
//
//   NOTE: this derivative assumes unnormalized wo (the raw output
//   of ComputeSpecularDirection before normalization).  If used with
//   the normalized version, apply the DeriveNormalized correction.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSpecularDirectionDerivativeWrtNormal(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Scalar dwo_dn[9]
	) const
{
	if( isReflection )
	{
		// wo = -wi + 2*(wi.n)*n
		// d(wo)/d(n) = 2*(wi.n)*I + 2*outer(n, wi)
		// where outer(a,b)_ij = a_i * b_j
		const Scalar d = Vector3Ops::Dot( wi, normal );

		// Row 0
		dwo_dn[0] = 2.0 * d + 2.0 * normal.x * wi.x;		// d(wo.x)/d(n.x)
		dwo_dn[1] = 2.0 * normal.x * wi.y;				// d(wo.x)/d(n.y)
		dwo_dn[2] = 2.0 * normal.x * wi.z;				// d(wo.x)/d(n.z)
		// Row 1
		dwo_dn[3] = 2.0 * normal.y * wi.x;				// d(wo.y)/d(n.x)
		dwo_dn[4] = 2.0 * d + 2.0 * normal.y * wi.y;		// d(wo.y)/d(n.y)
		dwo_dn[5] = 2.0 * normal.y * wi.z;				// d(wo.y)/d(n.z)
		// Row 2
		dwo_dn[6] = 2.0 * normal.z * wi.x;				// d(wo.z)/d(n.x)
		dwo_dn[7] = 2.0 * normal.z * wi.y;				// d(wo.z)/d(n.y)
		dwo_dn[8] = 2.0 * d + 2.0 * normal.z * wi.z;		// d(wo.z)/d(n.z)
	}
	else
	{
		// Refraction: wo = -eta*wi + mu*n  where mu = eta*cos_i - cos_t
		// cos_i = dot(wi, n), cos_t = sqrt(1 - eta^2*(1-cos_i^2))
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );
		const Scalar sin2_t = eta * eta * (1.0 - cos_i * cos_i);

		Scalar cos_t = 0.0;
		if( sin2_t < 1.0 ) {
			cos_t = sqrt( 1.0 - sin2_t );
		}
		if( cos_t < NEARZERO ) {
			cos_t = NEARZERO;
		}

		const Scalar mu = eta * cos_i - cos_t;

		// d(mu)/d(n_j) = eta * wi_j * (1 - eta * cos_i / cos_t)
		// d(wo_i)/d(n_j) = d(mu)/d(n_j) * n_i + mu * delta_ij
		// = eta * wi_j * (1 - eta*cos_i/cos_t) * n_i + mu * delta_ij

		const Scalar factor = eta * (1.0 - eta * cos_i / cos_t);

		// Row 0
		dwo_dn[0] = factor * wi.x * normal.x + mu;
		dwo_dn[1] = factor * wi.y * normal.x;
		dwo_dn[2] = factor * wi.z * normal.x;
		// Row 1
		dwo_dn[3] = factor * wi.x * normal.y;
		dwo_dn[4] = factor * wi.y * normal.y + mu;
		dwo_dn[5] = factor * wi.z * normal.y;
		// Row 2
		dwo_dn[6] = factor * wi.x * normal.z;
		dwo_dn[7] = factor * wi.y * normal.z;
		dwo_dn[8] = factor * wi.z * normal.z + mu;
	}
}

//////////////////////////////////////////////////////////////////////
// DirectionToSpherical
//
//   Converts a direction to spherical coords (theta, phi) in the
//   local tangent frame defined by dpdu, dpdv, normal.
//////////////////////////////////////////////////////////////////////

Point2 ManifoldSolver::DirectionToSpherical(
	const Vector3& dir,
	const Vector3& dpdu,
	const Vector3& dpdv,
	const Vector3& normal
	) const
{
	const Vector3 u = Vector3Ops::Normalize( dpdu );
	const Vector3 v = Vector3Ops::Normalize( dpdv );

	const Scalar x = Vector3Ops::Dot( dir, u );
	const Scalar y = Vector3Ops::Dot( dir, v );
	const Scalar z = Vector3Ops::Dot( dir, normal );

	// Clamp z to [-1, 1] for numerical safety
	Scalar cz = z;
	if( cz > 1.0 ) cz = 1.0;
	if( cz < -1.0 ) cz = -1.0;

	const Scalar theta = acos( cz );
	const Scalar phi = atan2( y, x );

	return Point2( theta, phi );
}

//////////////////////////////////////////////////////////////////////
// EvaluateConstraint
//
//   Half-vector constraint (Zeltner et al. 2020).
//
//   For each specular vertex i, construct the generalized half-vector:
//     Refraction: h = -(wi + eta_eff * wo),  normalized
//     Reflection: h =   wi + wo,              normalized
//   where eta_eff accounts for entering vs exiting the medium.
//
//   When the specular constraint is exactly satisfied, h is parallel
//   to the surface normal.  The constraint measures the tangent-plane
//   projection of h, which must be zero:
//     C[2i]   = dot(dpdu, h)
//     C[2i+1] = dot(dpdv, h)
//
//   This avoids the angular wrapping issues of the spherical-coordinate
//   formulation and yields a well-conditioned Jacobian.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::EvaluateConstraint(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& C
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	C.resize( 2 * k, 0.0 );

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Previous and next positions
		Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// wi = direction from vertex toward previous vertex
		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// wo = direction from vertex toward next vertex
		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		// Construct the generalized half-vector
		Vector3 h;

		if( v.isReflection )
		{
			// Reflection: h = wi + wo
			h = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
		}
		else
		{
			// Refraction: Walter et al. 2007 generalized half-vector
			//   h ∝ -(η_i wi + η_t wo)
			// where η_i is the IOR on the wi side (incoming medium) and
			// η_t is the IOR on the wo side (outgoing medium).  Pulled
			// from BuildSeedChain's per-vertex (etaI, etaT) population.
			//
			// The OLD formula `h = -(wi + eta_eff*wo)` with `eta_eff =
			// isExiting ? 1/eta : eta` is mathematically equivalent for
			// the air-on-the-other-side case (it differs by a constant
			// factor of η_i, which the normalization downstream washes
			// out).  But it silently hardcodes "the other side is air"
			// — wrong for nested dielectrics like an air-cavity inside
			// glass, where the inner-cavity vertex has η=1.5 on one
			// side and η=1.0 on the other.  Walter's form handles
			// nested dielectrics correctly.
			//
			// GetEffectiveEtas falls back to the old air-as-other-side
			// behaviour when (etaI, etaT) are at default-1.0 — this
			// preserves bit-exact behaviour for the existing test
			// corpus that hand-constructs vertices with only `eta`
			// set.  See GetEffectiveEtas docstring for full rationale.
			Scalar eta_i, eta_t;
			GetEffectiveEtas( v, eta_i, eta_t );

			h = Vector3(
				-(eta_i * wi.x + eta_t * wo.x),
				-(eta_i * wi.y + eta_t * wo.y),
				-(eta_i * wi.z + eta_t * wo.z)
			);
		}

		// Normalize h
		Scalar hLen = Vector3Ops::Magnitude( h );
		if( hLen < NEARZERO )
		{
			// Degenerate — set large constraint
			C[2*i]   = 1.0;
			C[2*i+1] = 1.0;
			continue;
		}
		h = h * (1.0 / hLen);

		// Tangent-plane projection: when Snell's law is satisfied,
		// h is parallel to the normal, so these projections are zero.
		//
		// Use the Gram-Schmidt-projected tangent basis
		//     s = normalize(dpdu - (dpdu·n) n)
		//     t = n × s
		// (same convention as Cycles MNEE and BuildJacobian below).
		// This matters for curved surfaces: the BuildJacobian's ds/du
		// and dt/du analytical terms assume s is defined this way.  If
		// EvaluateConstraint used plain Normalize(dpdu) instead, the
		// Jacobian wouldn't match dC/dx and Newton would fail to
		// converge on non-flat surfaces.
		const Scalar dpdu_dot_n = Vector3Ops::Dot( v.dpdu, v.normal );
		Vector3 s_unnorm(
			v.dpdu.x - dpdu_dot_n * v.normal.x,
			v.dpdu.y - dpdu_dot_n * v.normal.y,
			v.dpdu.z - dpdu_dot_n * v.normal.z );
		const Scalar s_len = Vector3Ops::Magnitude( s_unnorm );
		Vector3 s = (s_len > NEARZERO) ? (s_unnorm * (1.0 / s_len)) :
			Vector3Ops::Normalize( v.dpdu );
		Vector3 t = Vector3Ops::Cross( v.normal, s );

		C[2*i]   = Vector3Ops::Dot( s, h );
		C[2*i+1] = Vector3Ops::Dot( t, h );
	}
}

//////////////////////////////////////////////////////////////////////
// EvaluateConstraintAtVertex
//
//   Angle-difference constraint (Zeltner et al. 2020).
//
//   For a specular vertex, the constraint measures the angular
//   deviation between the actual outgoing direction wo and the
//   specularly scattered direction wo_spec:
//
//     C0 = theta(wo) - theta(wo_spec)
//     C1 = wrapToPi( phi(wo) - phi(wo_spec) )
//
//   where theta and phi are spherical coordinates in the local
//   tangent frame (s, t, normal).
//
//   This avoids the back-facing degeneracies of the half-vector
//   projection and provides larger convergence basins for Newton
//   iteration with distant initial guesses.
//////////////////////////////////////////////////////////////////////

// NOTE: EvaluateConstraintAtVertex (this function), BuildJacobianAngleDiff,
// and BuildJacobianAngleDiffNumerical use the angle-difference constraint
// form via ComputeSpecularDirection.  ComputeSpecularDirection silently
// assumes "the other side of the interface is air" — same nested-dielectric
// bug as the OLD eta_eff formula in EvaluateConstraint.  Production code
// (ManifoldSolver::Solve) uses the half-vector form via EvaluateConstraint
// and BuildJacobian, both of which have been fixed via GetEffectiveEtas to
// use the per-vertex (etaI, etaT) populated by BuildSeedChain.
//
// The angle-diff functions are TEST-ONLY (called from ManifoldSolverTest.cpp
// to validate the analytical-vs-numerical Jacobian agreement on flat single-
// IOR test geometry).  Their air-on-other-side assumption holds for the
// existing test corpus.  If a future test wants to exercise nested-
// dielectric topology via the angle-diff path, ComputeSpecularDirection
// would need an (eta_i, eta_t) overload — left as a future cleanup.
void ManifoldSolver::EvaluateConstraintAtVertex(
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
	) const
{
	// Direction from vertex toward previous vertex (incoming)
	Vector3 wi = Vector3Ops::mkVector3( prevPos, vertexPos );
	const Scalar wiLen = Vector3Ops::NormalizeMag( wi );

	// Direction from vertex toward next vertex (actual outgoing)
	Vector3 wo_actual = Vector3Ops::mkVector3( nextPos, vertexPos );
	const Scalar woLen = Vector3Ops::NormalizeMag( wo_actual );

	if( wiLen < NEARZERO || woLen < NEARZERO )
	{
		C0 = 1.0;
		C1 = 1.0;
		return;
	}

	// Compute the specularly scattered direction
	Vector3 wo_specular;
	if( !ComputeSpecularDirection( wi, vertexNormal, vertexEta,
		vertexIsReflection, wo_specular ) )
	{
		// Total internal reflection for a refraction vertex — degenerate
		C0 = 1.0;
		C1 = 1.0;
		return;
	}

	// Convert both directions to spherical coordinates in the
	// local tangent frame
	const Point2 angles_actual = DirectionToSpherical(
		wo_actual, vertexDpdu, vertexDpdv, vertexNormal );
	const Point2 angles_specular = DirectionToSpherical(
		wo_specular, vertexDpdu, vertexDpdv, vertexNormal );

	// Theta difference (no periodicity issue)
	C0 = angles_actual.x - angles_specular.x;

	// Phi difference — must wrap to [-pi, pi] to handle periodicity
	// (Zeltner errata: without this, the solver does not realize that
	// a value near 2*pi is close to a zero and fails to converge)
	Scalar phi_diff = angles_actual.y - angles_specular.y;
	if( phi_diff > PI ) phi_diff -= 2.0 * PI;
	if( phi_diff < -PI ) phi_diff += 2.0 * PI;
	C1 = phi_diff;
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianNumerical
//
//   Builds the block-tridiagonal Jacobian dC/dx via central finite
//   differences on the constraint function.  This matches whichever
//   constraint formulation EvaluateConstraint uses (angle-difference
//   or half-vector).
//
//   For each vertex i and each surface parameter p (u or v):
//     - Diagonal block: perturb vertex i position and normal,
//       evaluate C_i at (pos ± dp*eps, normal ± dn*eps)
//     - Upper block (i+1): perturb nextPos, evaluate C_i
//     - Lower block (i-1): perturb prevPos, evaluate C_i
//
//   Uses central differences: dC/dp ≈ (C(+eps) - C(-eps)) / (2*eps)
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianNumerical(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	const Scalar eps = 1e-5;
	const Scalar inv2eps = 1.0 / (2.0 * eps);

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	// Numerically differentiates the SAME half-vector constraint
	// that BuildJacobian uses analytically.  For each vertex j and
	// each surface parameter p (u or v), perturb vertex j's position
	// and normal, evaluate the full chain constraint, and extract
	// the finite-difference derivative for all constraint components
	// that depend on vertex j.

	for( unsigned int j = 0; j < k; j++ )
	{
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? chain[j].dpdu : chain[j].dpdv;
			const Vector3& dn = (p == 0) ? chain[j].dndu : chain[j].dndv;

			// Build perturbed chains
			std::vector<ManifoldVertex> chainPlus( chain );
			std::vector<ManifoldVertex> chainMinus( chain );

			chainPlus[j].position = Point3Ops::mkPoint3( chain[j].position, dp * eps );
			chainPlus[j].normal = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x + dn.x * eps,
				chain[j].normal.y + dn.y * eps,
				chain[j].normal.z + dn.z * eps ) );

			chainMinus[j].position = Point3Ops::mkPoint3( chain[j].position, dp * (-eps) );
			chainMinus[j].normal = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x - dn.x * eps,
				chain[j].normal.y - dn.y * eps,
				chain[j].normal.z - dn.z * eps ) );

			std::vector<Scalar> Cp, Cm;
			EvaluateConstraint( chainPlus, fixedStart, fixedEnd, Cp );
			EvaluateConstraint( chainMinus, fixedStart, fixedEnd, Cm );

			// Extract derivatives for constraint i w.r.t. vertex j
			// Diagonal: i == j
			diag[j*4 + 0 + p] = (Cp[2*j]   - Cm[2*j])   * inv2eps;
			diag[j*4 + 2 + p] = (Cp[2*j+1] - Cm[2*j+1]) * inv2eps;

			// Upper: vertex j affects constraint j-1 (if j > 0)
			// lower[(j-1)] maps vertex j to constraint j-1
			if( j > 0 )
			{
				unsigned int ci = j - 1;  // constraint index
				upper[ci*4 + 0 + p] = (Cp[2*ci]   - Cm[2*ci])   * inv2eps;
				upper[ci*4 + 2 + p] = (Cp[2*ci+1] - Cm[2*ci+1]) * inv2eps;
			}

			// Lower: vertex j affects constraint j+1 (if j < k-1)
			// lower[j] maps vertex j to constraint j+1
			if( j < k - 1 )
			{
				unsigned int ci = j + 1;  // constraint index
				lower[j*4 + 0 + p] = (Cp[2*ci]   - Cm[2*ci])   * inv2eps;
				lower[j*4 + 2 + p] = (Cp[2*ci+1] - Cm[2*ci+1]) * inv2eps;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobian
//
//   Analytical Jacobian for the half-vector constraint
//   (Zeltner et al. 2020, adapted from their reference code).
//
//   C_i = (dot(s_i, h_i),  dot(t_i, h_i))
//
//   The Jacobian is block-tridiagonal because C_i depends only on
//   vertices i-1, i, i+1.  Each block is 2x2.
//
//   For each vertex i, we compute dC_i / dX_j analytically by
//   differentiating h with respect to vertex positions and
//   accounting for the tangent-frame variation (ds, dt) due to
//   normal curvature (dndu, dndv).
//////////////////////////////////////////////////////////////////////

Vector3 ManifoldSolver::DeriveNormalized( const Vector3& h, const Vector3& dv, Scalar vLen )
{
	if( vLen < NEARZERO ) return Vector3( 0, 0, 0 );
	const Scalar invLen = 1.0 / vLen;
	// (dv - h * dot(h, dv)) / |v|
	const Scalar proj = Vector3Ops::Dot( h, dv );
	return Vector3(
		(dv.x - h.x * proj) * invLen,
		(dv.y - h.y * proj) * invLen,
		(dv.z - h.z * proj) * invLen );
}

void ManifoldSolver::BuildJacobian(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower,
	bool includeCurvature
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Previous and next positions
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// Directions and distances
		Vector3 d_wi = Vector3Ops::mkVector3( prevPos, v.position );
		const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
		const Vector3 wi = d_wi;

		Vector3 d_wo = Vector3Ops::mkVector3( nextPos, v.position );
		const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
		const Vector3 wo = d_wo;

		if( dist_i < NEARZERO || dist_o < NEARZERO ) continue;

		const Scalar inv_li = 1.0 / dist_i;
		const Scalar inv_lo = 1.0 / dist_o;

		// Tangent frame (normalized)
		Vector3 s = Vector3Ops::Normalize( v.dpdu );
		Vector3 t = Vector3Ops::Normalize( v.dpdv );

		// Walter et al. 2007 generalized half-vector (η_i, η_t form).
		// Resolved from the per-vertex (etaI, etaT) populated by
		// BuildSeedChain's IOR-stack tracking; falls back to the
		// "air-on-the-other-side" assumption for hand-constructed test
		// chains that only set v.eta.  See GetEffectiveEtas docstring.
		//
		// The OLD `eta_eff` form (`isExiting ? 1/eta : eta`) is
		// equivalent to Walter's form divided by η_i — same direction
		// after normalization, same Jacobian after the chain rule (the
		// constant scaling cancels in DeriveNormalized).  But it
		// silently hardcodes "the other side is air" — wrong for
		// nested dielectrics like an air-cavity inside glass.
		Scalar eta_i_v = Scalar( 1.0 );
		Scalar eta_t_v = Scalar( 1.0 );
		if( !v.isReflection ) {
			GetEffectiveEtas( v, eta_i_v, eta_t_v );
		}

		// Half-vector (unnormalized)
		Vector3 h_raw;
		if( v.isReflection )
		{
			h_raw = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
		}
		else
		{
			h_raw = Vector3(
				-(eta_i_v * wi.x + eta_t_v * wo.x),
				-(eta_i_v * wi.y + eta_t_v * wo.y),
				-(eta_i_v * wi.z + eta_t_v * wo.z) );
		}

		Scalar h_len = Vector3Ops::Magnitude( h_raw );
		if( h_len < NEARZERO ) continue;
		Vector3 h = h_raw * (1.0 / h_len);

		// ---- Derivative of h w.r.t. moving vertex i ----
		//
		// Moving vertex i by dp changes both wi and wo:
		//   dwi/dp = -(I - wi⊗wi) / dist_i * dp  (wi moves toward prev)
		//   dwo/dp = -(I - wo⊗wo) / dist_o * dp  (wo moves toward next)
		//
		// For h = sign * (wi + eta*wo) / |...|, with dp = dpdu:
		//   dh_raw/dp = sign * (dwi/dp + eta * dwo/dp)
		//   dh/dp = (dh_raw/dp - h * dot(h, dh_raw/dp)) / h_len
		//
		// We compute dh for dp = dpdu and dp = dpdv separately.

		// For each surface parameter (u, v):
		Vector3 dh_du, dh_dv;
		{
			// dp/du = dpdu, dp/dv = dpdv
			// dwi/du = -(dpdu - wi * dot(wi, dpdu)) / dist_i
			const Vector3 dwi_du = Vector3(
				-(v.dpdu.x - wi.x * Vector3Ops::Dot( wi, v.dpdu )) * inv_li,
				-(v.dpdu.y - wi.y * Vector3Ops::Dot( wi, v.dpdu )) * inv_li,
				-(v.dpdu.z - wi.z * Vector3Ops::Dot( wi, v.dpdu )) * inv_li );
			const Vector3 dwo_du = Vector3(
				-(v.dpdu.x - wo.x * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo,
				-(v.dpdu.y - wo.y * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo,
				-(v.dpdu.z - wo.z * Vector3Ops::Dot( wo, v.dpdu )) * inv_lo );

			Vector3 dh_raw_du;
			if( v.isReflection )
			{
				dh_raw_du = Vector3( dwi_du.x + dwo_du.x, dwi_du.y + dwo_du.y, dwi_du.z + dwo_du.z );
			}
			else
			{
				// Walter form: ∂h/∂p = -(η_i ∂wi/∂p + η_t ∂wo/∂p)
				dh_raw_du = Vector3(
					-(eta_i_v * dwi_du.x + eta_t_v * dwo_du.x),
					-(eta_i_v * dwi_du.y + eta_t_v * dwo_du.y),
					-(eta_i_v * dwi_du.z + eta_t_v * dwo_du.z) );
			}
			dh_du = DeriveNormalized( h, dh_raw_du, h_len );

			// Same for dv
			const Vector3 dwi_dv = Vector3(
				-(v.dpdv.x - wi.x * Vector3Ops::Dot( wi, v.dpdv )) * inv_li,
				-(v.dpdv.y - wi.y * Vector3Ops::Dot( wi, v.dpdv )) * inv_li,
				-(v.dpdv.z - wi.z * Vector3Ops::Dot( wi, v.dpdv )) * inv_li );
			const Vector3 dwo_dv = Vector3(
				-(v.dpdv.x - wo.x * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo,
				-(v.dpdv.y - wo.y * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo,
				-(v.dpdv.z - wo.z * Vector3Ops::Dot( wo, v.dpdv )) * inv_lo );

			Vector3 dh_raw_dv;
			if( v.isReflection )
			{
				dh_raw_dv = Vector3( dwi_dv.x + dwo_dv.x, dwi_dv.y + dwo_dv.y, dwi_dv.z + dwo_dv.z );	// reflection: η_i = η_t (irrelevant)
			}
			else
			{
				// Walter form: ∂h/∂p = -(η_i ∂wi/∂p + η_t ∂wo/∂p)
				dh_raw_dv = Vector3(
					-(eta_i_v * dwi_dv.x + eta_t_v * dwo_dv.x),
					-(eta_i_v * dwi_dv.y + eta_t_v * dwo_dv.y),
					-(eta_i_v * dwi_dv.z + eta_t_v * dwo_dv.z) );
			}
			dh_dv = DeriveNormalized( h, dh_raw_dv, h_len );
		}

		// Derivative of tangent frame w.r.t. surface parameters (u, v).
		//
		// Derivation (from Blender Cycles MNEE, which matches Zeltner 2020):
		// Let s = normalize(dpdu - (dpdu·n) n)  (Gram-Schmidt projection of
		// dpdu into the tangent plane).  Treating dpdu as fixed while only
		// n varies (the constraint function holds dpdu constant), the
		// product rule gives:
		//   ds/du = -1/|s| × [ (dpdu·dndu) n + (dpdu·n) dndu ]
		// Then re-orthogonalize against s so ds/du stays perpendicular to s:
		//   ds/du -= s × (s·ds/du)
		// The corresponding bitangent t = n × s, so:
		//   dt/du = dndu × s + n × ds/du
		//
		// CRITICAL: this formula produces a nonzero ds/du contribution at
		// the converged specular solution (h ≈ n) for curved surfaces, which
		// is essential for the Jacobian determinant to capture surface
		// curvature (the ingredient that creates caustic focusing).
		//
		// The previous formula (dndu - s*(s·dndu))/|dpdu| degenerated to
		// (dndu·t) t/|dpdu| because dndu·n = 0 identically.  Dotted with h=n
		// at convergence, that gave zero, silently eliminating the curvature
		// contribution from the Jacobian — the manifold then behaved as if
		// on a flat surface regardless of actual displacement.
		const Scalar dpdu_dot_n = Vector3Ops::Dot( v.dpdu, v.normal );
		const Vector3 s_unnorm = Vector3(
			v.dpdu.x - dpdu_dot_n * v.normal.x,
			v.dpdu.y - dpdu_dot_n * v.normal.y,
			v.dpdu.z - dpdu_dot_n * v.normal.z );
		const Scalar s_len = Vector3Ops::Magnitude( s_unnorm );
		const Scalar inv_s_len = 1.0 / fmax( s_len, NEARZERO );
		// (s may differ slightly from the previously-computed s because
		// that was Normalize(dpdu) without the tangent-plane projection;
		// here we use the Gram-Schmidt variant to stay consistent with
		// the product-rule derivation below.)
		const Vector3 s_proj = s_unnorm * inv_s_len;
		const Vector3 t_cross = Vector3Ops::Cross( v.normal, s_proj );

		Vector3 ds_du( 0, 0, 0 ), ds_dv( 0, 0, 0 );
		Vector3 dt_du( 0, 0, 0 ), dt_dv( 0, 0, 0 );
		if( includeCurvature ) {
			const Scalar dpdu_dot_dndu = Vector3Ops::Dot( v.dpdu, v.dndu );
			const Scalar dpdu_dot_dndv = Vector3Ops::Dot( v.dpdu, v.dndv );
			ds_du = Vector3(
				-inv_s_len * (dpdu_dot_dndu * v.normal.x + dpdu_dot_n * v.dndu.x),
				-inv_s_len * (dpdu_dot_dndu * v.normal.y + dpdu_dot_n * v.dndu.y),
				-inv_s_len * (dpdu_dot_dndu * v.normal.z + dpdu_dot_n * v.dndu.z) );
			ds_dv = Vector3(
				-inv_s_len * (dpdu_dot_dndv * v.normal.x + dpdu_dot_n * v.dndv.x),
				-inv_s_len * (dpdu_dot_dndv * v.normal.y + dpdu_dot_n * v.dndv.y),
				-inv_s_len * (dpdu_dot_dndv * v.normal.z + dpdu_dot_n * v.dndv.z) );
			// Re-orthogonalize against s so ds_du stays ⊥ s.
			const Scalar ds_du_dot_s = Vector3Ops::Dot( ds_du, s_proj );
			const Scalar ds_dv_dot_s = Vector3Ops::Dot( ds_dv, s_proj );
			ds_du = Vector3(
				ds_du.x - s_proj.x * ds_du_dot_s,
				ds_du.y - s_proj.y * ds_du_dot_s,
				ds_du.z - s_proj.z * ds_du_dot_s );
			ds_dv = Vector3(
				ds_dv.x - s_proj.x * ds_dv_dot_s,
				ds_dv.y - s_proj.y * ds_dv_dot_s,
				ds_dv.z - s_proj.z * ds_dv_dot_s );
			// t = n × s, so dt/d* = dn/d* × s + n × ds/d*.
			dt_du = Vector3Ops::Cross( v.dndu, s_proj )
				+ Vector3Ops::Cross( v.normal, ds_du );
			dt_dv = Vector3Ops::Cross( v.dndv, s_proj )
				+ Vector3Ops::Cross( v.normal, ds_dv );
		}

		// Use the Gram-Schmidt-projected s (consistent with the derivative
		// formula above) for the constraint-projection terms, overriding
		// the earlier s = Normalize(dpdu).  For well-conditioned tangent
		// frames these differ only by a tiny rotation.
		s = s_proj;
		t = t_cross;


		// ---- Diagonal block: dC_i / dX_i ----
		// dC_i_s / du = dot(ds_du, h) + dot(s, dh_du)
		// dC_i_s / dv = dot(ds_dv, h) + dot(s, dh_dv)
		// dC_i_t / du = dot(dt_du, h) + dot(t, dh_du)
		// dC_i_t / dv = dot(dt_dv, h) + dot(t, dh_dv)
		diag[i*4 + 0] = Vector3Ops::Dot( ds_du, h ) + Vector3Ops::Dot( s, dh_du );  // row0, col0
		diag[i*4 + 1] = Vector3Ops::Dot( ds_dv, h ) + Vector3Ops::Dot( s, dh_dv );  // row0, col1
		diag[i*4 + 2] = Vector3Ops::Dot( dt_du, h ) + Vector3Ops::Dot( t, dh_du );  // row1, col0
		diag[i*4 + 3] = Vector3Ops::Dot( dt_dv, h ) + Vector3Ops::Dot( t, dh_dv );  // row1, col1

		// ---- Off-diagonal blocks: dC_i / dX_{i-1}  and  dC_i / dX_{i+1} ----
		//
		// Moving vertex i-1 only affects wi (not wo).
		// Moving vertex i+1 only affects wo (not wi).
		// The tangent frame (s, t) of vertex i does NOT change when
		// neighboring vertices move — only h changes.

		// Upper block: dC_i / dX_{i+1}  (if i < k-1)
		// Moving vertex i+1 by dp changes wo:
		//   dwo/dp_next = +(dp_next - wo * dot(wo, dp_next)) / dist_o
		// Note the +sign: when the next vertex moves by +dp, wo changes positively.
		if( i < k - 1 )
		{
			const ManifoldVertex& vn = chain[i+1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vn.dpdu : vn.dpdv;
				const Vector3 dwo = Vector3(
					(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

				Vector3 dh_raw_next;
				if( v.isReflection )
					dh_raw_next = dwo;
				else
					// Walter form: ∂h/∂p_{i+1} = -η_t ∂wo/∂p (only wo
					// depends on next vertex)
					dh_raw_next = Vector3( -eta_t_v * dwo.x, -eta_t_v * dwo.y, -eta_t_v * dwo.z );

				const Vector3 dh_next = DeriveNormalized( h, dh_raw_next, h_len );

				// upper[i] maps vertex i+1 to constraint i
				upper[i*4 + 0 + p] = Vector3Ops::Dot( s, dh_next );
				upper[i*4 + 2 + p] = Vector3Ops::Dot( t, dh_next );
			}
		}

		// Lower block: dC_i / dX_{i-1}  (if i > 0)
		// Moving vertex i-1 by dp changes wi:
		//   dwi/dp_prev = +(dp_prev - wi * dot(wi, dp_prev)) / dist_i
		if( i > 0 )
		{
			const ManifoldVertex& vp = chain[i-1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vp.dpdu : vp.dpdv;
				const Vector3 dwi = Vector3(
					(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

				Vector3 dh_raw_prev;
				if( v.isReflection )
					dh_raw_prev = dwi;
				else
					// Walter form: ∂h/∂p_{i-1} = -η_i ∂wi/∂p (only wi
					// depends on previous vertex).  The OLD code had
					// no eta factor here — equivalent to assuming
					// η_i = 1 (the single-dielectric-in-air case
					// where the Walter form divided by η_i puts
					// 1·wi on the wi side).  For nested dielectrics
					// where the previous-side medium IS the object
					// (e.g. crossing into the air-cavity from
					// glass), η_i = 1.5 here and matters.
					dh_raw_prev = Vector3( -eta_i_v * dwi.x, -eta_i_v * dwi.y, -eta_i_v * dwi.z );

				const Vector3 dh_prev = DeriveNormalized( h, dh_raw_prev, h_len );

				// lower[i-1] maps vertex i-1 to constraint i
				lower[(i-1)*4 + 0 + p] = Vector3Ops::Dot( s, dh_prev );
				lower[(i-1)*4 + 2 + p] = Vector3Ops::Dot( t, dh_prev );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// SolveBlockTridiagonal
//
//   Solves J * delta = rhs where J is block-tridiagonal.
//   Each 2x2 block is stored as 4 scalars [a,b,c,d] in row-major.
//   diag is modified in place.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::Invert2x2( const Scalar* m, Scalar* inv )
{
	const Scalar det = m[0] * m[3] - m[1] * m[2];
	if( fabs(det) < NEARZERO )
	{
		return false;
	}
	const Scalar inv_det = 1.0 / det;
	inv[0] =  m[3] * inv_det;
	inv[1] = -m[1] * inv_det;
	inv[2] = -m[2] * inv_det;
	inv[3] =  m[0] * inv_det;
	return true;
}

void ManifoldSolver::Mul2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0]*B[0] + A[1]*B[2];
	C[1] = A[0]*B[1] + A[1]*B[3];
	C[2] = A[2]*B[0] + A[3]*B[2];
	C[3] = A[2]*B[1] + A[3]*B[3];
}

void ManifoldSolver::Mul2x2Vec( const Scalar* A, const Scalar* v, Scalar* r )
{
	r[0] = A[0]*v[0] + A[1]*v[1];
	r[1] = A[2]*v[0] + A[3]*v[1];
}

void ManifoldSolver::Sub2x2( const Scalar* A, const Scalar* B, Scalar* C )
{
	C[0] = A[0] - B[0];
	C[1] = A[1] - B[1];
	C[2] = A[2] - B[2];
	C[3] = A[3] - B[3];
}

//////////////////////////////////////////////////////////////////////
// ComputeDielectricFresnel
//
//   Exact dielectric Fresnel reflectance (unpolarized average of
//   s- and p-polarized components).  Returns 1.0 for TIR.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeDielectricFresnel(
	Scalar cosI,
	Scalar eta_i,
	Scalar eta_t
	)
{
	if( cosI < 0 ) cosI = -cosI;

	const Scalar sinI2 = 1.0 - cosI * cosI;
	const Scalar sinT2 = (eta_i * eta_i) / (eta_t * eta_t) * sinI2;

	if( sinT2 >= 1.0 )
	{
		return 1.0;  // Total internal reflection
	}

	const Scalar cosT = sqrt( 1.0 - sinT2 );
	const Scalar rs = (eta_i * cosI - eta_t * cosT) / (eta_i * cosI + eta_t * cosT);
	const Scalar rp = (eta_t * cosI - eta_i * cosT) / (eta_t * cosI + eta_i * cosT);
	return (rs * rs + rp * rp) * 0.5;
}

//////////////////////////////////////////////////////////////////////
// ComputeSphericalDerivatives
//
//   World-space gradients of theta and phi from DirectionToSpherical.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSphericalDerivatives(
	const Vector3& dir,
	const Vector3& s,
	const Vector3& t,
	const Vector3& normal,
	Vector3& dTheta_dDir,
	Vector3& dPhi_dDir
	)
{
	const Scalar x = Vector3Ops::Dot( dir, s );
	const Scalar y = Vector3Ops::Dot( dir, t );
	const Scalar z = Vector3Ops::Dot( dir, normal );

	// theta = acos(z), d(theta)/d(z) = -1/sin(theta)
	const Scalar sinTheta = sqrt( fmax( 1.0 - z * z, 0.0 ) );
	if( sinTheta > NEARZERO )
	{
		const Scalar invSin = -1.0 / sinTheta;
		dTheta_dDir = Vector3( invSin * normal.x, invSin * normal.y, invSin * normal.z );
	}
	else
	{
		// dir ≈ ±normal, theta gradient is degenerate
		dTheta_dDir = Vector3( 0, 0, 0 );
	}

	// phi = atan2(y, x), d(phi)/d(x) = -y/r², d(phi)/d(y) = x/r²
	const Scalar r2 = x * x + y * y;
	if( r2 > NEARZERO )
	{
		const Scalar invR2 = 1.0 / r2;
		// dPhi/d(dir) = (-y/r²) * s + (x/r²) * t
		dPhi_dDir = Vector3(
			(-y * invR2) * s.x + (x * invR2) * t.x,
			(-y * invR2) * s.y + (x * invR2) * t.y,
			(-y * invR2) * s.z + (x * invR2) * t.z );
	}
	else
	{
		// dir ≈ ±normal, phi gradient is degenerate
		dPhi_dDir = Vector3( 0, 0, 0 );
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeSpecularDirectionDerivativeWrtWi
//
//   3x3 Jacobian d(wo)/d(wi) for reflection/refraction.
//   Assumes unnormalized wo (before normalization in
//   ComputeSpecularDirection).
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeSpecularDirectionDerivativeWrtWi(
	const Vector3& wi,
	const Vector3& normal,
	Scalar eta,
	bool isReflection,
	Scalar dwo_dwi[9]
	)
{
	if( isReflection )
	{
		// wo = -wi + 2*(wi·n)*n
		// d(wo)/d(wi) = -I + 2*outer(n, n)
		dwo_dwi[0] = -1.0 + 2.0 * normal.x * normal.x;
		dwo_dwi[1] =        2.0 * normal.x * normal.y;
		dwo_dwi[2] =        2.0 * normal.x * normal.z;
		dwo_dwi[3] =        2.0 * normal.y * normal.x;
		dwo_dwi[4] = -1.0 + 2.0 * normal.y * normal.y;
		dwo_dwi[5] =        2.0 * normal.y * normal.z;
		dwo_dwi[6] =        2.0 * normal.z * normal.x;
		dwo_dwi[7] =        2.0 * normal.z * normal.y;
		dwo_dwi[8] = -1.0 + 2.0 * normal.z * normal.z;
	}
	else
	{
		// wo = -eta_ratio*wi + (eta_ratio*cos_i - cos_t)*n
		// For entering (cos_i > 0): eta_ratio = 1/eta
		// For exiting (cos_i < 0): eta_ratio = eta
		const Scalar cos_i = Vector3Ops::Dot( wi, normal );

		Vector3 n = normal;
		Scalar eta_ratio = 1.0 / eta;
		Scalar ci = cos_i;

		if( ci < 0.0 )
		{
			n = Vector3( -normal.x, -normal.y, -normal.z );
			ci = -ci;
			eta_ratio = eta;
		}

		const Scalar sin2_t = eta_ratio * eta_ratio * (1.0 - ci * ci);
		Scalar cos_t = 0.0;
		if( sin2_t < 1.0 )
			cos_t = sqrt( 1.0 - sin2_t );
		if( cos_t < NEARZERO )
			cos_t = NEARZERO;

		// d(wo)/d(wi) = -eta_ratio*I + eta_ratio*(1 - eta_ratio*ci/cos_t)*outer(n, n)
		const Scalar factor = eta_ratio * (1.0 - eta_ratio * ci / cos_t);

		dwo_dwi[0] = -eta_ratio + factor * n.x * n.x;
		dwo_dwi[1] =              factor * n.x * n.y;
		dwo_dwi[2] =              factor * n.x * n.z;
		dwo_dwi[3] =              factor * n.y * n.x;
		dwo_dwi[4] = -eta_ratio + factor * n.y * n.y;
		dwo_dwi[5] =              factor * n.y * n.z;
		dwo_dwi[6] =              factor * n.z * n.x;
		dwo_dwi[7] =              factor * n.z * n.y;
		dwo_dwi[8] = -eta_ratio + factor * n.z * n.z;
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianAngleDiffNumerical
//
//   Numerical Jacobian for the angle-difference constraint.
//   Perturbs vertices and evaluates EvaluateConstraintAtVertex.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianAngleDiffNumerical(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	const Scalar eps = 1e-5;
	const Scalar inv2eps = 1.0 / (2.0 * eps);

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	// Helper lambda: evaluate angle-diff constraint at vertex ci
	// given modified chain positions
	auto evalAtVertex = [&]( unsigned int ci,
		const Point3& prevP, const Point3& nextP,
		const Point3& vPos, const Vector3& vNorm,
		Scalar& C0, Scalar& C1 )
	{
		const ManifoldVertex& v = chain[ci];
		EvaluateConstraintAtVertex(
			vPos, vNorm, v.dpdu, v.dpdv,
			v.eta, v.isReflection,
			prevP, nextP, C0, C1 );
	};

	for( unsigned int j = 0; j < k; j++ )
	{
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? chain[j].dpdu : chain[j].dpdv;
			const Vector3& dn = (p == 0) ? chain[j].dndu : chain[j].dndv;

			const Point3 posPlus = Point3Ops::mkPoint3( chain[j].position, dp * eps );
			const Point3 posMinus = Point3Ops::mkPoint3( chain[j].position, dp * (-eps) );
			Vector3 normPlus = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x + dn.x * eps,
				chain[j].normal.y + dn.y * eps,
				chain[j].normal.z + dn.z * eps ) );
			Vector3 normMinus = Vector3Ops::Normalize( Vector3(
				chain[j].normal.x - dn.x * eps,
				chain[j].normal.y - dn.y * eps,
				chain[j].normal.z - dn.z * eps ) );

			// Diagonal: constraint j w.r.t. vertex j
			{
				const Point3 prevP = (j == 0) ? fixedStart : chain[j-1].position;
				const Point3 nextP = (j == k-1) ? fixedEnd : chain[j+1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( j, prevP, nextP, posPlus, normPlus, Cp0, Cp1 );
				evalAtVertex( j, prevP, nextP, posMinus, normMinus, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				diag[j*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				diag[j*4 + 2 + p] = dC1 * inv2eps;
			}

			// Upper: constraint j-1 w.r.t. vertex j (if j > 0)
			if( j > 0 )
			{
				unsigned int ci = j - 1;
				const Point3 prevP = (ci == 0) ? fixedStart : chain[ci-1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( ci, prevP, posPlus, chain[ci].position, chain[ci].normal, Cp0, Cp1 );
				evalAtVertex( ci, prevP, posMinus, chain[ci].position, chain[ci].normal, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				upper[ci*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				upper[ci*4 + 2 + p] = dC1 * inv2eps;
			}

			// Lower: constraint j+1 w.r.t. vertex j (if j < k-1)
			if( j < k - 1 )
			{
				unsigned int ci = j + 1;
				const Point3 nextP = (ci == k-1) ? fixedEnd : chain[ci+1].position;

				Scalar Cp0, Cp1, Cm0, Cm1;
				evalAtVertex( ci, posPlus, nextP, chain[ci].position, chain[ci].normal, Cp0, Cp1 );
				evalAtVertex( ci, posMinus, nextP, chain[ci].position, chain[ci].normal, Cm0, Cm1 );

				Scalar dC1 = Cp1 - Cm1;
				if( dC1 > PI ) dC1 -= 2.0 * PI;
				if( dC1 < -PI ) dC1 += 2.0 * PI;

				lower[j*4 + 0 + p] = (Cp0 - Cm0) * inv2eps;
				lower[j*4 + 2 + p] = dC1 * inv2eps;
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// BuildJacobianAngleDiff
//
//   Analytical Jacobian for the angle-difference constraint.
//   Chain rule through spherical coordinates, specular direction,
//   and surface curvature (Weingarten map).
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::BuildJacobianAngleDiff(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	std::vector<Scalar>& diag,
	std::vector<Scalar>& upper,
	std::vector<Scalar>& lower
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	diag.resize( k * 4, 0.0 );
	if( k > 1 )
	{
		upper.resize( (k-1) * 4, 0.0 );
		lower.resize( (k-1) * 4, 0.0 );
	}
	else
	{
		upper.clear();
		lower.clear();
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		// Directions and distances
		Vector3 d_wi = Vector3Ops::mkVector3( prevPos, v.position );
		const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
		const Vector3 wi = d_wi;

		Vector3 d_wo = Vector3Ops::mkVector3( nextPos, v.position );
		const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
		const Vector3 wo = d_wo;

		if( dist_i < NEARZERO || dist_o < NEARZERO ) continue;

		const Scalar inv_li = 1.0 / dist_i;
		const Scalar inv_lo = 1.0 / dist_o;

		// Tangent frame (normalized)
		const Vector3 s = Vector3Ops::Normalize( v.dpdu );
		const Vector3 t = Vector3Ops::Normalize( v.dpdv );

		// Specularly scattered direction
		Vector3 wo_spec;
		if( !ComputeSpecularDirection( wi, v.normal, v.eta, v.isReflection, wo_spec ) )
		{
			// TIR — degenerate, leave zero entries
			continue;
		}

		// Spherical derivatives for wo_actual and wo_specular
		Vector3 dThetaA_dDir, dPhiA_dDir;
		ComputeSphericalDerivatives( wo, s, t, v.normal, dThetaA_dDir, dPhiA_dDir );

		Vector3 dThetaS_dDir, dPhiS_dDir;
		ComputeSphericalDerivatives( wo_spec, s, t, v.normal, dThetaS_dDir, dPhiS_dDir );

		// d(wo_spec)/d(wi) and d(wo_spec)/d(n) — 3x3 Jacobians
		Scalar dwoSpec_dwi[9], dwoSpec_dn[9];
		ComputeSpecularDirectionDerivativeWrtWi( wi, v.normal, v.eta, v.isReflection, dwoSpec_dwi );
		ComputeSpecularDirectionDerivativeWrtNormal( wi, v.normal, v.eta, v.isReflection, dwoSpec_dn );

		// ---- Diagonal block: dC_i / dX_i ----
		for( unsigned int p = 0; p < 2; p++ )
		{
			const Vector3& dp = (p == 0) ? v.dpdu : v.dpdv;
			const Vector3& dn = (p == 0) ? v.dndu : v.dndv;

			// d(wo_actual)/du: moving vertex changes direction to next vertex
			const Vector3 dwo_du = Vector3(
				-(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
				-(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
				-(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

			// d(wi)/du: moving vertex changes direction to previous vertex
			const Vector3 dwi_du = Vector3(
				-(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
				-(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
				-(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

			// d(wo_spec)/du = dwoSpec_dwi × dwi_du + dwoSpec_dn × dn
			Vector3 dwoSpec_du;
			dwoSpec_du.x = dwoSpec_dwi[0]*dwi_du.x + dwoSpec_dwi[1]*dwi_du.y + dwoSpec_dwi[2]*dwi_du.z
				         + dwoSpec_dn[0]*dn.x + dwoSpec_dn[1]*dn.y + dwoSpec_dn[2]*dn.z;
			dwoSpec_du.y = dwoSpec_dwi[3]*dwi_du.x + dwoSpec_dwi[4]*dwi_du.y + dwoSpec_dwi[5]*dwi_du.z
				         + dwoSpec_dn[3]*dn.x + dwoSpec_dn[4]*dn.y + dwoSpec_dn[5]*dn.z;
			dwoSpec_du.z = dwoSpec_dwi[6]*dwi_du.x + dwoSpec_dwi[7]*dwi_du.y + dwoSpec_dwi[8]*dwi_du.z
				         + dwoSpec_dn[6]*dn.x + dwoSpec_dn[7]*dn.y + dwoSpec_dn[8]*dn.z;

			// dC0/du = dTheta_actual/d(dir) · dwo_du - dTheta_spec/d(dir) · dwoSpec_du
			Scalar dC0_dp = Vector3Ops::Dot( dThetaA_dDir, dwo_du )
				          - Vector3Ops::Dot( dThetaS_dDir, dwoSpec_du );

			// dC1/du = dPhi_actual/d(dir) · dwo_du - dPhi_spec/d(dir) · dwoSpec_du
			Scalar dC1_dp = Vector3Ops::Dot( dPhiA_dDir, dwo_du )
				          - Vector3Ops::Dot( dPhiS_dDir, dwoSpec_du );

			// Frame rotation contribution: when the vertex moves, the
			// tangent frame (s, t, n) rotates, changing how the same
			// world-space direction maps to spherical coordinates.
			// For theta = acos(dir·n): d(theta)/d(n_pert) = -(1/sin(theta)) * dir
			// For phi: d(phi)/d(s_pert) and d(phi)/d(t_pert) via chain rule
			// These enter as additional terms for both wo_actual and wo_spec.
			//
			// theta_actual frame term: d(acos(wo·n))/d(n) × dn/du
			const Scalar sinThetaA = sqrt( fmax( 1.0 - Vector3Ops::Dot(wo, v.normal) * Vector3Ops::Dot(wo, v.normal), 0.0 ) );
			if( sinThetaA > NEARZERO )
			{
				const Scalar dthetaA_dn = -Vector3Ops::Dot( wo, dn ) / sinThetaA;
				const Scalar sinThetaS = sqrt( fmax( 1.0 - Vector3Ops::Dot(wo_spec, v.normal) * Vector3Ops::Dot(wo_spec, v.normal), 0.0 ) );
				const Scalar dthetaS_dn = (sinThetaS > NEARZERO) ?
					-Vector3Ops::Dot( wo_spec, dn ) / sinThetaS : 0.0;
				dC0_dp += dthetaA_dn - dthetaS_dn;
			}

			// phi frame terms (tangent rotation) — smaller effect, skip for now
			// to avoid excessive complexity.  The numerical Jacobian will
			// validate whether this omission matters.

			diag[i*4 + 0 + p] = dC0_dp;
			diag[i*4 + 2 + p] = dC1_dp;
		}

		// ---- Upper block: dC_i / dX_{i+1} ----
		if( i < k - 1 )
		{
			const ManifoldVertex& vn = chain[i+1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vn.dpdu : vn.dpdv;

				// Moving next vertex only affects wo_actual
				const Vector3 dwo_next = Vector3(
					(dp.x - wo.x * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.y - wo.y * Vector3Ops::Dot( wo, dp )) * inv_lo,
					(dp.z - wo.z * Vector3Ops::Dot( wo, dp )) * inv_lo );

				upper[i*4 + 0 + p] = Vector3Ops::Dot( dThetaA_dDir, dwo_next );
				upper[i*4 + 2 + p] = Vector3Ops::Dot( dPhiA_dDir, dwo_next );
			}
		}

		// ---- Lower block: dC_i / dX_{i-1} ----
		if( i > 0 )
		{
			const ManifoldVertex& vp = chain[i-1];
			for( unsigned int p = 0; p < 2; p++ )
			{
				const Vector3& dp = (p == 0) ? vp.dpdu : vp.dpdv;

				// Moving previous vertex only affects wi → wo_specular
				const Vector3 dwi_prev = Vector3(
					(dp.x - wi.x * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.y - wi.y * Vector3Ops::Dot( wi, dp )) * inv_li,
					(dp.z - wi.z * Vector3Ops::Dot( wi, dp )) * inv_li );

				// d(wo_spec)/d(wi_prev) = dwoSpec_dwi × dwi_prev
				Vector3 dwoSpec_prev;
				dwoSpec_prev.x = dwoSpec_dwi[0]*dwi_prev.x + dwoSpec_dwi[1]*dwi_prev.y + dwoSpec_dwi[2]*dwi_prev.z;
				dwoSpec_prev.y = dwoSpec_dwi[3]*dwi_prev.x + dwoSpec_dwi[4]*dwi_prev.y + dwoSpec_dwi[5]*dwi_prev.z;
				dwoSpec_prev.z = dwoSpec_dwi[6]*dwi_prev.x + dwoSpec_dwi[7]*dwi_prev.y + dwoSpec_dwi[8]*dwi_prev.z;

				lower[(i-1)*4 + 0 + p] = -Vector3Ops::Dot( dThetaS_dDir, dwoSpec_prev );
				lower[(i-1)*4 + 2 + p] = -Vector3Ops::Dot( dPhiS_dDir, dwoSpec_prev );
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////
// ValidateChainPhysics
//
//   Checks that converged specular vertices have physically
//   consistent geometry: refraction requires wi and wo on opposite
//   sides of the surface; reflection requires both on the same side.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ValidateChainPhysics(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );
		Vector3 wo = Vector3Ops::mkVector3( nextPos, v.position );
		wo = Vector3Ops::Normalize( wo );

		const Scalar wiDotN = Vector3Ops::Dot( wi, v.normal );
		const Scalar woDotN = Vector3Ops::Dot( wo, v.normal );

		if( v.isReflection )
		{
			// Reflection: both directions on the same side of surface
			if( wiDotN * woDotN < 0.0 )
			{
				return false;
			}
		}
		else
		{
			// Refraction: directions on opposite sides of surface
			if( wiDotN * woDotN > 0.0 )
			{
				return false;
			}
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// ComputeBlockTridiagonalDeterminant
//
//   Computes det(J) of a block-tridiagonal matrix via LU forward
//   elimination.  det = product of det(Dp[i]) for each 2x2 block.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeBlockTridiagonalDeterminant(
	const std::vector<Scalar>& diag,
	const std::vector<Scalar>& upper,
	const std::vector<Scalar>& lower,
	unsigned int k
	) const
{
	if( k == 0 ) return 1.0;

	std::vector<Scalar> Dp( k * 4 );
	Scalar detProduct = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			for( int q = 0; q < 4; q++ )
				Dp[q] = diag[q];
		}
		else
		{
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return 0.0;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );
		}

		const Scalar blkDet = Dp[i*4+0] * Dp[i*4+3] - Dp[i*4+1] * Dp[i*4+2];
		detProduct *= blkDet;
	}

	return detProduct;
}

bool ManifoldSolver::SolveBlockTridiagonal(
	std::vector<Scalar>& diag,
	const std::vector<Scalar>& upper,
	const std::vector<Scalar>& lower,
	const std::vector<Scalar>& rhs,
	unsigned int k,
	std::vector<Scalar>& delta
	) const
{
	if( k == 0 )
	{
		return false;
	}

	delta.resize( 2 * k, 0.0 );

	// Modified diagonal blocks and modified rhs
	// We'll work with arrays of 2x2 blocks (4 scalars each) and 2-vectors
	std::vector<Scalar> Dp( k * 4 );   // modified diagonal
	std::vector<Scalar> rp( k * 2 );   // modified rhs

	// Forward sweep
	for( unsigned int i = 0; i < k; i++ )
	{
		if( i == 0 )
		{
			// Dp[0] = D[0]
			for( int q = 0; q < 4; q++ ) {
				Dp[q] = diag[q];
			}
			rp[0] = rhs[0];
			rp[1] = rhs[1];
		}
		else
		{
			// Dp[i] = D[i] - L[i] * inv(Dp[i-1]) * U[i-1]
			Scalar invDp[4];
			if( !Invert2x2( &Dp[(i-1)*4], invDp ) )
			{
				return false;
			}

			Scalar LiInvDp[4];
			Mul2x2( &lower[(i-1)*4], invDp, LiInvDp );

			Scalar LiInvDpUi[4];
			Mul2x2( LiInvDp, &upper[(i-1)*4], LiInvDpUi );

			Sub2x2( &diag[i*4], LiInvDpUi, &Dp[i*4] );

			// rp[i] = rhs[i] - L[i] * inv(Dp[i-1]) * rp[i-1]
			Scalar LiInvDpRhs[2];
			Mul2x2Vec( LiInvDp, &rp[(i-1)*2], LiInvDpRhs );

			rp[i*2]   = rhs[i*2]   - LiInvDpRhs[0];
			rp[i*2+1] = rhs[i*2+1] - LiInvDpRhs[1];
		}
	}

	// Back substitution
	for( int i = static_cast<int>(k) - 1; i >= 0; i-- )
	{
		Scalar rhs_i[2];
		rhs_i[0] = rp[i*2];
		rhs_i[1] = rp[i*2+1];

		if( i < static_cast<int>(k) - 1 )
		{
			// rhs_i -= U[i] * delta[i+1]
			Scalar Ud[2];
			Mul2x2Vec( &upper[i*4], &delta[(i+1)*2], Ud );
			rhs_i[0] -= Ud[0];
			rhs_i[1] -= Ud[1];
		}

		// delta[i] = inv(Dp[i]) * rhs_i
		Scalar invDp[4];
		if( !Invert2x2( &Dp[i*4], invDp ) )
		{
			return false;
		}
		Mul2x2Vec( invDp, rhs_i, &delta[i*2] );
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// UpdateVertexOnSurface
//
//   Steps a vertex along the surface parameterization by (du, dv)
//   and re-snaps to the surface via ray intersection.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::UpdateVertexOnSurface(
	ManifoldVertex& vertex,
	Scalar du,
	Scalar dv,
	Scalar smoothing
	) const
{
	if( !vertex.pObject )
	{
		return false;
	}

	// SMS two-stage Stage 1 path: take the step in (u, v) space directly
	// and re-evaluate the smoothing-aware analytical surface.  Bypasses
	// the mesh ray-cast snap below — at smoothing > 0 the chain lives on
	// a smoothed surface that the underlying mesh doesn't approximate, so
	// the snap would put us back on the actual surface and undo Stage 1's
	// reason for existing.  See docs/SMS_TWO_STAGE_SOLVER.md.
	if( smoothing > 0.0 )
	{
		Point2 newUv( vertex.uv.x + du, vertex.uv.y + dv );
		// Spherical-style pole wrap.  For sphere/ellipsoid parameter-
		// isations, crossing a pole (v < 0 or v > 1) reflects the v
		// coordinate and shifts u by half a turn — same surface point
		// reached by going the other way around.  Without this, a
		// clamp-to-[0,1] would put the vertex AT the pole, where
		// dpdu = sin(phi)·(...) collapses to zero and the Jacobian
		// becomes singular.  Empirically (SMS_NEWTON_STATS' singular
		// counter) clamping caused ≥17% of Newton calls to degenerate
		// on the displaced-egg scene's two-stage Stage 1.
		if( newUv.y < 0.0 ) {
			newUv.y = -newUv.y;
			newUv.x += 0.5;
		} else if( newUv.y > 1.0 ) {
			newUv.y = 2.0 - newUv.y;
			newUv.x += 0.5;
		}
		while( newUv.x < 0.0 ) newUv.x += 1.0;
		while( newUv.x >= 1.0 ) newUv.x -= 1.0;
		Point3  aP;
		Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
		if( vertex.pObject->ComputeAnalyticalDerivatives(
				newUv, smoothing, aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
		{
			vertex.uv       = newUv;
			vertex.position = aP;
			vertex.normal   = aN;
			vertex.dpdu     = aDpdu;
			vertex.dpdv     = aDpdv;
			vertex.dndu     = aDndu;
			vertex.dndv     = aDndv;
			OrthonormalizeTangentFrame( vertex );
			vertex.valid = true;
			return true;
		}
		// Analytical query failed — caller (NewtonSolve under two-stage
		// Stage 1) treats as a rejected step and halves β.
		return false;
	}

	// Linear approximation of new position using tangent derivatives
	const Point3 newPos = Point3Ops::mkPoint3(
		vertex.position,
		vertex.dpdu * du + vertex.dpdv * dv
		);

	// Also update normal using normal derivatives (first-order)
	Vector3 newNormal = Vector3(
		vertex.normal.x + vertex.dndu.x * du + vertex.dndv.x * dv,
		vertex.normal.y + vertex.dndu.y * du + vertex.dndv.y * dv,
		vertex.normal.z + vertex.dndu.z * du + vertex.dndv.z * dv
		);
	newNormal = Vector3Ops::Normalize( newNormal );

	// For small steps, use linear approximation for position/normal
	// but always re-snap to the actual surface via intersection so
	// we get accurate derivatives for the next Newton step.
	const Scalar stepSize = sqrt( du * du + dv * dv );
	if( stepSize < 1e-8 )
	{
		// Negligible step — no change needed
		vertex.valid = true;
		return true;
	}

	// Project back onto the surface via ray intersection.
	// The probe offset should be small enough to stay near the current
	// surface but large enough to clear the local geometry.
	const Scalar probeOffset = fmin( fmax( stepSize * 2.0, 0.01 ), 0.5 );
	bool snapped = false;

	// Try from the normal side first
	{
		const Ray probeRay(
			Point3Ops::mkPoint3( newPos, newNormal * probeOffset ),
			Vector3( -newNormal.x, -newNormal.y, -newNormal.z )
			);

		RayIntersection ri( probeRay, nullRasterizerState );
		vertex.pObject->IntersectRay( ri, 2.0 * probeOffset, true, true, false );

		if( ri.geometric.bHit )
		{
			// Apply modifier — same reason as in BuildSeedChain.  Stage 2
			// of the two-stage solver (smoothing == 0) wants the perturbed
			// normal so SMS's chain matches PT's bumpy ray traversal.
			if( ri.pModifier ) {
				ri.pModifier->Modify( ri.geometric );
			}
			vertex.position = ri.geometric.ptIntersection;
			vertex.normal = ri.geometric.vNormal;
			vertex.uv = ri.geometric.ptCoord;
			snapped = true;
		}
	}

	if( !snapped )
	{
		// Try from the other side
		const Ray probeRay2(
			Point3Ops::mkPoint3( newPos, newNormal * (-probeOffset) ),
			newNormal
			);

		RayIntersection ri2( probeRay2, nullRasterizerState );
		vertex.pObject->IntersectRay( ri2, 2.0 * probeOffset, true, true, false );

		if( ri2.geometric.bHit )
		{
			if( ri2.pModifier ) {
				ri2.pModifier->Modify( ri2.geometric );
			}
			vertex.position = ri2.geometric.ptIntersection;
			vertex.normal = ri2.geometric.vNormal;
			vertex.uv = ri2.geometric.ptCoord;
			snapped = true;
		}
	}

	if( !snapped )
	{
		// Fall back to the linear approximation (no re-snap)
		vertex.position = newPos;
		vertex.normal = newNormal;
	}

	// Recompute surface derivatives at the new position
	vertex.valid = ComputeVertexDerivatives( vertex );
	return vertex.valid;
}

//////////////////////////////////////////////////////////////////////
// ComputeVertexDerivatives
//
//   Fills in dpdu, dpdv, dndu, dndv by querying the object's
//   geometry with proper world/object space transforms.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::OrthonormalizeTangentFrame(
	ManifoldVertex& vertex
	) const
{
	// Project dpdu and dpdv into the tangent plane.
	const Vector3& n = vertex.normal;
	Scalar d;

	d = Vector3Ops::Dot( vertex.dpdu, n );
	vertex.dpdu = Vector3(
		vertex.dpdu.x - n.x * d,
		vertex.dpdu.y - n.y * d,
		vertex.dpdu.z - n.z * d );

	d = Vector3Ops::Dot( vertex.dpdv, n );
	vertex.dpdv = Vector3(
		vertex.dpdv.x - n.x * d,
		vertex.dpdv.y - n.y * d,
		vertex.dpdv.z - n.z * d );

	// Gram-Schmidt: make dpdv perpendicular to dpdu.
	//
	// CRITICAL: apply the same projection to dndv (normal derivative)
	// as we apply to dpdv.  Gram-Schmidt with
	//     dpdv' = dpdv - α·dpdu     (α = (dpdv·dpdu)/|dpdu|²)
	// is equivalent to reparameterizing along a new v-direction
	// v' = v - α·u.  By the chain rule dn/dv' = dn/dv - α·dn/du, so
	// dndv must receive the same correction or it ends up representing
	// the rate of change in the OLD (non-orthogonal) v-direction while
	// dpdv and the subsequent 1/|dpdv| rescale below are in the NEW
	// (orthogonal) v-direction — inflating |dndv| by 1/sin(θ) where θ
	// is the angle between the raw dpdu and dpdv.
	// On analytic surfaces (sphere_geometry etc.) the raw dpdu, dpdv
	// are already orthogonal and both projections collapse to zero —
	// a no-op that costs a few flops but doesn't change results.
	// On triangle meshes (including displaced_geometry with disp_scale=0)
	// the raw derivatives come from e1=V1-V0, e2=V2-V0 which are NEVER
	// orthogonal, so the correction is essential.
	const Scalar uSq = Vector3Ops::SquaredModulus( vertex.dpdu );
	if( uSq > NEARZERO ) {
		const Scalar proj = Vector3Ops::Dot( vertex.dpdv, vertex.dpdu ) / uSq;
		vertex.dpdv = Vector3(
			vertex.dpdv.x - vertex.dpdu.x * proj,
			vertex.dpdv.y - vertex.dpdu.y * proj,
			vertex.dpdv.z - vertex.dpdu.z * proj );
		vertex.dndv = Vector3(
			vertex.dndv.x - vertex.dndu.x * proj,
			vertex.dndv.y - vertex.dndu.y * proj,
			vertex.dndv.z - vertex.dndu.z * proj );
	}

	// If dpdu collapsed, pick any tangent.
	const Scalar uLen = Vector3Ops::Magnitude( vertex.dpdu );
	if( uLen < NEARZERO ) {
		vertex.dpdu = Vector3Ops::Normalize( Vector3Ops::Perpendicular( n ) );
	} else {
		vertex.dpdu = vertex.dpdu * (1.0 / uLen);
	}

	// If dpdv collapsed, derive from cross product.
	const Scalar vLen = Vector3Ops::Magnitude( vertex.dpdv );
	if( vLen < NEARZERO ) {
		vertex.dpdv = Vector3Ops::Cross( n, vertex.dpdu );
	} else {
		vertex.dpdv = vertex.dpdv * (1.0 / vLen);
	}

	// Scale normal derivatives so they represent rate of change per unit
	// displacement in the NEW unit-length (dpdu, dpdv) basis.  The raw
	// dn*/d* entries from the geometry are in the ORIGINAL parameterization
	// scale (e.g. per triangle edge).  Dividing by the original magnitude
	// converts to per-unit-length.  For flat surfaces these are zero
	// either way and the rescale is a no-op.
	if( uLen > NEARZERO ) {
		const Scalar invU = 1.0 / uLen;
		vertex.dndu = Vector3(
			vertex.dndu.x * invU,
			vertex.dndu.y * invU,
			vertex.dndu.z * invU );
	}
	if( vLen > NEARZERO ) {
		const Scalar invV = 1.0 / vLen;
		vertex.dndv = Vector3(
			vertex.dndv.x * invV,
			vertex.dndv.y * invV,
			vertex.dndv.z * invV );
	}
}

bool ManifoldSolver::ComputeVertexDerivatives(
	ManifoldVertex& vertex,
	Scalar smoothing
	) const
{
	if( !vertex.pObject )
	{
		return false;
	}

	// Smoothing-aware analytical path (SMS two-stage solver, Zeltner 2020 §5).
	// ONLY engaged at smoothing > 0 — at smoothing = 0 we want the
	// existing on-mesh derivatives (per-triangle UV-Jacobian for triangle
	// meshes, FD probe for smooth analytical primitives), because those
	// derivatives describe the *actual* surface that PT's emission-
	// suppression machinery has already paired with.  Replacing them with
	// the smooth analytical equivalent at smoothing = 0 was the rejected
	// "Fix C" — it gives Newton's J a different surface than the rendering
	// pipeline uses, and ΣL_sms/ΣL_supp collapses (see investigation
	// timeline in docs/SMS_TWO_STAGE_SOLVER.md).
	if( smoothing > 0.0 )
	{
		Point3  aP;
		Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
		if( vertex.pObject->ComputeAnalyticalDerivatives(
				vertex.uv, smoothing, aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
		{
			vertex.position = aP;
			vertex.normal   = aN;
			vertex.dpdu     = aDpdu;
			vertex.dpdv     = aDpdv;
			vertex.dndu     = aDndu;
			vertex.dndv     = aDndv;
			OrthonormalizeTangentFrame( vertex );
			vertex.valid = true;
			return true;
		}
		// No analytical path available — Stage 1 of the two-stage solver
		// can't proceed for this vertex.  Caller (Solve) sees failure
		// and falls back to single-stage Newton.
		return false;
	}

	// Fast path: try to get analytical derivatives at the current
	// position via a ray cast that the geometry can populate at
	// intersection time (triangle meshes do this; see
	// TriangleMeshGeometryIndexedSpecializations.h).  If the
	// geometry populates ri.derivatives, we skip the expensive
	// 4-probe FD walk below.
	//
	// This probe ALSO serves as the on-surface verification.  If the
	// vertex has been pushed off the specular geometry (e.g. Newton
	// stepping beyond a bounded plane's extent), the probe ray misses
	// and we reject the vertex.  Without this, the fallback FD path
	// would synthesize fake tangent frames from the stale normal and
	// Newton would converge to algebraically-satisfied but
	// geometrically-nonexistent paths, causing false contribution in
	// the penumbra of bounded refractors.
	bool onSurface = false;
	{
		const Scalar probeOffsetAnalytic = 0.05;
		const Ray probeRay(
			Point3Ops::mkPoint3( vertex.position, vertex.normal * probeOffsetAnalytic ),
			Vector3( -vertex.normal.x, -vertex.normal.y, -vertex.normal.z ) );
		RayIntersection ri( probeRay, nullRasterizerState );
		vertex.pObject->IntersectRay( ri, 2.0 * probeOffsetAnalytic, true, true, false );
		if( ri.geometric.bHit ) {
			// Apply intersection modifier (bump map / normal map) so SMS
			// sees the perturbed normal — same as RayCaster does for the
			// rendering pipeline.  Without this, SMS's chain operates on
			// the unperturbed surface while PT operates on the perturbed
			// one and energy doesn't balance.
			if( ri.pModifier ) {
				ri.pModifier->Modify( ri.geometric );
			}
			onSurface = true;
			if( ri.geometric.derivatives.valid ) {
				vertex.position = ri.geometric.ptIntersection;
				vertex.normal = ri.geometric.vNormal;
				vertex.dpdu = ri.geometric.derivatives.dpdu;
				vertex.dpdv = ri.geometric.derivatives.dpdv;
				vertex.dndu = ri.geometric.derivatives.dndu;
				vertex.dndv = ri.geometric.derivatives.dndv;
#if SMS_SMOOTH_BASE_JACOBIAN
				// Override the just-populated mesh derivatives with the
				// smooth analytical surface's derivatives at the same uv,
				// when the geometry exposes them.  Position and normal
				// stay from the ray-cast (residual is evaluated against
				// the actual bumpy mesh); only the Newton-Jacobian inputs
				// dpdu/dpdv/dndu/dndv switch to the smooth model.
				{
					Point3 aP;
					Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
					if( vertex.pObject->ComputeAnalyticalDerivatives(
							vertex.uv, Scalar( 1.0 ),
							aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
					{
						vertex.dpdu = aDpdu;
						vertex.dpdv = aDpdv;
						vertex.dndu = aDndu;
						vertex.dndv = aDndv;
					}
				}
#endif
				OrthonormalizeTangentFrame( vertex );
				vertex.valid = true;
				return true;
			}
		} else {
			// Probe from the opposite side too before giving up
			const Ray probeRay2(
				Point3Ops::mkPoint3( vertex.position, vertex.normal * (-probeOffsetAnalytic) ),
				vertex.normal );
			RayIntersection ri2( probeRay2, nullRasterizerState );
			vertex.pObject->IntersectRay( ri2, 2.0 * probeOffsetAnalytic, true, true, false );
			if( ri2.geometric.bHit ) {
				if( ri2.pModifier ) {
					ri2.pModifier->Modify( ri2.geometric );
				}
				onSurface = true;
				if( ri2.geometric.derivatives.valid ) {
					vertex.position = ri2.geometric.ptIntersection;
					vertex.normal = ri2.geometric.vNormal;
					vertex.dpdu = ri2.geometric.derivatives.dpdu;
					vertex.dpdv = ri2.geometric.derivatives.dpdv;
					vertex.dndu = ri2.geometric.derivatives.dndu;
					vertex.dndv = ri2.geometric.derivatives.dndv;
#if SMS_SMOOTH_BASE_JACOBIAN
					// See companion override at the front-probe site.
					{
						Point3 aP;
						Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
						if( vertex.pObject->ComputeAnalyticalDerivatives(
								vertex.uv, Scalar( 1.0 ),
								aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
						{
							vertex.dpdu = aDpdu;
							vertex.dpdv = aDpdv;
							vertex.dndu = aDndu;
							vertex.dndv = aDndv;
						}
					}
#endif
					OrthonormalizeTangentFrame( vertex );
					vertex.valid = true;
					return true;
				}
			}
		}
	}

	if( !onSurface ) {
		// Vertex is not on the specular geometry.  This happens when
		// Newton steps beyond the object's extent or when a chain
		// goes through empty space.  Reject — fake synthesized
		// derivatives would mislead the solver into accepting
		// non-physical paths.
		return false;
	}

	// Fallback: central-difference FD probes.  Kept for geometries
	// that don't yet populate ri.derivatives during IntersectRay.
	// (Sphere, torus, ellipsoid, cylinder, box, disk, clipped plane,
	// infinite plane — eventual plan is to populate derivatives at
	// intersection time for all of them, then this FD path can be
	// retired.)
	const Scalar eps = 5e-4;

	Vector3 tangent_u, tangent_v;

	if( Vector3Ops::SquaredModulus( vertex.dpdu ) > NEARZERO &&
		Vector3Ops::SquaredModulus( vertex.dpdv ) > NEARZERO )
	{
		tangent_u = Vector3Ops::Normalize( vertex.dpdu );
		tangent_v = Vector3Ops::Normalize( vertex.dpdv );
	}
	else
	{
		tangent_u = Vector3Ops::Perpendicular( vertex.normal );
		tangent_u = Vector3Ops::Normalize( tangent_u );
		tangent_v = Vector3Ops::Cross( vertex.normal, tangent_u );
		tangent_v = Vector3Ops::Normalize( tangent_v );
	}

	// Probe a surface point near vertex.position along a tangent direction.
	// Returns true if successful, filling outPos and outNormal.
	//
	// The probe ray shoots along -normal from above the test position, and
	// must cover the local surface displacement amplitude to be reliable.
	// Too small: misses displaced-mesh vertices whose local surface is
	// higher/lower than the offset (observed: 85% failure rate with 0.01
	// offset on a slab with 0.1 displacement range).  Too large: can
	// punch through thin geometry and hit the far surface.
	//
	// Strategy: try a sequence of offsets from small to large, stopping
	// at the first successful hit.  The smallest offset that works gives
	// the most accurate hit (least tangential drift of the probe).
	struct ProbeResult { Point3 pos; Vector3 normal; bool ok; };
	auto probeAt = [&]( const Point3& testPos ) -> ProbeResult
	{
		ProbeResult r;
		r.ok = false;

		// Try a sequence of probe offsets.  Start small to avoid punching
		// through thin geometry; grow to handle displaced meshes.
		const Scalar probeOffsets[] = { 0.01, 0.05, 0.2, 1.0 };
		const unsigned int nOffsets = sizeof(probeOffsets) / sizeof(probeOffsets[0]);

		for( unsigned int oi = 0; oi < nOffsets; oi++ )
		{
			const Scalar probeOffset = probeOffsets[oi];

			// Probe from the normal side (ray goes -n toward surface)
			{
				const Ray probeRay(
					Point3Ops::mkPoint3( testPos, vertex.normal * probeOffset ),
					Vector3( -vertex.normal.x, -vertex.normal.y, -vertex.normal.z ) );
				RayIntersection ri( probeRay, nullRasterizerState );
				vertex.pObject->IntersectRay( ri, 2.0 * probeOffset, true, true, false );
				if( ri.geometric.bHit )
				{
					// Apply modifier (e.g. bump map) so the FD captures
					// derivatives of the *perturbed* normal field, not the
					// unperturbed one.  Without this, vertex.dndu/dndv
					// describe the smooth surface even when vertex.normal
					// is bumpy — Newton's Jacobian sees a different
					// surface than the constraint.
					if( ri.pModifier ) {
						ri.pModifier->Modify( ri.geometric );
					}
					r.pos = ri.geometric.ptIntersection;
					r.normal = ri.geometric.vNormal;
					r.ok = true;
					return r;
				}
			}

			// Try from the other side (ray goes +n, for when the local
			// surface is on the other side of testPos)
			{
				const Ray probeRay2(
					Point3Ops::mkPoint3( testPos, vertex.normal * (-probeOffset) ),
					vertex.normal );
				RayIntersection ri2( probeRay2, nullRasterizerState );
				vertex.pObject->IntersectRay( ri2, 2.0 * probeOffset, true, true, false );
				if( ri2.geometric.bHit )
				{
					if( ri2.pModifier ) {
						ri2.pModifier->Modify( ri2.geometric );
					}
					r.pos = ri2.geometric.ptIntersection;
					r.normal = ri2.geometric.vNormal;
					r.ok = true;
					return r;
				}
			}
		}

		return r;
	};

	// Central differences: probe at +eps and -eps in each tangent direction
	ProbeResult u_plus  = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_u * eps ) );
	ProbeResult u_minus = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_u * (-eps) ) );
	ProbeResult v_plus  = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_v * eps ) );
	ProbeResult v_minus = probeAt( Point3Ops::mkPoint3( vertex.position, tangent_v * (-eps) ) );


	if( u_plus.ok && u_minus.ok )
	{
		const Scalar inv2eps = 1.0 / (2.0 * eps);
		vertex.dpdu = Vector3(
			(u_plus.pos.x - u_minus.pos.x) * inv2eps,
			(u_plus.pos.y - u_minus.pos.y) * inv2eps,
			(u_plus.pos.z - u_minus.pos.z) * inv2eps );
		vertex.dndu = Vector3(
			(u_plus.normal.x - u_minus.normal.x) * inv2eps,
			(u_plus.normal.y - u_minus.normal.y) * inv2eps,
			(u_plus.normal.z - u_minus.normal.z) * inv2eps );
	}
	else if( u_plus.ok )
	{
		// Fall back to forward difference
		const Scalar invEps = 1.0 / eps;
		vertex.dpdu = Vector3(
			(u_plus.pos.x - vertex.position.x) * invEps,
			(u_plus.pos.y - vertex.position.y) * invEps,
			(u_plus.pos.z - vertex.position.z) * invEps );
		vertex.dndu = Vector3(
			(u_plus.normal.x - vertex.normal.x) * invEps,
			(u_plus.normal.y - vertex.normal.y) * invEps,
			(u_plus.normal.z - vertex.normal.z) * invEps );
	}
	else
	{
		vertex.dpdu = tangent_u;
		vertex.dndu = Vector3( 0, 0, 0 );
	}

	if( v_plus.ok && v_minus.ok )
	{
		const Scalar inv2eps = 1.0 / (2.0 * eps);
		vertex.dpdv = Vector3(
			(v_plus.pos.x - v_minus.pos.x) * inv2eps,
			(v_plus.pos.y - v_minus.pos.y) * inv2eps,
			(v_plus.pos.z - v_minus.pos.z) * inv2eps );
		vertex.dndv = Vector3(
			(v_plus.normal.x - v_minus.normal.x) * inv2eps,
			(v_plus.normal.y - v_minus.normal.y) * inv2eps,
			(v_plus.normal.z - v_minus.normal.z) * inv2eps );
	}
	else if( v_plus.ok )
	{
		const Scalar invEps = 1.0 / eps;
		vertex.dpdv = Vector3(
			(v_plus.pos.x - vertex.position.x) * invEps,
			(v_plus.pos.y - vertex.position.y) * invEps,
			(v_plus.pos.z - vertex.position.z) * invEps );
		vertex.dndv = Vector3(
			(v_plus.normal.x - vertex.normal.x) * invEps,
			(v_plus.normal.y - vertex.normal.y) * invEps,
			(v_plus.normal.z - vertex.normal.z) * invEps );
	}
	else
	{
		vertex.dpdv = tangent_v;
		vertex.dndv = Vector3( 0, 0, 0 );
	}

	// Project dpdu and dpdv into the tangent plane and orthogonalize.
	// Finite difference probes on curved surfaces can produce tangent
	// vectors with a normal component, which makes the Jacobian
	// inconsistent with the constraint's tangent-plane projection.
	{
		const Vector3& n = vertex.normal;

		// Project dpdu into tangent plane
		Scalar d_n = Vector3Ops::Dot( vertex.dpdu, n );
		vertex.dpdu = Vector3(
			vertex.dpdu.x - n.x * d_n,
			vertex.dpdu.y - n.y * d_n,
			vertex.dpdu.z - n.z * d_n );

		// Project dpdv into tangent plane
		d_n = Vector3Ops::Dot( vertex.dpdv, n );
		vertex.dpdv = Vector3(
			vertex.dpdv.x - n.x * d_n,
			vertex.dpdv.y - n.y * d_n,
			vertex.dpdv.z - n.z * d_n );

		// Gram-Schmidt: make dpdv orthogonal to dpdu
		const Scalar dpdu_sq = Vector3Ops::SquaredModulus( vertex.dpdu );
		if( dpdu_sq > NEARZERO )
		{
			const Scalar proj = Vector3Ops::Dot( vertex.dpdv, vertex.dpdu ) / dpdu_sq;
			vertex.dpdv = Vector3(
				vertex.dpdv.x - vertex.dpdu.x * proj,
				vertex.dpdv.y - vertex.dpdu.y * proj,
				vertex.dpdv.z - vertex.dpdu.z * proj );
		}

		// If dpdv collapsed, reconstruct from cross product
		if( Vector3Ops::SquaredModulus( vertex.dpdv ) < NEARZERO )
		{
			vertex.dpdv = Vector3Ops::Cross( n, vertex.dpdu );
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////
// NewtonSolve
//
//   Runs Newton iteration to solve C(x) = 0.
//   Modifies chain in-place.  Returns true if converged.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::NewtonSolve(
	std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd,
	Scalar smoothing
	) const
{
#if SMS_TRACE_DIAGNOSTIC
	// Per-failure-mode counters.  Each return-false (or accepted-soft) path
	// in this routine increments exactly one counter.  Periodic dump every
	// 200k Newton calls so the breakdown shows up in the render log.
	static std::atomic<int> g_newton_total{ 0 };
	static std::atomic<int> g_newton_empty{ 0 };
	static std::atomic<int> g_newton_singular{ 0 };
	static std::atomic<int> g_newton_no_progress{ 0 };
	static std::atomic<int> g_newton_update_failed{ 0 };
	static std::atomic<int> g_newton_iter_limit{ 0 };
	static std::atomic<int> g_newton_soft_converged{ 0 };
	static std::atomic<int> g_newton_strict_converged{ 0 };
	// D4 sub-counters: at the noProgress failure site, bucket by which
	// iteration the line search died on, and by the residual norm at that
	// point (relative to solverThreshold).  Discriminates H4a (Jacobian
	// wrong from iter 0) vs H4b (Newton made progress then hit a non-
	// smooth wall).
	static std::atomic<int> g_np_iter[5]{ {0}, {0}, {0}, {0}, {0} };
	// Buckets:  0=iter==0  1=iter==1  2=iter==2  3=iter in [3,5]  4=iter>=6
	static std::atomic<int> g_np_norm[5]{ {0}, {0}, {0}, {0}, {0} };
	// Buckets relative to solverThreshold (default 1e-4):
	//   0: norm < 10·thr  (would soft-converge after iter limit)
	//   1: norm < 100·thr
	//   2: norm < 1000·thr
	//   3: norm < 1
	//   4: norm >= 1
	const int nt = g_newton_total.fetch_add( 1, std::memory_order_relaxed );
	if( (nt & 0x3ffff) == 0 && nt > 0 ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_NEWTON_STATS: total=%d empty=%d singular=%d noProgress=%d updateFailed=%d iterLimit=%d softOk=%d strictOk=%d",
			nt, g_newton_empty.load(),
			g_newton_singular.load(), g_newton_no_progress.load(),
			g_newton_update_failed.load(), g_newton_iter_limit.load(),
			g_newton_soft_converged.load(), g_newton_strict_converged.load() );
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_NP_ITER:  iter0=%d  iter1=%d  iter2=%d  iter[3-5]=%d  iter>=6=%d",
			g_np_iter[0].load(), g_np_iter[1].load(), g_np_iter[2].load(),
			g_np_iter[3].load(), g_np_iter[4].load() );
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_NP_NORM:  norm<10thr=%d  <100thr=%d  <1000thr=%d  <1=%d  >=1=%d",
			g_np_norm[0].load(), g_np_norm[1].load(), g_np_norm[2].load(),
			g_np_norm[3].load(), g_np_norm[4].load() );
	}
#endif

	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
#if SMS_TRACE_DIAGNOSTIC
		g_newton_empty.fetch_add( 1, std::memory_order_relaxed );
#endif
		return false;
	}

	// Levenberg-Marquardt damping factor.  Persists across iterations of
	// THIS Solve call: shrunk on accepted line-search steps (toward pure
	// Newton, which converges quadratically near a root), grown on
	// rejected line-search steps (toward gradient descent, which can
	// escape plateaus where Newton's J⁻¹·C direction is unreliable).
	// Init 0 means the first iter is pure Newton — preserves baseline
	// behaviour on well-conditioned chains.  When
	// `config.useLevenbergMarquardt` is false, this stays 0 throughout
	// and every LM-related branch below is a no-op.
	Scalar lmLambda = 0;
#if SMS_SOLVE_DIAG
	bool lmDidEscalate = false;
#endif

	for( unsigned int iter = 0; iter < config.maxIterations; iter++ )
	{
#if SMS_SOLVE_DIAG
		if( config.useLevenbergMarquardt ) {
			g_solveDiag_lmTotalIters.fetch_add( 1, std::memory_order_relaxed );
			if( lmLambda > 0 ) {
				g_solveDiag_lmDamped.fetch_add( 1, std::memory_order_relaxed );
			}
		}
#endif
		// Evaluate constraint
		std::vector<Scalar> C;
		EvaluateConstraint( chain, fixedStart, fixedEnd, C );

		// Compute norm of constraint
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < 2 * k; i++ )
		{
			norm2 += C[i] * C[i];
		}
		const Scalar norm = sqrt( norm2 );

		if( norm < config.solverThreshold )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_newton_strict_converged.fetch_add( 1, std::memory_order_relaxed );
#endif
#if SMS_SOLVE_DIAG
			if( config.useLevenbergMarquardt && lmDidEscalate ) {
				g_solveDiag_lmRecovered.fetch_add( 1, std::memory_order_relaxed );
			}
#endif
			return true;  // Converged
		}

		// Build Jacobian for Newton step.  Include curvature terms
		// (dndu / dndv via Weingarten) — on analytical smooth surfaces
		// like spheres, they dominate the Jacobian and Newton diverges
		// without them.  Original rationale for excluding curvature
		// (tri-mesh per-triangle discontinuity) is handled by the
		// mesh Jacobian returning zero dndu/dndv in those cases.
		std::vector<Scalar> diag, upper_blocks, lower_blocks;
		BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks,
			/*includeCurvature=*/true );

		// Levenberg-Marquardt diagonal damping.  When `lmLambda > 0`,
		// add `lmLambda × mean(|J_ii|)` to each diagonal block's (0,0)
		// and (1,1) entries.  Scaling by the mean diagonal magnitude
		// keeps the damping order-of-magnitude appropriate regardless
		// of the chain length / IOR / surface scale at this vertex —
		// raw `lmLambda · I` would be dominated by the original J entries
		// at well-conditioned vertices and have no effect, while at
		// ill-conditioned vertices a large enough λ moves the step
		// direction toward gradient descent.  Skipped when LM is
		// disabled (`lmLambda` stays 0 throughout).
		if( config.useLevenbergMarquardt && lmLambda > 0 )
		{
			Scalar diagMagSum = 0;
			for( unsigned int i = 0; i < k; i++ ) {
				diagMagSum += std::fabs( diag[i*4]   );  // J(0,0) of vertex i
				diagMagSum += std::fabs( diag[i*4+3] );  // J(1,1) of vertex i
			}
			const Scalar diagMagMean = ( k > 0 ) ? diagMagSum / Scalar( 2 * k ) : Scalar( 1.0 );
			const Scalar dampAmount = lmLambda * diagMagMean;
			for( unsigned int i = 0; i < k; i++ ) {
				diag[i*4]   += dampAmount;
				diag[i*4+3] += dampAmount;
			}
		}

		// Solve for Newton step: J * delta = C  (we solve J * delta = C, then subtract)
		std::vector<Scalar> delta;
		if( !SolveBlockTridiagonal( diag, upper_blocks, lower_blocks, C, k, delta ) )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_newton_singular.fetch_add( 1, std::memory_order_relaxed );
#endif
			return false;  // Singular Jacobian
		}

		// World-space step-norm cap.  After computing the unconstrained
		// Newton step `delta` (in tangent-plane (du,dv) parameters per
		// vertex), convert each vertex's per-iter displacement to world
		// space via the local tangent basis, find the maximum across
		// vertices, and if it exceeds <cap_frac> × mean_segment_length,
		// rescale the entire delta vector by the same factor.  Preserves
		// step direction; only bounds magnitude.
		//
		// Why mean segment length is the right scale: on heavy-
		// displacement geometry, the Jacobian can become ill-conditioned
		// at a vertex where the local normal field has a near-singular
		// (Weingarten) curvature term.  The unconstrained Newton step
		// then pushes that vertex far off the local manifold; the
		// backtracking line search has to halve up to 10× to recover and
		// often gives up.  A cap proportional to local feature size keeps
		// every iter's per-vertex move conservative without sacrificing
		// correctness — when the Jacobian IS well-conditioned and the
		// step is small relative to feature size, the cap is a no-op.
		if( kNewtonStepNormCapFrac > 0 )
		{
			// Mean segment length covers the vertex chain plus the
			// shading-point and emitter-point bookend segments.
			Scalar segSum = 0;
			Scalar segCount = 0;
			for( unsigned int i = 0; i < k; i++ ) {
				const Point3 prev = ( i == 0 ) ? fixedStart : chain[i-1].position;
				segSum += Point3Ops::Distance( prev, chain[i].position );
				segCount += 1;
			}
			if( k > 0 ) {
				segSum += Point3Ops::Distance( chain[k-1].position, fixedEnd );
				segCount += 1;
			}
			const Scalar meanSeg = ( segCount > 0 ) ? segSum / segCount : Scalar(1.0);
			const Scalar capWorld = kNewtonStepNormCapFrac * meanSeg;

			// Find max per-vertex world-space step magnitude.
			Scalar maxStepWorld = 0;
			for( unsigned int i = 0; i < k; i++ ) {
				const Vector3 dWorld =
					chain[i].dpdu * delta[2*i] +
					chain[i].dpdv * delta[2*i+1];
				const Scalar mag = Vector3Ops::Magnitude( dWorld );
				if( mag > maxStepWorld ) maxStepWorld = mag;
			}

			if( maxStepWorld > capWorld && maxStepWorld > NEARZERO ) {
				const Scalar scale = capWorld / maxStepWorld;
				for( unsigned int i = 0; i < 2 * k; i++ ) {
					delta[i] *= scale;
				}
#if SMS_SOLVE_DIAG
				g_solveDiag_capFired.fetch_add( 1, std::memory_order_relaxed );
				const uint64_t fixedRatio = static_cast<uint64_t>(
					( maxStepWorld / std::max( capWorld, Scalar(1e-12) ) ) * 1000.0 );
				g_solveDiag_capRatioSum.fetch_add( fixedRatio, std::memory_order_relaxed );
#endif
			}
#if SMS_SOLVE_DIAG
			g_solveDiag_capChecks.fetch_add( 1, std::memory_order_relaxed );
#endif
		}

		// Apply update with line search: try decreasing step sizes
		// until the constraint norm actually decreases.
		Scalar beta = 1.0;
		bool allValid = false;
		bool improved = false;

		// Save chain state
		std::vector<ManifoldVertex> savedChain( chain );

		for( unsigned int attempt = 0; attempt < 10; attempt++ )
		{
			// Restore chain from saved state
			chain = savedChain;

			// Apply step: x_new = x - beta * delta
			allValid = true;
			for( unsigned int i = 0; i < k; i++ )
			{
				const Scalar du = -beta * delta[2*i];
				const Scalar dv = -beta * delta[2*i+1];

				if( !UpdateVertexOnSurface( chain[i], du, dv, smoothing ) )
				{
					allValid = false;
					break;
				}

				if( !chain[i].valid )
				{
					allValid = false;
					break;
				}
			}

#if SMS_EDGE_AWARE_NEWTON
			// Linearization-trust check (edge-aware Newton step).  After
			// the step is applied and the chain re-projected to the
			// surface, compare the actual normal rotation at each vertex
			// to what the saved Jacobian predicted.  When the ratio
			// exceeds `kEdgeTrustRelRatio` (and both magnitudes are
			// above the absolute floor), the linearization wasn't
			// trustworthy for this step — the typical cause on a
			// triangle mesh is the new vertex landing in a different
			// triangle whose Phong-interpolation slope differs from
			// the source triangle's.  Reject and let line search halve.
			//
			// On smooth analytical surfaces with continuous derivatives,
			// predicted ≈ actual within higher-order terms and the test
			// rarely fires (verified on sms_k1_refract / k2_glassblock /
			// k2_glasssphere regressions: <0.1% reject rate).
			if( allValid )
			{
#if SMS_SOLVE_DIAG
				g_solveDiag_edgeChecks.fetch_add( 1, std::memory_order_relaxed );
#endif
				bool linearizationTrusted = true;
				for( unsigned int i = 0; i < k; i++ )
				{
					const Scalar du = -beta * delta[2*i];
					const Scalar dv = -beta * delta[2*i+1];
					// Predicted normal change from the savedChain Jacobian.
					const Vector3 predicted_dn(
						savedChain[i].dndu.x * du + savedChain[i].dndv.x * dv,
						savedChain[i].dndu.y * du + savedChain[i].dndv.y * dv,
						savedChain[i].dndu.z * du + savedChain[i].dndv.z * dv );
					const Scalar predicted_mag = Vector3Ops::Magnitude( predicted_dn );
					// Actual change: chain[i].normal − savedChain[i].normal.
					const Vector3 actual_dn(
						chain[i].normal.x - savedChain[i].normal.x,
						chain[i].normal.y - savedChain[i].normal.y,
						chain[i].normal.z - savedChain[i].normal.z );
					const Scalar actual_mag = Vector3Ops::Magnitude( actual_dn );

					if( actual_mag > kEdgeTrustAbsFloor &&
						actual_mag > kEdgeTrustRelRatio *
							std::max( predicted_mag, kEdgeTrustAbsFloor ) )
					{
						linearizationTrusted = false;
						break;
					}
				}
				if( !linearizationTrusted )
				{
#if SMS_SOLVE_DIAG
					g_solveDiag_edgeRejects.fetch_add( 1, std::memory_order_relaxed );
#endif
					beta *= 0.5;
					continue;
				}
			}
#endif

			if( allValid )
			{
				// Check if the norm actually decreased
				std::vector<Scalar> C_test;
				EvaluateConstraint( chain, fixedStart, fixedEnd, C_test );
				Scalar testNorm2 = 0.0;
				for( unsigned int i = 0; i < 2*k; i++ )
				{
					testNorm2 += C_test[i] * C_test[i];
				}
				if( sqrt(testNorm2) < norm )
				{
					improved = true;
					break;
				}
			}

			beta *= 0.5;
		}

		if( !improved )
		{
			// Even the smallest step didn't improve — but if Newton already
			// brought ||C|| within the soft-convergence band (10× the strict
			// threshold), accept the chain rather than discarding it.  On
			// triangle meshes whose chord-vs-arc tessellation error sets a
			// floor on the achievable residual, Newton can iterate to this
			// floor and then stall; without this acceptance, those chains
			// (≈1% of total Newton calls on the displaced Veach-egg scene
			// per SMS_NP_NORM diagnostic) get silently discarded even
			// though their constraint is satisfied to within visual
			// precision.  Mirrors the existing post-iter-limit soft-converge
			// check at the bottom of NewtonSolve — same threshold, applied
			// at the earlier exit too.
			chain = savedChain;
			if( norm < config.solverThreshold * 10.0 )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_newton_soft_converged.fetch_add( 1, std::memory_order_relaxed );
#endif
				return true;
			}

			if( config.useLevenbergMarquardt )
			{
				// Levenberg-Marquardt escalation: line search couldn't find
				// a β that decreases ||C|| — Newton's J⁻¹·C step direction
				// is itself unreliable.  Increase damping and retry the
				// same iter slot with a more gradient-descent-like solve.
				// When damping reaches `kLM_LambdaMax` without progress,
				// fall through to the legacy fail path — at that point
				// the problem is genuinely beyond LM's reach (multiple
				// basins with no descent connection, etc.).
				lmLambda = ( lmLambda <= 0 )
					? kLM_LambdaInit
					: std::min( lmLambda * kLM_LambdaUp, kLM_LambdaMax );
#if SMS_SOLVE_DIAG
				g_solveDiag_lmEscalated.fetch_add( 1, std::memory_order_relaxed );
				lmDidEscalate = true;
#endif
				if( lmLambda < kLM_LambdaMax ) {
					continue;  // retry next iter with more damping
				}
			}

#if SMS_TRACE_DIAGNOSTIC
			g_newton_no_progress.fetch_add( 1, std::memory_order_relaxed );
			// Bucket by iteration index where line search died
			{
				int b = 0;
				if( iter == 0 )      b = 0;
				else if( iter == 1 ) b = 1;
				else if( iter == 2 ) b = 2;
				else if( iter <= 5 ) b = 3;
				else                 b = 4;
				g_np_iter[b].fetch_add( 1, std::memory_order_relaxed );
			}
			// Bucket by current iteration's residual norm (in units of
			// solverThreshold), so we can see if the failure happened
			// near-converged or far from any solution.
			{
				const Scalar thr = config.solverThreshold;
				int b = 0;
				if( norm < 10.0   * thr ) b = 0;
				else if( norm < 100.0  * thr ) b = 1;
				else if( norm < 1000.0 * thr ) b = 2;
				else if( norm < 1.0          ) b = 3;
				else                            b = 4;
				g_np_norm[b].fetch_add( 1, std::memory_order_relaxed );
			}
#endif
			return false;
		}

		// Step accepted.  Shrink LM damping toward 0 (pure Newton) so the
		// next iter can take a quadratically-converging Newton step when
		// the chain has moved into a well-conditioned region.  No-op when
		// LM is disabled (lmLambda stays 0).
		if( config.useLevenbergMarquardt && lmLambda > 0 )
		{
			lmLambda *= kLM_LambdaDown;
			if( lmLambda < kLM_LambdaMin ) lmLambda = 0;
		}

		if( !allValid )
		{
			// Restore and report failure
			chain = savedChain;
#if SMS_TRACE_DIAGNOSTIC
			g_newton_update_failed.fetch_add( 1, std::memory_order_relaxed );
#endif
			return false;
		}
	}

	// Did not converge within maxIterations.  Check if the constraint
	// norm is close enough to accept as a soft convergence.  On meshes
	// with discontinuous normals (triangle edges), Newton may oscillate
	// near the solution without reaching the strict threshold.  A
	// relaxed threshold of 10× catches these near-solutions.
	{
		std::vector<Scalar> C_final;
		EvaluateConstraint( chain, fixedStart, fixedEnd, C_final );
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < 2 * k; i++ )
			norm2 += C_final[i] * C_final[i];
		if( sqrt(norm2) < config.solverThreshold * 10.0 )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_newton_soft_converged.fetch_add( 1, std::memory_order_relaxed );
#endif
			return true;  // Soft convergence
		}
	}

#if SMS_TRACE_DIAGNOSTIC
	g_newton_iter_limit.fetch_add( 1, std::memory_order_relaxed );
#endif
	return false;
}

//////////////////////////////////////////////////////////////////////
// SMSLoopSampler — sampler-dimension-drift firewall
//
//   Wraps a fresh RandomNumberGenerator + IndependentSampler in one
//   stack-scoped object.  Construct from the parent sampler at the
//   top of each `EvaluateAtShadingPoint*` entry; pass `.sampler` into
//   variable-count internal work (M-trial loop, Bernoulli K-loop,
//   `EstimatePDF`, `Solve`).
//
//   The parent sampler advances by exactly TWO 1-D dimensions
//   (regardless of how many internal trials run) when constructing
//   this scope, so an LDS sampler (Sobol etc.) keeps a predictable
//   dimension stream — `EvaluateAtShadingPoint*` no longer pollutes
//   downstream call sites.  The internal RNG is seeded from those
//   two dimensions via a multiplicative hash, so each pixel sample
//   gets a different RNG state and the variable-count work stays
//   i.i.d.-uniform (matches Mitsuba's purely-RNG SMS implementation).
//
//   Member-init order is intentional: `rng` declared first so its
//   constructor runs before `sampler`'s reference is bound.
//////////////////////////////////////////////////////////////////////

namespace {
	struct SMSLoopSampler {
		RandomNumberGenerator rng;
		IndependentSampler    sampler;

		explicit SMSLoopSampler( ISampler& parent )
			: rng( deriveSeed( parent ) ), sampler( rng ) {}

	private:
		static unsigned int deriveSeed( ISampler& parent ) {
			const Scalar s0 = parent.Get1D();
			const Scalar s1 = parent.Get1D();
			const unsigned int a = static_cast<unsigned int>( s0 * 4294967295.0 );
			const unsigned int b = static_cast<unsigned int>( s1 * 4294967295.0 );
			// Golden-ratio multiplicative hash combine — decorrelates
			// the two parent draws so adjacent LDS dimension pairs
			// produce well-spread RNG seeds.
			return a ^ ( b * 2654435761u );
		}
	};

	// Fisher-Yates partial shuffle that retains the first `cap` elements
	// of `seeds` as a uniform random subset.  Used to bound the per-
	// shading-point Newton-solve cost when QuerySeeds returns a dense
	// neighbour set (a focused caustic can return hundreds of photons
	// from a 1M-photon kd-tree).  `cap == 0` disables the cap.
	//
	// O(cap) work; the rest of the vector is left untouched at the tail
	// and resize() trims it.  Caller-provided sampler keeps the LDS
	// dimensions consistent with the rest of the SMS trial loop.
	inline void RandomSubsamplePhotonSeeds(
		std::vector<SMSPhoton>& seeds,
		unsigned int cap,
		ISampler& sampler )
	{
		if( cap == 0 || seeds.size() <= cap ) return;
		for( unsigned int i = 0; i < cap; i++ ) {
			const unsigned int range = static_cast<unsigned int>( seeds.size() - i );
			const unsigned int j = i + static_cast<unsigned int>(
				sampler.Get1D() * range );
			const unsigned int jj = ( j >= seeds.size() ) ? seeds.size() - 1 : j;
			if( jj != i ) std::swap( seeds[i], seeds[jj] );
		}
		seeds.resize( cap );
	}
}

//////////////////////////////////////////////////////////////////////
// EnumerateSpecularCasters (static)
//
//   Walk the scene's object manager once and collect every object
//   whose material reports `isSpecular`.  Used to build the cached
//   list consumed by uniform-on-shape SMS seeding (Mitsuba-faithful
//   single-/multi-scatter).  Each object is sampled at its
//   (0.5, 0.5, 0.5) parametric centre via UniformRandomPoint to get
//   a valid RayIntersectionGeometric for the GetSpecularInfo query
//   — this matters for materials whose painters are textured
//   functions of (u, v).
//////////////////////////////////////////////////////////////////////

namespace {
	class SpecularCasterCollector : public IEnumCallback<IObject>
	{
	public:
		std::vector<const IObject*>& out;
		explicit SpecularCasterCollector( std::vector<const IObject*>& o ) : out( o ) {}

		bool operator()( const IObject& obj ) override
		{
			const IMaterial* pMat = obj.GetMaterial();
			if( !pMat ) {
				return true;   // continue enumeration
			}

			// Probe at several deterministic prands.  A `SwitchPel`-style
			// painter that keys off (u, v) and reports `isSpecular` only
			// in some patches must still classify the object as a caster
			// — single-point probes at (0.5, 0.5, 0.5) misclassify those.
			static const Point3 kProbes[] = {
				Point3( 0.5, 0.5, 0.5 ),
				Point3( 0.25, 0.5, 0.75 ),
				Point3( 0.75, 0.25, 0.25 )
			};

			for( const Point3& prand : kProbes )
			{
				Point3 p;
				Vector3 n;
				Point2 uv;
				obj.UniformRandomPoint( &p, &n, &uv, prand );

				Ray dummyRay( p, n );
				RayIntersectionGeometric rig( dummyRay, nullRasterizerState );
				rig.bHit          = true;
				rig.ptIntersection = p;
				rig.vNormal       = n;
				rig.ptCoord       = uv;

				IORStack iorStack( 1.0 );
				SpecularInfo specInfo = pMat->GetSpecularInfo( rig, iorStack );

				if( specInfo.isSpecular ) {
					out.push_back( &obj );
					break;   // any single-probe positive accepts; don't double-add
				}
			}
			return true;   // continue enumeration
		}
	};
}

void ManifoldSolver::EnumerateSpecularCasters(
	const IScene& scene,
	std::vector<const IObject*>& out
	)
{
	const IObjectManager* pObjMgr = scene.GetObjects();
	if( !pObjMgr ) {
		return;
	}

	SpecularCasterCollector collector( out );
	pObjMgr->EnumerateObjects( collector );
}

//////////////////////////////////////////////////////////////////////
// BuildSeedChain
//
//   Traces a ray from start toward end, following refraction at each
//   specular surface to build a seed chain that naturally discovers
//   multi-object specular paths.  At each glass surface, the ray is
//   refracted using Snell's law, allowing it to follow the physical
//   light path through interlocking glass objects rather than only
//   finding objects along the straight line.
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::BuildSeedChain(
	const Point3& start,
	const Point3& end,
	const IScene& scene,
	const IRayCaster& caster,
	std::vector<ManifoldVertex>& chain
	) const
{
	chain.clear();

	Vector3 dir = Vector3Ops::mkVector3( end, start );
	const Scalar totalDist = Vector3Ops::NormalizeMag( dir );

#if defined(SMS_TRACE_DIAGNOSTIC) && SMS_TRACE_DIAGNOSTIC
	static std::atomic<int> g_bsc{ 0 };
	const bool traceBSC =
		( std::fabs( start.x ) < 0.02 ) &&
		( std::fabs( start.z ) < 0.02 ) &&
		( start.y >= -0.02 && start.y <= 0.02 ) &&
		( g_bsc.fetch_add( 1, std::memory_order_relaxed ) < 5 );
	if( traceBSC ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_BSC: start=(%.4f,%.4f,%.4f) end=(%.4f,%.4f,%.4f) dir=(%.4f,%.4f,%.4f) totalDist=%.4f",
			start.x, start.y, start.z, end.x, end.y, end.z,
			dir.x, dir.y, dir.z, totalDist );
	}
#endif

	if( totalDist < NEARZERO )
	{
		return 0;
	}

	// Initial seed: the ray walks from the shading point toward the light
	// sample.  Medium starts as air (IOR=1.0).  Snell-continue handles the
	// per-vertex push/pop, vertex creation, and ray refraction.
	Point3 currentOrigin = start;
	Scalar currentIOR = 1.0;
	IORStack seedIor( 1.0 );

	return SnellContinueChain(
		currentOrigin, dir, totalDist,
		currentIOR, seedIor,
		scene, caster, chain );
}

//////////////////////////////////////////////////////////////////////
// SnellContinueChain
//
//   Continues the seed-chain construction from a starting state
//   (currentOrigin, dir, currentIOR, seedIor) by ray-tracing forward
//   and Snell-refracting / mirror-reflecting at every specular hit.
//   Stops at the first non-specular hit, no-more-intersection, or
//   safety-distance cutoff.  See the header doc-comment for full
//   semantics.
//
//   Extracted from BuildSeedChain in Phase 3 of the Mitsuba-faithful
//   SMS port (docs/SMS_UNIFORM_SEEDING_PLAN.md): used by both the
//   legacy Snell-trace seed entry AND the uniform-on-shape Mitsuba-
//   faithful seed entry (where the first vertex is sampled and we
//   need to extend the chain through any subsequent specular hits).
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::SnellContinueChain(
	Point3& currentOrigin,
	Vector3& dir,
	Scalar maxDist,
	Scalar& currentIOR,
	IORStack& seedIor,
	const IScene& scene,
	const IRayCaster& caster,
	std::vector<ManifoldVertex>& chain
	) const
{
	(void)caster;   // reserved for future visibility queries

	const IObjectManager* pObjMgr = scene.GetObjects();
	if( !pObjMgr ) {
		return 0;
	}

	const Scalar offsetEps = 1e-2;
	const std::size_t startSize = chain.size();

#if defined(SMS_TRACE_DIAGNOSTIC) && SMS_TRACE_DIAGNOSTIC
	// Trace gate: snapshot the entry origin once, mirror BuildSeedChain's
	// "shading-point near (0,0,0)" heuristic from before the refactor.
	const Point3 traceOrigin = currentOrigin;
	static std::atomic<int> g_scc{ 0 };
	const bool traceBSC =
		( std::fabs( traceOrigin.x ) < 0.02 ) &&
		( std::fabs( traceOrigin.z ) < 0.02 ) &&
		( traceOrigin.y >= -0.02 && traceOrigin.y <= 0.02 ) &&
		( g_scc.fetch_add( 1, std::memory_order_relaxed ) < 5 );
#endif

	// Medium tracking semantics: see BuildSeedChain doc-comment.  This
	// function mutates the IOR stack in place.

	for( unsigned int depth = 0; depth < config.maxChainDepth; depth++ )
	{
		// Offset origin along ray direction to avoid
		// re-intersecting the surface we just left
		Point3 offsetOrigin = Point3Ops::mkPoint3(
			currentOrigin, dir * offsetEps );

		Ray ray( offsetOrigin, dir );
		RayIntersection ri( ray, nullRasterizerState );

		// Scene-wide intersection via the acceleration structure
		pObjMgr->IntersectRay( ri, true, true, false );

		// Apply intersection modifier (e.g. bump map / normal map) so the
		// SMS chain's normals match what the rendering pipeline (RayCaster)
		// sees.  Without this, BuildSeedChain operates on the unperturbed
		// surface while PT operates on the perturbed surface, and SMS's
		// chain estimate won't align with PT's emission-suppression
		// (dimming, energy ratio collapse).  RayCaster::CastRay applies
		// the modifier at every hit (RayCaster.cpp:728-730); SMS bypasses
		// the RayCaster, so we re-apply here.
		if( ri.geometric.bHit && ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

#if defined(SMS_TRACE_DIAGNOSTIC) && SMS_TRACE_DIAGNOSTIC
		if( traceBSC ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_BSC:  depth=%u origin=(%.4f,%.4f,%.4f) dir=(%.4f,%.4f,%.4f) hit=%d range=%.4f  hitPos=(%.4f,%.4f,%.4f) obj=%p",
				depth, offsetOrigin.x, offsetOrigin.y, offsetOrigin.z,
				dir.x, dir.y, dir.z,
				int( ri.geometric.bHit ), ri.geometric.range,
				ri.geometric.ptIntersection.x, ri.geometric.ptIntersection.y, ri.geometric.ptIntersection.z,
				(void*)ri.pObject );
		}
#endif

		if( !ri.geometric.bHit )
		{
			break;
		}

		// Self-intersection guard: if we hit the same object extremely
		// close to where we just were, we are re-hitting the surface
		// we just left.  The threshold must be small enough to NOT
		// filter out the legitimate exit intersection from the far
		// side of the same object.
		if( ri.geometric.range < offsetEps * 2.0 && !chain.empty() &&
			ri.pObject == chain.back().pObject )
		{
			currentOrigin = Point3Ops::mkPoint3(
				ri.geometric.ptIntersection, dir * offsetEps );
			continue;
		}

		// Safety: don't trace forever
		if( ri.geometric.range > maxDist * 3.0 )
		{
			break;
		}

		// Check if the hit material is specular
		const IMaterial* pMat = ri.pMaterial;
		if( !pMat )
		{
			break;
		}

		SpecularInfo specInfo = pMat->GetSpecularInfo( ri.geometric, seedIor );

		if( !specInfo.isSpecular )
		{
			// Hit a non-specular surface; stop tracing
			break;
		}

		// Create ManifoldVertex from this intersection.  mv.isReflection is
		// PROVISIONALLY set to !canRefract (true only for mirror-only
		// materials) and then promoted to true below if the Snell test for
		// a refractive material produces total internal reflection — the
		// geometric direction we follow then IS a reflection, and if we
		// left the flag as refraction Newton would solve the wrong
		// half-vector constraint (or reject the chain), and throughput
		// would use the wrong eta pair.
		ManifoldVertex mv;
		mv.position = ri.geometric.ptIntersection;
		mv.normal = ri.geometric.vNormal;
		// Surface parameters at the hit.  Without this, vertex.uv stays at
		// its default (0, 0) — which is a parametric pole on most closed
		// surfaces.  Future SMS variants that key derivative computation
		// off (u, v) (analytical-derivative paths, photon-aided reseeding,
		// etc.) would silently see a degenerate parameterisation.  Cheap
		// and always-on; harmless for the FD-probe path which doesn't read
		// vertex.uv anyway.
		mv.uv = ri.geometric.ptCoord;
		mv.pObject = ri.pObject;
		mv.pMaterial = pMat;
		mv.eta = specInfo.ior;
		mv.attenuation = specInfo.attenuation;
		mv.isReflection = !specInfo.canRefract;
		mv.canRefract = specInfo.canRefract;
		mv.valid = false;  // Derivatives not yet computed; Solve will handle it

		// Determine entering vs exiting:
		//   - If we've already pushed THIS IObject* onto the stack during
		//     a previous crossing, the ray MUST be exiting (we're currently
		//     inside this specific mesh).  Catches the thin-double-sided-
		//     mesh case where both crossings have the normal pointing the
		//     same way, so a raw cosI-sign test misses it.
		//   - Else fall back to sign(dot(dir, normal)) < 0 ⇒ entering.
		//     This is the correct test for closed volumes (sphere) AND
		//     multi-object slabs-from-planes (each plane is a distinct
		//     IObject* with a distinct outward normal direction).
		seedIor.SetCurrentObject( ri.pObject );
		const bool sameObjectAgain = seedIor.containsCurrent();
		const Scalar cosI = Vector3Ops::Dot( dir, mv.normal );
		const bool bEntering = sameObjectAgain ? false : (cosI < 0);
		mv.isExiting = !bEntering;

		// Populate (etaI, etaT) — Walter et al. 2007 η_i / η_t for the
		// half-vector / Snell / Fresnel math downstream.  See the field
		// docs in ManifoldSolver.h for the rationale; in short, the old
		// `eta_eff = isExiting ? 1/eta : eta` formula assumed the OTHER
		// side of every interface was air (IOR=1.0), which is wrong
		// for nested dielectrics like the Veach Egg's air-cavity inside
		// glass (where the inner sphere's "other side" is glass at
		// IOR=1.5, not air).
		//
		// For ENTERING: the ray was in `currentIOR` (the surrounding
		// medium); after crossing this interface it'll be in
		// `specInfo.ior` (the object material).  η_i = surrounding,
		// η_t = object.
		//
		// For EXITING: the ray was inside the object (etaI = surface
		// material's IOR); after crossing it'll be back in whatever was
		// on the IOR stack BEFORE we entered this object.  We pop the
		// stack BELOW (in the canRefract / bEntering=false branch) and
		// READ THE NEW TOP — that's the post-pop surrounding medium.
		// We update mv.etaT after the pop using the stack's `top()`,
		// not the pre-existing hardcoded `currentIOR = 1.0` (which
		// silently assumed every exit lands in air).  For the egg
		// air-cavity exit back into the glass shell, this restores
		// etaT = 1.5 instead of 1.0.
		if( bEntering ) {
			mv.etaI = currentIOR;
			mv.etaT = specInfo.ior;
		} else {
			mv.etaI = specInfo.ior;
			mv.etaT = currentIOR;	// provisional; corrected after the pop below
		}

		const std::size_t idxJustPushed = chain.size();
		chain.push_back( mv );

		// Follow refraction/reflection to determine the next ray direction.
		if( specInfo.canRefract )
		{
			Vector3 n = mv.normal;
			Scalar etaRatio;
			if( bEntering ) {
				etaRatio = currentIOR / specInfo.ior;
				// normal already points outward relative to entering ray
			} else {
				etaRatio = specInfo.ior / currentIOR;
				// For exiting, flip the normal to face the incoming ray
				// (required by the Snell formula below).
				if( Vector3Ops::Dot( dir, n ) > 0 ) {
					n = n * (-1.0);
				}
			}
			// For entering case too, ensure normal opposes dir (may need flip
			// for the thin-sheet "same object crossed again with same-side normal"
			// scenario that we coerced to exiting, plus any other grazing cases).
			if( Vector3Ops::Dot( dir, n ) > 0 ) {
				n = n * (-1.0);
			}

			// Compute refracted direction using Snell's law
			const Scalar cosI2 = -Vector3Ops::Dot( dir, n );
			const Scalar sin2T = etaRatio * etaRatio * (1.0 - cosI2 * cosI2);

			if( sin2T <= 1.0 )
			{
				// Refraction succeeds
				const Scalar cosT = sqrt( 1.0 - sin2T );
				dir = dir * etaRatio + n * (etaRatio * cosI2 - cosT);
				dir = Vector3Ops::Normalize( dir );

				// Update current medium IOR and push/pop the object stack.
				if( bEntering ) {
					currentIOR = specInfo.ior;
					seedIor.SetCurrentObject( ri.pObject );
					seedIor.push( specInfo.ior );
				} else {
					if( sameObjectAgain ) {
						// Only pop if we pushed earlier.  The legacy
						// slabs-from-planes pattern uses cosI-based
						// exiting without a matching stack entry.
						seedIor.SetCurrentObject( ri.pObject );
						seedIor.pop();
						// After pop, the stack's top() is the IOR of
						// the medium we just re-entered.  For nested
						// dielectrics (e.g. air-cavity inside glass)
						// this is NOT 1.0; for a single dielectric in
						// air it IS 1.0 (the environment IOR pushed
						// by the IORStack constructor).
						currentIOR = seedIor.top();
					} else {
						// No matching push — legacy slabs-from-planes
						// pattern.  Fall back to the old hardcoded
						// "back to air" behaviour for this case so we
						// don't break tests that exercise it.
						currentIOR = 1.0;
					}
					// Backfill the just-pushed vertex's etaT with the
					// post-pop surrounding-medium IOR.  Provisional
					// value (set above to currentIOR) used the OLD
					// currentIOR = inside-object IOR, which is wrong
					// for the half-vector math.
					chain[ idxJustPushed ].etaT = currentIOR;
				}
			}
			else
			{
				// Total internal reflection on a refractive material.  The
				// geometric direction we follow from here on IS a reflection,
				// so promote the just-pushed manifold vertex from its
				// provisional `isReflection=false` (refractive material) to
				// `isReflection=true` — otherwise Newton would solve the
				// refractive half-vector constraint at this vertex and
				// either fail or converge to a physically-wrong chain, and
				// EvaluateChainThroughput would apply the refraction (1-Fr)
				// factor instead of the reflection Fr factor.
				//
				// On TIR, etaI/etaT semantics for "reflection at this
				// interface" are still meaningful for the Fresnel call
				// below — they describe the INTERFACE the ray reflects
				// off, even though the ray doesn't transmit.  Leave
				// etaI/etaT as set above (per entering/exiting); the
				// reflection branch of EvaluateConstraint and
				// BuildJacobian doesn't read them anyway (it uses
				// h = wi + wo with no IOR weighting).
				chain[ idxJustPushed ].isReflection = true;
				dir = dir + n * (2.0 * cosI2);
				dir = Vector3Ops::Normalize( dir );
			}
		}
		else
		{
			// Pure reflection (mirror) — no medium transition.
			const Scalar cosI_refl = -Vector3Ops::Dot( dir, mv.normal );
			dir = dir + mv.normal * (2.0 * cosI_refl);
			dir = Vector3Ops::Normalize( dir );
		}

		// Move origin past the surface along the refracted/reflected
		// direction.  We use just the new direction with the offset
		// applied at the top of the loop.
		currentOrigin = ri.geometric.ptIntersection;
	}

	return static_cast<unsigned int>( chain.size() - startSize );
}

//////////////////////////////////////////////////////////////////////
// BuildSeedChainBranching
//
//   Branching seed-chain builder.  At each sub-critical dielectric
//   vertex, decides between SPLITTING the chain into both Fresnel-
//   reflection and refraction continuations (when running throughput
//   exceeds `config.branchingThreshold`) or Russian-roulette picking
//   one branch weighted by Fr (otherwise).  See header doc-comment
//   for the math and the caller's `contribution / proposalPdf` step.
//
//   Implemented iteratively with a stack of partial-chain "frames"
//   to bound the worst-case fan-out at 2^k (k = chain length) and
//   avoid recursive call cost.  Frames are moved through the stack;
//   the reflection branch copies its frame's chain because both
//   branches need an unmodified prefix, the refraction branch is the
//   moved tail.
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::BuildSeedChainBranching(
	const Point3& start,
	const Point3& end,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	std::vector<SeedChainResult>& out
	) const
{
	(void)caster;
	out.clear();

	Vector3 initialDir = Vector3Ops::mkVector3( end, start );
	const Scalar totalDist = Vector3Ops::NormalizeMag( initialDir );
	if( totalDist < NEARZERO ) return 0;

	const IObjectManager* pObjMgr = scene.GetObjects();
	if( !pObjMgr ) return 0;

	// Per-frame state: a partial chain mid-construction.  Stack-
	// based traversal so we can branch by pushing one extra frame.
	//
	// Throughput-gated branching — every dielectric vertex with
	// running throughput > `config.branchingThreshold` produces TWO
	// continuation chains (reflect + refract).  Once throughput
	// decimates below the gate, subsequent Fresnel-decision vertices
	// fall back to Russian-roulette pick weighted by Fr (recorded in
	// `proposalPdf`).  Total chain count is bounded at 2^k (k = chain
	// length) but in practice self-limits because each reflection
	// branch's throughput drops by factor Fr ≈ 0.04 — those branches
	// are immediately below threshold and don't re-split.
	//
	// Note: this is more aggressive than PT's `!splitFired` semantic
	// (which splits once per camera ray).  SMS needs the fuller
	// branching because each chain represents a distinct caustic
	// path that contributes its own energy — refract-refract,
	// refract-reflect, and reflect-only at v0 are all physically
	// separate paths through nested dielectrics, each with its own
	// caustic.  Single-split would miss the refract-reflect class.
	struct Frame {
		std::vector<ManifoldVertex> chain;
		Point3                     currentOrigin;
		Vector3                    dir;
		Scalar                     currentIOR;
		IORStack                   seedIor;
		Scalar                     throughput;   // running max(Fresnel-factor) product
		Scalar                     proposalPdf;  // running RR-pick pdf product

		Frame()
			: currentOrigin( 0, 0, 0 )
			, dir( 0, 0, 0 )
			, currentIOR( 1.0 )
			, seedIor( 1.0 )
			, throughput( 1.0 )
			, proposalPdf( 1.0 )
		{}
	};

	const Scalar offsetEps   = 1e-2;
	const Scalar bThreshold  = config.branchingThreshold;
	const unsigned int maxDepth = config.maxChainDepth;
	const unsigned int maxFrames = 1u << 8;   // safety cap (2^k can blow up; bound it)
	unsigned int framesPopped = 0;

	std::vector<Frame> active;
	active.reserve( 8 );
	{
		Frame f0;
		f0.currentOrigin = start;
		f0.dir           = initialDir;
		active.push_back( std::move( f0 ) );
	}

	while( !active.empty() )
	{
		Frame f = std::move( active.back() );
		active.pop_back();

		// Outer safety: cap total work per call.  Cheaper than letting
		// pathological scenes (e.g. many overlapping dielectric layers)
		// produce 2^k frames.
		if( ++framesPopped > maxFrames ) {
			out.push_back( SeedChainResult{ std::move( f.chain ), f.proposalPdf } );
			continue;
		}

		// Depth cap — emit chain as terminal.
		if( f.chain.size() >= maxDepth ) {
			out.push_back( SeedChainResult{ std::move( f.chain ), f.proposalPdf } );
			continue;
		}

		// Single ray-cast step.
		Point3 offsetOrigin = Point3Ops::mkPoint3(
			f.currentOrigin, f.dir * offsetEps );
		Ray ray( offsetOrigin, f.dir );
		RayIntersection ri( ray, nullRasterizerState );
		pObjMgr->IntersectRay( ri, true, true, false );
		if( ri.geometric.bHit && ri.pModifier ) {
			ri.pModifier->Modify( ri.geometric );
		}

		if( !ri.geometric.bHit ) {
			out.push_back( SeedChainResult{ std::move( f.chain ), f.proposalPdf } );
			continue;
		}

		// Self-intersection guard — re-step from the just-hit surface.
		if( ri.geometric.range < offsetEps * 2.0 && !f.chain.empty() &&
			ri.pObject == f.chain.back().pObject )
		{
			f.currentOrigin = Point3Ops::mkPoint3(
				ri.geometric.ptIntersection, f.dir * offsetEps );
			active.push_back( std::move( f ) );
			continue;
		}

		// Safety: don't trace forever.
		if( ri.geometric.range > totalDist * 3.0 ) {
			out.push_back( SeedChainResult{ std::move( f.chain ), f.proposalPdf } );
			continue;
		}

		const IMaterial* pMat = ri.pMaterial;
		if( !pMat ) {
			out.push_back( SeedChainResult{ std::move( f.chain ), f.proposalPdf } );
			continue;
		}

		SpecularInfo specInfo = pMat->GetSpecularInfo( ri.geometric, f.seedIor );
		if( !specInfo.isSpecular ) {
			out.push_back( SeedChainResult{ std::move( f.chain ), f.proposalPdf } );
			continue;
		}

		// Build the vertex once — branch logic mutates `isReflection`
		// after pushing.
		ManifoldVertex mv;
		mv.position    = ri.geometric.ptIntersection;
		mv.normal      = ri.geometric.vNormal;
		mv.uv          = ri.geometric.ptCoord;
		mv.pObject     = ri.pObject;
		mv.pMaterial   = pMat;
		mv.eta         = specInfo.ior;
		mv.attenuation = specInfo.attenuation;
		mv.canRefract  = specInfo.canRefract;
		mv.isReflection = !specInfo.canRefract;
		mv.valid       = false;

		f.seedIor.SetCurrentObject( ri.pObject );
		const bool sameObjectAgain = f.seedIor.containsCurrent();
		const Scalar cosI = Vector3Ops::Dot( f.dir, mv.normal );
		const bool bEntering = sameObjectAgain ? false : (cosI < 0);
		mv.isExiting = !bEntering;
		if( bEntering ) {
			mv.etaI = f.currentIOR;
			mv.etaT = specInfo.ior;
		} else {
			mv.etaI = specInfo.ior;
			mv.etaT = f.currentIOR;
		}

		f.chain.push_back( mv );
		const std::size_t idxJustPushed = f.chain.size() - 1;
		const Point3 hitPos = ri.geometric.ptIntersection;

		// Pure mirror — single continuation, no branch / RR.
		if( !specInfo.canRefract ) {
			const Scalar cosI_refl = -Vector3Ops::Dot( f.dir, mv.normal );
			f.dir = f.dir + mv.normal * (2.0 * cosI_refl);
			f.dir = Vector3Ops::Normalize( f.dir );
			f.currentOrigin = hitPos;
			f.throughput *= ColorMath::MaxValue( specInfo.attenuation );
			active.push_back( std::move( f ) );
			continue;
		}

		// Dielectric: compute the refraction-side geometry first so the
		// reflection branch can borrow `cosI2` (Fresnel argument) and
		// the refraction branch can use `etaRatio` / `sin2T`.
		Vector3 nFace = mv.normal;
		Scalar etaRatio;
		if( bEntering ) {
			etaRatio = f.currentIOR / specInfo.ior;
		} else {
			etaRatio = specInfo.ior / f.currentIOR;
			if( Vector3Ops::Dot( f.dir, nFace ) > 0 ) nFace = nFace * (-1.0);
		}
		if( Vector3Ops::Dot( f.dir, nFace ) > 0 ) nFace = nFace * (-1.0);

		const Scalar cosI2 = -Vector3Ops::Dot( f.dir, nFace );
		const Scalar sin2T = etaRatio * etaRatio * (1.0 - cosI2 * cosI2);

		// TIR — forced reflection.  No Fresnel branch / RR.
		if( sin2T > 1.0 ) {
			f.chain[ idxJustPushed ].isReflection = true;
			f.dir = f.dir + nFace * (2.0 * cosI2);
			f.dir = Vector3Ops::Normalize( f.dir );
			f.currentOrigin = hitPos;
			active.push_back( std::move( f ) );
			continue;
		}

		// Sub-critical dielectric: Fresnel decision point.
		const Scalar Fr = ComputeDielectricFresnel( cosI2, mv.etaI, mv.etaT );
		// Branch at every dielectric vertex whose running throughput
		// is high enough — this is "Option B for the first N levels,
		// then Option A in the tail" as the throughput naturally
		// decimates with each Fresnel multiplication.  Unlike PT (which
		// splits once per camera ray because later recursive frames
		// inherit splitFired), SMS enumerates distinct caustic chains
		// — refract-refract, refract-reflect, reflect-only at v0 are
		// each separate physical paths that contribute their own
		// caustic.  Single-split would miss the refract-reflect-refract
		// class on nested dielectrics (e.g. Veach-egg's outer-then-
		// inner-cavity reflection caustic), which is exactly the
		// energy we're trying to recover.
		const bool shouldBranch = ( f.throughput > bThreshold );

		// Refraction continuation — built from f (or its post-RR-pick
		// state) into either the same f or a fresh frame depending on
		// branching.
		auto applyRefraction = [&]( Frame& target, Scalar weight, Scalar pdfFactor ) {
			const Scalar cosT = std::sqrt( 1.0 - sin2T );
			target.dir = target.dir * etaRatio + nFace * (etaRatio * cosI2 - cosT);
			target.dir = Vector3Ops::Normalize( target.dir );
			if( bEntering ) {
				target.currentIOR = specInfo.ior;
				target.seedIor.SetCurrentObject( ri.pObject );
				target.seedIor.push( specInfo.ior );
			} else {
				if( sameObjectAgain ) {
					target.seedIor.SetCurrentObject( ri.pObject );
					target.seedIor.pop();
					target.currentIOR = target.seedIor.top();
				} else {
					target.currentIOR = 1.0;
				}
				target.chain[ idxJustPushed ].etaT = target.currentIOR;
			}
			target.currentOrigin = hitPos;
			target.throughput   *= weight;
			target.proposalPdf  *= pdfFactor;
		};

		auto applyReflection = [&]( Frame& target, Scalar weight, Scalar pdfFactor ) {
			target.chain[ idxJustPushed ].isReflection = true;
			target.dir = target.dir + nFace * (2.0 * cosI2);
			target.dir = Vector3Ops::Normalize( target.dir );
			target.currentOrigin = hitPos;
			target.throughput   *= weight;
			target.proposalPdf  *= pdfFactor;
		};

		if( shouldBranch )
		{
			// Branch: spawn the reflection continuation and move the
			// original f onto the refraction branch.
			//
			// Reflection branch — uniform-area-resampled v_i.
			//
			//   The Snell-trace places this just-pushed vertex in the
			//   REFRACTION-root basin (that's where it hit on the way
			//   through).  Newton walking the chain from there to the
			//   REFLECTION-root basin diverges most of the time —
			//   verified via per-seed convergence stats: 87% solveOK
			//   on refract-refract vs 0% on the reflect-only chain at
			//   the same v0 position.
			//
			//   Fix: for the reflection branch, REPLACE the just-pushed
			//   vertex's position with a uniform-area sample on the
			//   same caster (Mitsuba `manifold_ss::sample_path`-style).
			//   Across many spp, uniform-area sampling reaches every
			//   reflection-root basin statistically — Newton then
			//   converges from a seed that's actually near the root.
			//
			//   Visibility check: the ray from `start` to the new
			//   position must hit this caster as the FIRST specular
			//   interaction (matches Mitsuba's `si_init.shape != shape`
			//   rejection).  If blocked, the reflection branch is
			//   dropped (correctly biased toward zero — the path
			//   doesn't physically exist for this shading point).
			//
			//   The resulting chain is TRUNCATED at this vertex (k = i+1)
			//   — we don't continue Snell-tracing from the reflected
			//   ray because the new uniform-sampled position has no
			//   meaningful "continuing" trace direction.  Multi-bounce
			//   reflection chains beyond the split point are not
			//   captured here; that's a deeper extension (deferred).
			//
			//   The refraction branch is UNCHANGED — keeps the Snell-
			//   trace seed where Newton converges reliably.
			{
				Frame reflectFrame = f;   // deep-copy chain + IORStack
				ManifoldVertex& vi = reflectFrame.chain[ idxJustPushed ];

				Point3 sampledPos;
				Vector3 sampledNormal;
				Point2 sampledUv;
				mv.pObject->UniformRandomPoint(
					&sampledPos, &sampledNormal, &sampledUv,
					Point3( sampler.Get1D(), sampler.Get1D(), sampler.Get1D() ) );

				// Visibility from start to the new sampled position.
				// We re-use the ray-cast result for accurate hit data
				// (modifier-aware, matching the rest of BuildSeedChain).
				Vector3 dirToSample = Vector3Ops::mkVector3( sampledPos, start );
				const Scalar distToSample = Vector3Ops::NormalizeMag( dirToSample );

				bool reflectAccepted = false;
				if( distToSample > NEARZERO )
				{
					Ray visRay( start, dirToSample );
					RayIntersection visRi( visRay, nullRasterizerState );
					pObjMgr->IntersectRay( visRi, true, true, false );
					if( visRi.geometric.bHit && visRi.pModifier ) {
						visRi.pModifier->Modify( visRi.geometric );
					}

					if( visRi.geometric.bHit && visRi.pObject == mv.pObject )
					{
						vi.position    = visRi.geometric.ptIntersection;
						vi.normal      = visRi.geometric.vNormal;
						vi.uv          = visRi.geometric.ptCoord;
						vi.isReflection = true;
						vi.valid       = false;

						// Determine entering vs exiting from the new
						// trace direction at the sampled point.
						const Scalar cosI_new = Vector3Ops::Dot( dirToSample, vi.normal );
						const bool bEnt = ( cosI_new < 0 );
						vi.isExiting = !bEnt;
						if( bEnt ) {
							vi.etaI = 1.0;   // assume air on the start side at v0
							vi.etaT = mv.eta;
						} else {
							vi.etaI = mv.eta;
							vi.etaT = 1.0;
						}

						// Truncate the chain at this vertex (k = idxJustPushed + 1)
						reflectFrame.chain.resize( idxJustPushed + 1 );
						reflectFrame.throughput *= Fr;
						out.push_back( SeedChainResult{
							std::move( reflectFrame.chain ),
							reflectFrame.proposalPdf } );
						reflectAccepted = true;
					}
				}
				(void)reflectAccepted;   // dropped silently if false
			}

			// Refraction branch — Snell-trace continues unchanged.
			applyRefraction( f, 1.0 - Fr, 1.0 );
			active.push_back( std::move( f ) );
		}
		else
		{
			// RR pick weighted by Fr.  Multiplying `proposalPdf` by Fr
			// (or 1-Fr) ensures the caller's `contribution / proposalPdf`
			// cancels the BSDF's Fresnel factor (`EvaluateChainThroughput`
			// returns `Fr` for reflection / `1-Fr` for refraction).
			const Scalar u = sampler.Get1D();
			if( u < Fr ) {
				applyReflection( f, Fr, Fr );
			} else {
				applyRefraction( f, 1.0 - Fr, 1.0 - Fr );
			}
			active.push_back( std::move( f ) );
		}
	}

	return static_cast<unsigned int>( out.size() );
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainThroughput
//
//   Computes Fresnel-weighted transmittance/reflectance product
//   along a converged specular chain, including Beer's law.
//////////////////////////////////////////////////////////////////////
// EvaluateChainGeometry
//
//   Computes the geometric coupling factor through a specular chain
//   for the path integral.  For delta BSDFs, integrating the delta
//   function cancels the outgoing cosine at each specular vertex.
//   What remains per segment is cos(θ_incoming) / dist².
//
//   For chain  x → v_1 → ... → v_k → y  the result is:
//
//     cos(θ_x) × ∏_{j=1}^{k} [cos(θ_{vj,in}) / dist(prev,vj)²]
//              × cos(θ_y) / dist(vk,y)²
//
//   The caller supplies cosAtShading (= cos θ_x) and cosAtLight
//   (= cos θ_y) separately, so this function returns the product
//   of the segment factors only:
//
//     ∏_{j=1}^{k} [cos(θ_{vj,in}) / dist(prev,vj)²]  ×  1/dist(vk,y)²
//
//   cosAtShading and cosAtLight are multiplied by the caller.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainGeometry(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 1.0;

	Scalar geom = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 dir = Vector3Ops::mkVector3( chain[i].position, prevPos );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;
		dir = dir * (1.0 / dist);

		// Incoming cosine at this specular vertex
		const Scalar cosIn = fabs( Vector3Ops::Dot( chain[i].normal, dir ) );

		geom *= cosIn / (dist * dist);
	}

	// Last segment: chain[k-1] to endPoint (the light).
	// Only 1/dist² here — cosAtLight is supplied by the caller.
	{
		Vector3 dir = Vector3Ops::mkVector3( endPoint, chain[k-1].position );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;

		geom *= 1.0 / (dist * dist);
	}

	return geom;
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainCosineProduct
//
//   Product of incoming cosines at specular vertices, WITHOUT
//   distance terms.  Used in the SMS contribution formula where
//   the 1/dist² factors are already in the Jacobian determinant.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainCosineProduct(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 1.0;

	Scalar cosProduct = 1.0;

	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 dir = Vector3Ops::mkVector3( chain[i].position, prevPos );
		const Scalar dist = Vector3Ops::Magnitude( dir );
		if( dist < 1e-8 ) return 0.0;
		dir = dir * (1.0 / dist);

		cosProduct *= fabs( Vector3Ops::Dot( chain[i].normal, dir ) );
	}

	return cosProduct;
}

//////////////////////////////////////////////////////////////////////

RISEPel ManifoldSolver::EvaluateChainThroughput(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain
	) const
{
	RISEPel throughput( 1.0, 1.0, 1.0 );
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return throughput;
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		// Compute incoming direction at this vertex
		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;

		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// Exact dielectric Fresnel reflectance.  Use the chain-topological
		// flag `v.isExiting` (set by BuildSeedChain's IOR-stack bookkeeping)
		// to select eta_i / eta_t, NOT a local sign(dot(wi, n)) test.  For
		// a thin double-sided mesh the normal can point the same way at
		// entry and exit, so the sign-of-cosI test reports both crossings
		// as "entering" and the Fresnel factor comes out wrong.
		const Scalar cosI = fabs( Vector3Ops::Dot( wi, v.normal ) );
		// Convention: eta_i is the IOR on the prev (x-receiver) side,
		// eta_t on the next (y-source) side, for the photon's FORWARD
		// direction y -> v -> x.  In the chain-build (reverse) direction:
		//   v.isExiting = false ⇒ chain-ray enters glass at v ⇒
		//     prev side is outside (air)   → eta_i = 1
		//     next side is inside (glass)  → eta_t = v.eta
		//   v.isExiting = true  ⇒ chain-ray exits glass at v ⇒
		//     prev side is inside (glass)  → eta_i = v.eta
		//     next side is outside (air)   → eta_t = 1
		// (This matches the old sign(dot(wi, n)) convention where
		// cosI_signed >= 0 meant "wi points to air side".)
		// Pull (η_i, η_t) from the vertex's BuildSeedChain-populated
		// fields, with back-compat fallback to "air on the other side"
		// for hand-constructed test chains.  The OLD code had:
		//   const Scalar eta_i = v.isExiting ? v.eta : 1.0;
		//   const Scalar eta_t = v.isExiting ? 1.0 : v.eta;
		// which silently assumed every interface had air on the
		// non-material side — wrong for nested dielectrics (Veach Egg
		// air_cavity inside glass).  Fresnel reflectance at a
		// glass→air interface (Fr ≈ 0.04 for normal incidence) differs
		// substantially from a glass→glass interface (Fr = 0 if same
		// IOR, monotonic in |Δη| otherwise), so the bug shows up as
		// caustic energy that's the wrong intensity even when the
		// chain itself converged.
		Scalar eta_i, eta_t;
		GetEffectiveEtas( v, eta_i, eta_t );

		// Three semantically distinct vertex kinds drive three different
		// throughput laws.  The dispatch is on (canRefract, isReflection):
		//
		//   (false, true)   pure mirror — full reflectance from the painter,
		//                   no Fresnel angle factor.  PerfectReflectorSPF
		//                   models an idealized 100%-reflective surface;
		//                   ComputeDielectricFresnel(cosI, 1, 1) would give
		//                   the wrong answer (=0) here because the dielectric
		//                   formula is meaningless on a non-refracting medium.
		//   (true,  true)   dielectric reflection — Fresnel reflection on
		//                   glass, OR total internal reflection (the latter
		//                   falls out automatically: ComputeDielectricFresnel
		//                   returns 1.0 when sin²θ_t ≥ 1).  Reflection at a
		//                   dielectric interface is uncolored (the painter's
		//                   refractance only enters via Beer's law during
		//                   transmission) — matches PerfectRefractorSPF::
		//                   Scatter, which sets the Fresnel-reflection ray's
		//                   kray = (Fr, Fr, Fr) without a tau multiplier.
		//   (true,  false)  refraction — Fresnel transmission (1 − Fr) with
		//                   the tau (refractance) painter and the (η_i/η_t)²
		//                   radiance rescale across the dielectric boundary.
		if( v.isReflection )
		{
			Scalar R;
			if( v.canRefract )
			{
				R = ComputeDielectricFresnel( cosI, eta_i, eta_t );
			}
			else
			{
				R = 1.0;
			}
			throughput = throughput * v.attenuation * R;
		}
		else
		{
			// Radiance transport across a refracting interface:
			//   L_receiver = T * (n_receiver / n_source)^2 * L_source
			//
			// This (n_r/n_s)^2 factor is the standard radiance rescaling
			// across a dielectric boundary (radiance is NOT preserved;
			// L/n^2 is).  RISE's PerfectRefractorSPF omits this factor
			// in the forward path tracer; including it here only in SMS
			// gives SMS physically correct radiance while leaving
			// PT/VCM's "all air" convention intact.
			//
			// eta_i is the index on the x-receiver side, eta_t on the
			// y-source side, in the photon's FORWARD direction.  The
			// forward rescale is (n_receiver / n_source)^2 = (eta_i / eta_t)^2.
			const Scalar fr = ComputeDielectricFresnel( cosI, eta_i, eta_t );
			const Scalar eta_ratio = eta_i / eta_t;
			const Scalar radiance_rescale = eta_ratio * eta_ratio;
			throughput = throughput * v.attenuation * (1.0 - fr) * radiance_rescale;
		}
	}

	return throughput;
}

//////////////////////////////////////////////////////////////////////
// EvaluateChainThroughputNM
//
//   Scalar (spectral) variant of EvaluateChainThroughput.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EvaluateChainThroughputNM(
	const Point3& startPoint,
	const Point3& endPoint,
	const std::vector<ManifoldVertex>& chain,
	const Scalar nm
	) const
{
	Scalar throughput = 1.0;
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return throughput;
	}

	for( unsigned int i = 0; i < k; i++ )
	{
		const ManifoldVertex& v = chain[i];

		const Point3 prevPos = (i == 0) ? startPoint : chain[i-1].position;
		Vector3 wi = Vector3Ops::mkVector3( prevPos, v.position );
		wi = Vector3Ops::Normalize( wi );

		// Exact dielectric Fresnel reflectance — use the chain-topological
		// flag (see EvaluateChainThroughput RGB variant for full comment).
		// Pull (η_i, η_t) from the per-vertex fields populated by
		// BuildSeedChainNM — same air-on-other-side bug fix as the
		// RGB variant.
		const Scalar cosI = fabs( Vector3Ops::Dot( wi, v.normal ) );
		Scalar eta_i, eta_t;
		GetEffectiveEtas( v, eta_i, eta_t );

		// Same three-case dispatch as the RGB variant: pure mirrors take
		// full reflectance, dielectric reflection (incl. TIR) and
		// refraction use Fresnel.  See EvaluateChainThroughput for the
		// full discussion of why ComputeDielectricFresnel(cosI, 1, 1)
		// would silently zero the throughput on a mirror.
		if( v.isReflection )
		{
			if( v.canRefract )
			{
				throughput *= ComputeDielectricFresnel( cosI, eta_i, eta_t );
			}
			// else: pure mirror — multiply by 1 (no-op).  Spectral
			// reflectance painters aren't queried in this NM throughput
			// path; surface colour is applied at the integrator level.
		}
		else
		{
			const Scalar fr = ComputeDielectricFresnel( cosI, eta_i, eta_t );
			throughput *= (1.0 - fr);
		}
	}

	return throughput;
}

//////////////////////////////////////////////////////////////////////
// ComputeManifoldGeometricTerm
//
//   Computes the generalized geometric term (Jacobian determinant
//   ratio) for MIS weighting.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeManifoldGeometricTerm(
	const std::vector<ManifoldVertex>& chain,
	const Point3& fixedStart,
	const Point3& fixedEnd
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );

	if( k == 0 )
	{
		return 1.0;
	}

	// Build Jacobian and compute its determinant
	std::vector<Scalar> diag, upper_blocks, lower_blocks;
	BuildJacobian( chain, fixedStart, fixedEnd, diag, upper_blocks, lower_blocks );

	const Scalar detProduct = ComputeBlockTridiagonalDeterminant(
		diag, upper_blocks, lower_blocks, k );

	// Geometric distances
	Scalar distProduct = 1.0;
	for( unsigned int i = 0; i < k; i++ )
	{
		const Point3 prevPos = (i == 0) ? fixedStart : chain[i-1].position;
		const Point3 nextPos = (i == k-1) ? fixedEnd : chain[i+1].position;

		const Scalar d1 = Point3Ops::Distance( prevPos, chain[i].position );
		const Scalar d2 = Point3Ops::Distance( chain[i].position, nextPos );

		if( d1 > NEARZERO && d2 > NEARZERO )
		{
			distProduct *= d1 * d1 * d2 * d2;
		}
	}

	// Apply surface metric correction: convert from parameter-space
	// Jacobian to tangent-plane world-space Jacobian
	Scalar metricProduct = 1.0;
	for( unsigned int i = 0; i < k; i++ )
	{
		const Scalar dpduLen = Vector3Ops::Magnitude( chain[i].dpdu );
		const Scalar dpdvLen = Vector3Ops::Magnitude( chain[i].dpdv );
		if( dpduLen > NEARZERO && dpdvLen > NEARZERO )
		{
			metricProduct *= dpduLen * dpdvLen;
		}
	}

	Scalar geoTerm = fabs( detProduct ) / fmax( metricProduct, 1e-20 );
	if( distProduct > NEARZERO )
	{
		geoTerm /= distProduct;
	}

	// Clamp
	if( geoTerm > config.maxGeometricTerm )
	{
		geoTerm = config.maxGeometricTerm;
	}

	return geoTerm;
}

//////////////////////////////////////////////////////////////////////
// ComputeLastBlockLightJacobian
//
//   Computes the 2x2 block ∂C_at_vk / ∂y_tangent.  Mirrors the half-
//   vector setup in BuildJacobian, but differentiates with respect to
//   the LIGHT endpoint y instead of the specular vertex position.
//
//   Only wo (direction from v_k to y) depends on y; wi does not.
//   So ∂h_raw/∂y = (reflection ? ∂wo/∂y : -eta_eff * ∂wo/∂y).
//
//   Result is used with ComputeLightToFirstVertexJacobianDet to
//   propagate y-perturbations through the chain via block-tridiagonal
//   solve.
//////////////////////////////////////////////////////////////////////

void ManifoldSolver::ComputeLastBlockLightJacobian(
	const ManifoldVertex& vk,
	const Point3& prevPos,
	const Point3& lightPos,
	const Vector3& lightNormal,
	Scalar Jy[4]
	) const
{
	Jy[0] = Jy[1] = Jy[2] = Jy[3] = 0;

	// wi: direction from vk toward prev (same sign convention as BuildJacobian)
	Vector3 d_wi = Vector3Ops::mkVector3( prevPos, vk.position );
	const Scalar dist_i = Vector3Ops::NormalizeMag( d_wi );
	const Vector3 wi = d_wi;

	// wo: direction from vk toward next (= y).  When y moves by δy,
	// new d_wo = lightPos + δy - vk.position.  So ∂wo/∂y applies
	// (I - wo⊗wo)/dist_o to δy (in world-space coordinates).
	Vector3 d_wo = Vector3Ops::mkVector3( lightPos, vk.position );
	const Scalar dist_o = Vector3Ops::NormalizeMag( d_wo );
	const Vector3 wo = d_wo;

	if( dist_i < NEARZERO || dist_o < NEARZERO ) return;

	// Tangent frame at vk (assumes OrthonormalizeTangentFrame has been
	// called upstream, so these are unit orthogonal).
	const Vector3 s_v = Vector3Ops::Normalize( vk.dpdu );
	const Vector3 t_v = Vector3Ops::Normalize( vk.dpdv );

	// Walter form half-vector (η_i, η_t) — same convention and back-
	// compat fallback as BuildJacobian / EvaluateConstraint.
	Scalar eta_i_v = Scalar( 1.0 );
	Scalar eta_t_v = Scalar( 1.0 );
	if( !vk.isReflection ) {
		GetEffectiveEtas( vk, eta_i_v, eta_t_v );
	}

	// Half-vector (sign follows BuildJacobian)
	Vector3 h_raw;
	if( vk.isReflection ) {
		h_raw = Vector3( wi.x + wo.x, wi.y + wo.y, wi.z + wo.z );
	} else {
		h_raw = Vector3(
			-(eta_i_v * wi.x + eta_t_v * wo.x),
			-(eta_i_v * wi.y + eta_t_v * wo.y),
			-(eta_i_v * wi.z + eta_t_v * wo.z) );
	}
	const Scalar h_len = Vector3Ops::Magnitude( h_raw );
	if( h_len < NEARZERO ) return;
	const Vector3 h = h_raw * (1.0 / h_len);

	// Tangent basis at y (orthonormal, perpendicular to lightNormal)
	Vector3 y_s = Vector3Ops::Perpendicular( lightNormal );
	y_s = Vector3Ops::Normalize( y_s );
	Vector3 y_t = Vector3Ops::Cross( lightNormal, y_s );
	y_t = Vector3Ops::Normalize( y_t );

	const Scalar inv_lo = 1.0 / dist_o;

	// For each y tangent direction, compute ∂C/∂(that direction)
	for( int j = 0; j < 2; j++ ) {
		const Vector3& ydir = (j == 0) ? y_s : y_t;

		// ∂wo/∂y_dir = (I - wo⊗wo) * ydir / dist_o
		const Scalar wo_dot_ydir = Vector3Ops::Dot( wo, ydir );
		const Vector3 dwo(
			(ydir.x - wo.x * wo_dot_ydir) * inv_lo,
			(ydir.y - wo.y * wo_dot_ydir) * inv_lo,
			(ydir.z - wo.z * wo_dot_ydir) * inv_lo );

		// ∂h_raw/∂y.  wi is independent of y (depends only on prev and vk).
		// Walter form: ∂h/∂y = -η_t ∂wo/∂y (only wo depends on y).
		Vector3 dh_raw;
		if( vk.isReflection ) {
			dh_raw = dwo;
		} else {
			dh_raw = Vector3( -eta_t_v * dwo.x, -eta_t_v * dwo.y, -eta_t_v * dwo.z );
		}

		// ∂h/∂y = (dh_raw - h * dot(h, dh_raw)) / h_len
		const Scalar h_dot = Vector3Ops::Dot( h, dh_raw );
		const Vector3 dh(
			(dh_raw.x - h.x * h_dot) * (1.0 / h_len),
			(dh_raw.y - h.y * h_dot) * (1.0 / h_len),
			(dh_raw.z - h.z * h_dot) * (1.0 / h_len) );

		// Project onto (s_v, t_v) basis at vk.  Row index = {s_v, t_v}.
		const Scalar s_dot = Vector3Ops::Dot( s_v, dh );
		const Scalar t_dot = Vector3Ops::Dot( t_v, dh );
		Jy[ 0 * 2 + j ] = s_dot;  // ∂Cs/∂y_j
		Jy[ 1 * 2 + j ] = t_dot;  // ∂Ct/∂y_j
	}
}

//////////////////////////////////////////////////////////////////////
// ComputeLightToFirstVertexJacobianDet
//
//   Implicit-function-theorem application: the chain constraint
//   C(v_1, ..., v_k, y) = 0 defines v as a function of y.  This
//   returns |det(δv_1_⊥ / δy_⊥)|.
//
//   For k=1 this degenerates to |det(∂C/∂y)| / |det(∂C/∂v_1)|.
//   For k>1 we must solve the block-tridiagonal system
//     J_v * δv = -J_y * δy
//   where J_y has only its last block nonzero.  We solve twice
//   (one per y tangent direction) to recover the 2x2 matrix δv_1/δy.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::ComputeLightToFirstVertexJacobianDet(
	const std::vector<ManifoldVertex>& chain,
	const Point3& shadingPoint,
	const Point3& lightPos,
	const Vector3& lightNormal
	) const
{
	const unsigned int k = static_cast<unsigned int>( chain.size() );
	if( k == 0 ) return 0;

	// Build full chain Jacobian J_v (block-tridiagonal).
	std::vector<Scalar> diag, upper, lower;
	BuildJacobian( chain, shadingPoint, lightPos, diag, upper, lower );

	// Build the light-side Jacobian block at v_k.
	const Point3 prevPos = (k == 1)
		? shadingPoint
		: chain[k-2].position;
	Scalar Jy[4];
	ComputeLastBlockLightJacobian( chain[k-1], prevPos, lightPos, lightNormal, Jy );

	// Solve J_v * δv = -RHS for each of two y-tangent directions.
	// RHS is zero except for the last 2 entries = Jy column j.
	Scalar dv1[4];  // row-major: [δv1_s for δy_s, δv1_s for δy_t; δv1_t for δy_s, δv1_t for δy_t]

	for( unsigned int j = 0; j < 2; j++ ) {
		std::vector<Scalar> rhs( 2 * k, 0.0 );
		rhs[ 2 * (k - 1) + 0 ] = -Jy[ 0 * 2 + j ];
		rhs[ 2 * (k - 1) + 1 ] = -Jy[ 1 * 2 + j ];

		std::vector<Scalar> delta;
		std::vector<Scalar> diag_copy = diag;  // SolveBlockTridiagonal takes non-const ref
		if( !SolveBlockTridiagonal( diag_copy, upper, lower, rhs, k, delta ) ) {
			return 0;
		}

		dv1[ 0 * 2 + j ] = delta[0];
		dv1[ 1 * 2 + j ] = delta[1];
	}

	// 2x2 determinant
	const Scalar det = dv1[0] * dv1[3] - dv1[1] * dv1[2];
	return fabs( det );
}

//////////////////////////////////////////////////////////////////////
// EstimatePDF
//
//   Bernoulli trial estimator for unbiased PDF estimation.
//////////////////////////////////////////////////////////////////////

Scalar ManifoldSolver::EstimatePDF(
	const ManifoldResult& solution,
	const Point3& shadingPoint,
	const Point3& emitterPoint,
	const std::vector<ManifoldVertex>& seedTemplate,
	ISampler& sampler
	) const
{
	if( !solution.valid )
	{
		return 1.0;
	}

	unsigned int count = 0;
	unsigned int trials = 0;
	const unsigned int targetCount = 1;
	const unsigned int k = static_cast<unsigned int>( solution.specularChain.size() );

	while( count < targetCount && trials < config.maxBernoulliTrials )
	{
		// Generate a random seed chain by perturbing the template
		std::vector<ManifoldVertex> testChain( seedTemplate );

		for( unsigned int i = 0; i < testChain.size(); i++ )
		{
			// Random perturbation in tangent plane
			const Scalar ru = sampler.Get1D() * 0.2 - 0.1;
			const Scalar rv = sampler.Get1D() * 0.2 - 0.1;

			testChain[i].position = Point3Ops::mkPoint3(
				testChain[i].position,
				testChain[i].dpdu * ru + testChain[i].dpdv * rv
				);

			// Re-snap to surface
			UpdateVertexOnSurface( testChain[i], 0.0, 0.0 );
		}

		// Run Newton solve on this perturbed chain
		if( NewtonSolve( testChain, shadingPoint, emitterPoint ) )
		{
			// Check if this converged to the same solution
			bool same = true;
			for( unsigned int i = 0; i < k && i < testChain.size(); i++ )
			{
				const Scalar dist = Point3Ops::Distance(
					testChain[i].position,
					solution.specularChain[i].position
					);
				if( dist > config.uniquenessThreshold )
				{
					same = false;
					break;
				}
			}

			if( same )
			{
				count++;
			}
		}

		trials++;
	}

	if( count == 0 )
	{
		return 1.0;  // Fallback
	}

	return static_cast<Scalar>(trials) / static_cast<Scalar>(count);
}

//////////////////////////////////////////////////////////////////////
// Solve (main entry point)
//////////////////////////////////////////////////////////////////////

ManifoldResult ManifoldSolver::Solve(
	const Point3& shadingPoint,
	const Vector3& shadingNormal,
	const Point3& emitterPoint,
	const Vector3& emitterNormal,
	std::vector<ManifoldVertex>& specularChain,
	ISampler& sampler
	) const
{
	ManifoldResult result;

#if SMS_TRACE_DIAGNOSTIC
	// Ungated global counters: every Solve call increments one of these
	// buckets so we can see the failure breakdown across the whole image
	// (the traceSolve-gated verbose logging only looks at one patch).
	static std::atomic<int> g_solveTotal{ 0 };
	static std::atomic<int> g_solveEmpty{ 0 };
	static std::atomic<int> g_solveSeedTooFar{ 0 };
	static std::atomic<int> g_solveDerivFail{ 0 };
	static std::atomic<int> g_solveNewtonFail{ 0 };
	static std::atomic<int> g_solvePhysicsFail{ 0 };
	static std::atomic<int> g_solveShortSeg{ 0 };
	static std::atomic<int> g_solveOk{ 0 };
	const int st = g_solveTotal.fetch_add( 1, std::memory_order_relaxed );
	// Periodic print (every 200k solves) so we see the rate without flooding
	if( (st & 0x3ffff) == 0 && st > 0 ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_SOLVE_STATS: total=%d empty=%d seedTooFar=%d derivFail=%d newtonFail=%d physicsFail=%d shortSeg=%d ok=%d",
			st,
			g_solveEmpty.load(), g_solveSeedTooFar.load(),
			g_solveDerivFail.load(), g_solveNewtonFail.load(),
			g_solvePhysicsFail.load(), g_solveShortSeg.load(),
			g_solveOk.load() );
	}

	static std::atomic<int> g_solveTraceCount{ 0 };
	const bool traceSolve =
		( std::fabs( shadingPoint.x ) < 0.02 ) &&
		( std::fabs( shadingPoint.z ) < 0.02 ) &&
		( shadingPoint.y >= -0.02 && shadingPoint.y <= 0.02 ) &&
		( g_solveTraceCount.fetch_add( 1, std::memory_order_relaxed ) < 10 );
	if( traceSolve ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_SOLVE: enter k=%zu  shading=(%.4f,%.4f,%.4f)  emitter=(%.4f,%.4f,%.4f)",
			specularChain.size(),
			shadingPoint.x, shadingPoint.y, shadingPoint.z,
			emitterPoint.x, emitterPoint.y, emitterPoint.z );
	}
#endif

#if SMS_SOLVE_DIAG
	g_solveDiag_calls.fetch_add( 1, std::memory_order_relaxed );
#endif

	if( specularChain.empty() )
	{
#if SMS_TRACE_DIAGNOSTIC
		g_solveEmpty.fetch_add( 1, std::memory_order_relaxed );
#endif
		return result;
	}

	// Quick early-out: build minimal tangent frames from normals
	// and evaluate the initial constraint.  If the norm is too large,
	// bail before computing expensive surface derivatives.
	for( unsigned int i = 0; i < specularChain.size(); i++ )
	{
		ManifoldVertex& v = specularChain[i];
		if( Vector3Ops::SquaredModulus( v.dpdu ) < NEARZERO ||
			Vector3Ops::SquaredModulus( v.dpdv ) < NEARZERO )
		{
			v.dpdu = Vector3Ops::Perpendicular( v.normal );
			v.dpdu = Vector3Ops::Normalize( v.dpdu );
			v.dpdv = Vector3Ops::Cross( v.normal, v.dpdu );
			v.dpdv = Vector3Ops::Normalize( v.dpdv );
		}
	}

	// Evaluate constraint with minimal tangent frames.
	// Early-out if the seed is too far from any valid path.
	// Use a generous threshold — reflection seeds can be further
	// from the solution than refraction seeds because the straight-line
	// seed direction doesn't follow the reflected path.
	{
		std::vector<Scalar> C0;
		EvaluateConstraint( specularChain, shadingPoint, emitterPoint, C0 );
		Scalar norm2 = 0.0;
		for( unsigned int i = 0; i < C0.size(); i++ )
			norm2 += C0[i] * C0[i];
#if SMS_TRACE_DIAGNOSTIC
		if( traceSolve ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_SOLVE:  initial ||C||=%.4f  threshold=2.0", sqrt(norm2) );
		}
#endif
		if( sqrt(norm2) > 2.0 )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_solveSeedTooFar.fetch_add( 1, std::memory_order_relaxed );
			if( traceSolve ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_SOLVE:  EARLY-RETURN: seed too far from any valid path" );
			}
#endif
#if SMS_SOLVE_DIAG
			g_solveDiag_seedTooFar.fetch_add( 1, std::memory_order_relaxed );
#endif
			return result;  // Seed too far from valid path
		}
	}

	// Now compute full surface derivatives (the expensive part)
	for( unsigned int i = 0; i < specularChain.size(); i++ )
	{
		ManifoldVertex& v = specularChain[i];
		if( !v.valid )
		{
			if( !ComputeVertexDerivatives( v ) )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_solveDerivFail.fetch_add( 1, std::memory_order_relaxed );
				if( traceSolve ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SOLVE:  REJECTED: ComputeVertexDerivatives failed at vertex %u (pos=(%.4f,%.4f,%.4f))",
						i, v.position.x, v.position.y, v.position.z );
				}
#endif
#if SMS_SOLVE_DIAG
				g_solveDiag_derivFail.fetch_add( 1, std::memory_order_relaxed );
#endif
				return result;
			}
		}

		// Re-ensure tangent frame after derivative computation
		if( Vector3Ops::SquaredModulus( v.dpdu ) < NEARZERO ||
			Vector3Ops::SquaredModulus( v.dpdv ) < NEARZERO )
		{
			v.dpdu = Vector3Ops::Perpendicular( v.normal );
			v.dpdu = Vector3Ops::Normalize( v.dpdu );
			v.dpdv = Vector3Ops::Cross( v.normal, v.dpdu );
			v.dpdv = Vector3Ops::Normalize( v.dpdv );
		}
	}

	// Save seed template for Bernoulli trials
	const std::vector<ManifoldVertex> seedTemplate( specularChain );

	// SMS two-stage solver (Zeltner 2020 §5).  Run Newton first on the
	// SMOOTHED reference surface (smoothing = 1: underlying analytical base,
	// no displacement / no high-frequency detail), then refine on the
	// actual surface (smoothing = 0).  Opt-in via `config.twoStage`; only
	// fires for chains whose vertices all support
	// `IObject::ComputeAnalyticalDerivatives`.
	//
	// SCOPE (verified empirically, see docs/SMS_TWO_STAGE_SOLVER.md):
	//
	//  - HELPS on smooth analytic primitives + normal-perturbing maps
	//    (bumpmap_modifier / future normalmap_modifier on
	//    sphere/ellipsoid/etc.).  Normal field is bumpy but POSITION is
	//    invariant under smoothing — Stage 1's converged uv is at the
	//    same world position as the bumpy caustic root, Stage 2's seed
	//    is in the basin of attraction.  Empirically reaches ΣL_sms /
	//    ΣL_supp ≈ 1.0 at sane bump amplitudes (~10° max perturbation).
	//
	//  - HURTS on heavily-displaced meshes (displaced_geometry with
	//    disp_scale > a few percent of the curvature radius).  Stage 1
	//    converges to the SMOOTH-surface caustic at uv `u*`, whose
	//    corresponding actual-mesh position is up to `disp_scale` units
	//    away from where the bumpy caustic actually lives.  Stage 2's
	//    seed at `u*` is FARTHER from the bumpy caustic than the
	//    original Snell-traced on-mesh seed — two-stage actively
	//    regresses convergence.
	//
	// Mitsuba's reference matches this scope: their Figure 9 (the
	// dedicated two-stage demonstration) uses only smooth analytic
	// primitives + `normalmap` BSDFs; their Figure 16 (displaced-mesh
	// comparison) never engages two-stage.  Their smoothing is BSDF-
	// driven via `lean()` (Olano-Baker LEAN moments), which is zero by
	// default — so two-stage is a no-op for non-normal-mapped BSDFs in
	// their codebase regardless.  Our geometry-side smoothing extends
	// further (we CAN smooth a displaced surface to its base), but the
	// extension hits a regime the original method wasn't designed to
	// handle.  See docs/SMS_TWO_STAGE_SOLVER.md for the data and
	// proof-from-source citations.
	if( config.twoStage )
	{
#if SMS_TRACE_DIAGNOSTIC
		static std::atomic<int> g_twostage_attempted{ 0 };
		static std::atomic<int> g_twostage_inputs_unavailable{ 0 };
		static std::atomic<int> g_twostage_stage1_failed{ 0 };
		static std::atomic<int> g_twostage_transition_failed{ 0 };
		static std::atomic<int> g_twostage_full_success{ 0 };
		const int ta = g_twostage_attempted.fetch_add( 1, std::memory_order_relaxed );
		if( (ta & 0x3ffff) == 0 && ta > 0 ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TWOSTAGE: attempted=%d inputsUnavailable=%d stage1Failed=%d transitionFailed=%d fullSuccess=%d",
				ta,
				g_twostage_inputs_unavailable.load(),
				g_twostage_stage1_failed.load(),
				g_twostage_transition_failed.load(),
				g_twostage_full_success.load() );
		}
#endif

		// Snapshot the current state in case Stage 1 fails — we need to
		// fall back to the original on-mesh seed for Stage 2.
		const std::vector<ManifoldVertex> preStage1Snapshot( specularChain );

		// Re-evaluate every chain vertex's derivatives at smoothing = 1.
		// Bails out (cleanly, no scribble) if any vertex doesn't support
		// the smoothing-aware analytical query — those chains skip
		// two-stage and fall through to the original single-stage solve.
		bool stageInputsOk = true;
		for( unsigned int i = 0; i < specularChain.size(); i++ ) {
			if( !ComputeVertexDerivatives( specularChain[i], 1.0 ) ) {
				stageInputsOk = false;
				break;
			}
		}
		if( !stageInputsOk )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_twostage_inputs_unavailable.fetch_add( 1, std::memory_order_relaxed );
#endif
			specularChain = preStage1Snapshot;
		}
		if( stageInputsOk )
		{
			const bool stage1Converged = NewtonSolve(
				specularChain, shadingPoint, emitterPoint, 1.0 );
			if( stage1Converged ) {
				// Stage 1 found a seed on the smooth (smoothing=1) surface.
				// Bridge to the actual mesh in two steps:
				//   (a) Re-evaluate analytical at smoothing=0 to land on
				//       the smooth-displaced surface — the underlying
				//       continuous bumpy surface that the tessellated mesh
				//       approximates.  Position differs from the mesh hit
				//       by chord-vs-arc tessellation error (~0.005 units
				//       for detail=128) — small.
				//   (b) Run `ComputeVertexDerivatives` at smoothing=0,
				//       which FD-probes from the now-close-to-mesh position
				//       to find the actual mesh hit and pull per-triangle
				//       UV-Jacobian derivatives.  The 0.05-unit probe
				//       offset is sufficient because step (a) put us within
				//       chord-vs-arc.
				// Without step (a), step (b) probes from the smoothing=1
				// position — which can be ~disp_scale units inside the
				// bumpy mesh, beyond the probe's reach.
				bool transitionOk = true;
				for( unsigned int i = 0; i < specularChain.size(); i++ ) {
					ManifoldVertex& v = specularChain[i];
					Point3  aP;
					Vector3 aN, aDpdu, aDpdv, aDndu, aDndv;
					if( v.pObject->ComputeAnalyticalDerivatives(
							v.uv, 0.0, aP, aN, aDpdu, aDpdv, aDndu, aDndv ) )
					{
						v.position = aP;
						v.normal   = aN;
					}
					// Step (b): probe to actual mesh, pull mesh derivatives.
					if( !ComputeVertexDerivatives( v, 0.0 ) ) {
						transitionOk = false;
						break;
					}
				}
				if( !transitionOk ) {
#if SMS_TRACE_DIAGNOSTIC
					g_twostage_transition_failed.fetch_add( 1, std::memory_order_relaxed );
#endif
					specularChain = preStage1Snapshot;
				}
#if SMS_TRACE_DIAGNOSTIC
				else {
					g_twostage_full_success.fetch_add( 1, std::memory_order_relaxed );
				}
#endif
			} else {
				// Stage 1 didn't converge — restore pre-Stage-1 state and
				// fall through to single-stage Stage 2 with the original
				// on-mesh seed.
#if SMS_TRACE_DIAGNOSTIC
				g_twostage_stage1_failed.fetch_add( 1, std::memory_order_relaxed );
#endif
				specularChain = preStage1Snapshot;
			}
		}
	}

	// Run Newton solver — Stage 2 of the two-stage solver, or the single
	// stage when two-stage is disabled / inapplicable.
	const bool converged = NewtonSolve( specularChain, shadingPoint, emitterPoint );

#if SMS_TRACE_DIAGNOSTIC
	if( traceSolve ) {
		// Evaluate ||C|| after Newton to see how close it got
		std::vector<Scalar> Cfinal;
		EvaluateConstraint( specularChain, shadingPoint, emitterPoint, Cfinal );
		Scalar fn2 = 0;
		for( std::size_t i = 0; i < Cfinal.size(); i++ ) fn2 += Cfinal[i] * Cfinal[i];
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_SOLVE:  NewtonSolve converged=%d  final ||C||=%.4e  threshold=%.1e",
			int( converged ), sqrt( fn2 ), config.solverThreshold );
		for( std::size_t i = 0; i < specularChain.size(); i++ ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_SOLVE:   v[%zu] pos=(%.4f,%.4f,%.4f) n=(%.3f,%.3f,%.3f) eta=%.3f isRefl=%d isExit=%d",
				i,
				specularChain[i].position.x, specularChain[i].position.y, specularChain[i].position.z,
				specularChain[i].normal.x, specularChain[i].normal.y, specularChain[i].normal.z,
				specularChain[i].eta,
				int( specularChain[i].isReflection ), int( specularChain[i].isExiting ) );
		}
	}
#endif

	if( !converged )
	{
#if SMS_TRACE_DIAGNOSTIC
		g_solveNewtonFail.fetch_add( 1, std::memory_order_relaxed );
#endif
#if SMS_SOLVE_DIAG
		g_solveDiag_newtonFail.fetch_add( 1, std::memory_order_relaxed );
		// Bucket the post-Newton ||C|| so we know whether Newton was stuck
		// near the answer or diverged far.
		{
			std::vector<Scalar> Cend;
			EvaluateConstraint( specularChain, shadingPoint, emitterPoint, Cend );
			Scalar n2 = 0;
			for( std::size_t ii = 0; ii < Cend.size(); ii++ ) n2 += Cend[ii] * Cend[ii];
			const Scalar nrm = std::sqrt( n2 );
			if(      nrm < 1e-3 ) g_solveDiag_newtonFail_lt1e3.fetch_add( 1, std::memory_order_relaxed );
			else if( nrm < 1e-2 ) g_solveDiag_newtonFail_lt1e2.fetch_add( 1, std::memory_order_relaxed );
			else if( nrm < 1e-1 ) g_solveDiag_newtonFail_lt1e1.fetch_add( 1, std::memory_order_relaxed );
			else if( nrm < 1.0  ) g_solveDiag_newtonFail_lt1e0.fetch_add( 1, std::memory_order_relaxed );
			else                  g_solveDiag_newtonFail_ge1e0.fetch_add( 1, std::memory_order_relaxed );
		}
#endif
	}

	if( converged )
	{
		// Reject physically invalid converged solutions.
		if( !ValidateChainPhysics( specularChain, shadingPoint, emitterPoint ) )
		{
#if SMS_TRACE_DIAGNOSTIC
			g_solvePhysicsFail.fetch_add( 1, std::memory_order_relaxed );
			if( traceSolve ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_SOLVE:  REJECTED by ValidateChainPhysics (wi/wo sidedness mismatch)" );
			}
#endif
#if SMS_SOLVE_DIAG
			g_solveDiag_physicsFail.fetch_add( 1, std::memory_order_relaxed );
#endif
			return result;
		}

		// Reject chains with very short inter-vertex segments.
		//
		// The surface derivatives are computed via central finite
		// differences with probe radius derivProbeOffset ≈ 0.01.
		// When two adjacent vertices are closer than ~5× this radius,
		// the derivative stencils overlap — both vertices sample the
		// same surface patch, making the Jacobian entries correlated
		// and its determinant unreliable.  The resulting chainGeom/det
		// ratio can swing by 10-50× at nearby positions, causing
		// fireflies on displaced meshes with grazing-edge paths.
		{
			// Lowered from 0.05 to 0.01: for scenes with fine glass features
			// (torus tube radius 0.075, displaced mesh facets ~0.03), the
			// 0.05 threshold was rejecting a large fraction of legitimate
			// refraction chains — up to 85% of the rejections in the
			// torus_cross scene fell between 0.010 and 0.025.  These are
			// grazing paths through a thin tube that are entirely physical.
			// 0.01 still catches the truly pathological cases (e.g. < 1mm
			// separation where the derivative stencil radius 0.01 literally
			// overlaps the next vertex) without shedding good roots.
			const Scalar minReliableSegment = 0.01;
			const unsigned int k = static_cast<unsigned int>( specularChain.size() );
			bool tooShort = false;
			// tooShortIdx / tooShortDist exist only to feed the
			// SMS_TRACE_DIAGNOSTIC histogram + log.  Wrap declarations
			// AND assignments under the same #if so the variables don't
			// exist (and aren't unused-but-set) in production builds.
#if SMS_TRACE_DIAGNOSTIC
			unsigned int tooShortIdx = 0;
			Scalar tooShortDist = 0;
#endif
			for( unsigned int i = 0; i < k && !tooShort; i++ )
			{
				const Point3 prevPos = (i == 0) ? shadingPoint : specularChain[i-1].position;
				const Scalar d = Point3Ops::Distance( prevPos, specularChain[i].position );
				if( d < minReliableSegment ) {
					tooShort = true;
#if SMS_TRACE_DIAGNOSTIC
					tooShortIdx = i;
					tooShortDist = d;
#endif
				}
			}
			if( k > 0 && !tooShort )
			{
				const Scalar d = Point3Ops::Distance( specularChain[k-1].position, emitterPoint );
				if( d < minReliableSegment ) {
					tooShort = true;
#if SMS_TRACE_DIAGNOSTIC
					tooShortIdx = k;
					tooShortDist = d;
#endif
				}
			}
			if( tooShort )
			{
#if SMS_TRACE_DIAGNOSTIC
				g_solveShortSeg.fetch_add( 1, std::memory_order_relaxed );
				// Bucket the rejected distance to understand distribution
				static std::atomic<int> g_shortSegHist[5]{ {0}, {0}, {0}, {0}, {0} };
				int bucket = 0;
				if( tooShortDist < 0.001 ) bucket = 0;
				else if( tooShortDist < 0.005 ) bucket = 1;
				else if( tooShortDist < 0.010 ) bucket = 2;
				else if( tooShortDist < 0.025 ) bucket = 3;
				else bucket = 4;  // 0.025 - 0.05
				g_shortSegHist[bucket].fetch_add( 1, std::memory_order_relaxed );
				// Periodic dump
				if( (g_solveShortSeg.load() & 0x7fff) == 0 ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SHORTSEG_HIST: [<0.001]=%d  [0.001-0.005]=%d  [0.005-0.010]=%d  [0.010-0.025]=%d  [0.025-0.050]=%d",
						g_shortSegHist[0].load(), g_shortSegHist[1].load(),
						g_shortSegHist[2].load(), g_shortSegHist[3].load(),
						g_shortSegHist[4].load() );
				}
				if( traceSolve ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_SOLVE:  REJECTED by minReliableSegment: segment %u too short (d=%.4f < %.4f)",
						tooShortIdx, tooShortDist, minReliableSegment );
				}
#endif
#if SMS_SOLVE_DIAG
				g_solveDiag_shortSeg.fetch_add( 1, std::memory_order_relaxed );
#endif
				return result;
			}
		}

#if SMS_TRACE_DIAGNOSTIC
		g_solveOk.fetch_add( 1, std::memory_order_relaxed );
#endif
#if SMS_SOLVE_DIAG
		g_solveDiag_ok.fetch_add( 1, std::memory_order_relaxed );
#endif
		result.valid = true;
		result.specularChain = specularChain;

		// Evaluate throughput (Fresnel * Beer's law along the chain)
		result.contribution = EvaluateChainThroughput( shadingPoint, emitterPoint, specularChain );

		// Compute constraint Jacobian determinant |det(∂C/∂x_⊥)| for the
		// converged chain.  Uses the half-vector analytical Jacobian because
		// the contribution formula was derived for that formulation.
		// Newton uses angle-difference for convergence, but the measure
		// conversion factor must match the derivation.
		{
			const unsigned int k = static_cast<unsigned int>( specularChain.size() );

			std::vector<Scalar> diag, upper_blocks, lower_blocks;
			BuildJacobian( specularChain, shadingPoint, emitterPoint,
				diag, upper_blocks, lower_blocks );

			const Scalar detProduct = ComputeBlockTridiagonalDeterminant(
				diag, upper_blocks, lower_blocks, k );

			// Convert from parameter-space to tangent-plane world coordinates
			Scalar metricProduct = 1.0;
			for( unsigned int i = 0; i < k; i++ )
			{
				const Scalar dpduLen = Vector3Ops::Magnitude( specularChain[i].dpdu );
				const Scalar dpdvLen = Vector3Ops::Magnitude( specularChain[i].dpdv );
				if( dpduLen > NEARZERO && dpdvLen > NEARZERO )
				{
					metricProduct *= dpduLen * dpdvLen;
				}
			}

			result.jacobianDet = fabs( detProduct ) / fmax( metricProduct, 1e-20 );
			if( result.jacobianDet < 1e-20 )
				result.jacobianDet = 1e-20;
		}

		// PDF estimation
		if( !config.biased )
		{
			result.pdf = EstimatePDF( result, shadingPoint, emitterPoint, seedTemplate, sampler );
		}
		else
		{
			result.pdf = 1.0;
		}
	}
	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPoint
//
//   Standalone SMS evaluation at a single shading point.
//   Samples a light, builds a seed chain, solves the manifold,
//   evaluates the BSDF, and assembles the full contribution.
//
//   This is the reusable core that both BDPTIntegrator and
//   SMSShaderOp call.
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////
// ReversePhotonChainForSeed
//
//   Converts a photon's recorded chain (light->diffuse) into an
//   SMS seed chain (receiver->light) by reversing the order and
//   flipping isExiting on refraction-only vertices.  Re-queries
//   each material to recover attenuation / canRefract flags that
//   the photon record doesn't carry.
//
//   See header doc-comment for caveats around `etaI`/`etaT` (left
//   at default 1.0 because SMSPhoton storage doesn't snapshot the
//   IOR stack).
//////////////////////////////////////////////////////////////////////

unsigned int ManifoldSolver::ReversePhotonChainForSeed(
	const SMSPhoton& photon,
	std::vector<ManifoldVertex>& chain
	) const
{
	const unsigned int k = photon.chainLen;
	if( k == 0 || k > kSMSMaxPhotonChain ) {
		return 0;
	}

	chain.resize( k );
	IORStack queryIor( 1.0 );
	for( unsigned int i = 0; i < k; i++ )
	{
		const SMSPhotonChainVertex& pv = photon.chain[ k - 1 - i ];
		ManifoldVertex& mv = chain[i];
		mv.position    = pv.position;
		mv.normal      = pv.normal;
		mv.pObject     = pv.pObject;
		mv.pMaterial   = pv.pMaterial;
		mv.eta         = pv.eta;

		if( pv.pMaterial ) {
			Ray dummyRay( pv.position, pv.normal );
			RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
			rigLocal.bHit          = true;
			rigLocal.ptIntersection = pv.position;
			rigLocal.vNormal       = pv.normal;
			SpecularInfo spec = pv.pMaterial->GetSpecularInfo( rigLocal, queryIor );
			mv.attenuation = spec.attenuation;
			mv.canRefract  = spec.canRefract;
		} else {
			mv.attenuation = RISEPel( 1, 1, 1 );
			mv.canRefract  = true;
		}
		mv.isReflection = ( ( pv.flags & 0x2 ) != 0 );
		mv.isExiting    = mv.isReflection
			? ( ( pv.flags & 0x1 ) != 0 )
			: ( ( pv.flags & 0x1 ) == 0 );
		mv.valid = false;
	}
	return k;
}

//////////////////////////////////////////////////////////////////////
// ComputeTrialContribution
//
//   Per-trial contribution from a converged ManifoldResult.  See
//   the header doc-comment for the complete bail-out list.  Used by
//   both seeding modes and both photon-aided extension paths so the
//   contribution formula lives in exactly one place.
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ComputeTrialContribution(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const Vector3& woOutgoing,
	const IBSDF* pBSDF,
	const LightSample& lightSample,
	const ManifoldResult& mResult,
	const IRayCaster& caster,
	Vector3& outDir,
	RISEPel& outContribution
	) const
{
	outContribution = RISEPel( 0, 0, 0 );
	outDir = Vector3( 0, 0, 0 );

	if( !mResult.valid || mResult.specularChain.empty() || !pBSDF ) {
		return false;
	}

	// External-segment visibility (occluder between specular vertices,
	// or between last specular and the light).
	if( !CheckChainVisibility( pos, lightSample.position,
		mResult.specularChain, caster ) ) {
		return false;
	}

	// Direction from shading point toward first specular vertex.
	const ManifoldVertex& firstSpec = mResult.specularChain[0];
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distToFirstSpec = Vector3Ops::NormalizeMag( dirToFirstSpec );
	if( distToFirstSpec < 1e-8 ) {
		return false;
	}
	outDir = dirToFirstSpec;

	const Vector3 wiAtShading = dirToFirstSpec;

	// BSDF at shading point.
	Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
	RayIntersectionGeometric rig( evalRay, nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = pos;
	rig.vNormal = normal;
	rig.onb = onb;

	RISEPel fBSDF = pBSDF->value( wiAtShading, rig );
	if( ColorMath::MaxValue( fBSDF ) <= 0 ) return false;

	const Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
	if( cosAtShading <= 0 ) return false;

	// Light-side cos + Le for delta vs area lights.
	const ManifoldVertex& lastSpec = mResult.specularChain.back();
	Vector3 dirSpecToLight = Vector3Ops::mkVector3(
		lastSpec.position, lightSample.position );
	const Scalar distSpecToLight = Vector3Ops::NormalizeMag( dirSpecToLight );
	if( distSpecToLight < 1e-8 ) return false;

	Scalar cosAtLight;
	RISEPel actualLe;
	if( lightSample.isDelta ) {
		cosAtLight = 1.0;
		actualLe = lightSample.pLight
			? lightSample.pLight->emittedRadiance( dirSpecToLight )
			: lightSample.Le;
	} else {
		cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		if( cosAtLight <= 0 ) return false;
		actualLe = lightSample.Le;
	}
	(void)cosAtLight;   // Implicit in detDvDy via (I - wo⊗wo) projection.

	// SMS measure-conversion factor (G_x_v1 * |det dv1/dy|).
	Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
	if( distXtoV1 < 1e-8 ) return false;
	const Scalar cosV1atX = fabs( Vector3Ops::Dot( firstSpec.normal, dirXtoV1 ) );
	const Scalar G_x_v1 = cosV1atX / ( distXtoV1 * distXtoV1 );
	const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
		mResult.specularChain, pos, lightSample.position, lightSample.normal );
	const Scalar smsGeometric = G_x_v1 * detDvDy;

	outContribution = fBSDF
		* mResult.contribution
		* actualLe * cosAtShading * smsGeometric
		/ ( lightSample.pdfPosition * lightSample.pdfSelect );

	return true;
}

//////////////////////////////////////////////////////////////////////
// ComputeTrialContributionNM
//
//   Spectral counterpart of ComputeTrialContribution.  Same logic,
//   per-wavelength throughput via EvaluateChainThroughputNM and
//   per-wavelength BSDF via valueNM.  Le is luminance-projected
//   (the spectral path computes a single scalar value per wavelength
//   and luminance is the appropriate scalar projection of an
//   RGB Le).
//////////////////////////////////////////////////////////////////////

bool ManifoldSolver::ComputeTrialContributionNM(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const Vector3& woOutgoing,
	const IBSDF* pBSDF,
	const LightSample& lightSample,
	const ManifoldResult& mResult,
	const IRayCaster& caster,
	const Scalar nm,
	Vector3& outDir,
	Scalar& outContribution
	) const
{
	outContribution = 0;
	outDir = Vector3( 0, 0, 0 );

	if( !mResult.valid || mResult.specularChain.empty() || !pBSDF ) {
		return false;
	}

	if( !CheckChainVisibility( pos, lightSample.position,
		mResult.specularChain, caster ) ) {
		return false;
	}

	const ManifoldVertex& firstSpec = mResult.specularChain[0];
	Vector3 dirToFirstSpec = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distToFirstSpec = Vector3Ops::NormalizeMag( dirToFirstSpec );
	if( distToFirstSpec < 1e-8 ) {
		return false;
	}
	outDir = dirToFirstSpec;

	const Vector3 wiAtShading = dirToFirstSpec;

	Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
	RayIntersectionGeometric rig( evalRay, nullRasterizerState );
	rig.bHit = true;
	rig.ptIntersection = pos;
	rig.vNormal = normal;
	rig.onb = onb;

	Scalar fBSDF = pBSDF->valueNM( wiAtShading, rig, nm );
	if( fBSDF <= 0 ) return false;

	Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
	if( cosAtShading <= 0 ) return false;

	Scalar chainThroughput = EvaluateChainThroughputNM(
		pos, lightSample.position, mResult.specularChain, nm );

	const ManifoldVertex& lastSpec = mResult.specularChain.back();
	Vector3 dirSpecToLight = Vector3Ops::mkVector3(
		lastSpec.position, lightSample.position );
	const Scalar distSpecToLight = Vector3Ops::NormalizeMag( dirSpecToLight );
	if( distSpecToLight < 1e-8 ) return false;

	Scalar cosAtLight;
	Scalar Le;
	if( lightSample.isDelta ) {
		cosAtLight = 1.0;
		Le = lightSample.pLight
			? ColorMath::Luminance( lightSample.pLight->emittedRadiance( dirSpecToLight ) )
			: ColorMath::Luminance( lightSample.Le );
	} else {
		cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
		if( cosAtLight <= 0 ) return false;
		Le = ColorMath::Luminance( lightSample.Le );
	}
	(void)cosAtLight;

	Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpec.position, pos );
	const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
	if( distXtoV1 < 1e-8 ) return false;
	const Scalar cosV1atX = fabs( Vector3Ops::Dot( firstSpec.normal, dirXtoV1 ) );
	const Scalar G_x_v1 = cosV1atX / ( distXtoV1 * distXtoV1 );
	const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
		mResult.specularChain, pos, lightSample.position, lightSample.normal );
	const Scalar smsGeometric = G_x_v1 * detDvDy;
	const Scalar clampedGeometric = std::fmin( smsGeometric, config.maxGeometricTerm );

	outContribution = fBSDF
		* chainThroughput
		* Le * cosAtShading * clampedGeometric
		/ ( lightSample.pdfPosition * lightSample.pdfSelect );

	return true;
}

ManifoldSolver::SMSContribution ManifoldSolver::EvaluateAtShadingPoint(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler
	) const
{
	// Mitsuba-faithful uniform-on-shape seeding (opt-in via
	// `sms_seeding "uniform"`).  The two seeding strategies are
	// structurally different enough — per-caster iteration vs single
	// Snell-traced seed — that they live in separate functions.  See
	// `docs/SMS_UNIFORM_SEEDING_PLAN.md`.
	if( config.seedingMode == ManifoldSolverConfig::eSeedingUniform )
	{
		return EvaluateAtShadingPointUniform(
			pos, normal, onb, pMaterial, woOutgoing,
			scene, caster, sampler );
	}

	SMSContribution result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	// Use the caster's prepared LightSampler (which has the alias table
	// built during scene preparation) rather than our own uninitialized one.
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;

	// Sampler-dimension-drift firewall — see EvaluateAtShadingPointUniform
	// for the full rationale.  Variable-count internal work (multi-trial
	// loop, Solve→EstimatePDF) below uses `loopSampler`; the parent
	// sampler advances by exactly two dimensions for the seed.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Build seed chain toward the light sample.  When `branchingThreshold
	// < 1.0` use the Fresnel-branching builder so a Snell-traced shading-
	// point→light seed at a high-throughput sub-critical dielectric vertex
	// produces BOTH refraction and reflection continuation chains.  PT-
	// faithful single-split (matches `PathTracingIntegrator.cpp:1791`).
	// Each branched chain is consumed as a separate "base trial" in the
	// multi-trial loop below.
	std::vector<SeedChainResult> baseSeeds;
	if( config.branchingThreshold < 1.0 ) {
		BuildSeedChainBranching(
			pos, lightSample.position,
			scene, caster, loopSampler, baseSeeds );
	}
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = 0;
	if( !baseSeeds.empty() && !baseSeeds[0].chain.empty() ) {
		seedChain = baseSeeds[0].chain;
		chainLen = static_cast<unsigned int>( seedChain.size() );
	} else {
		// Branching off, or branching produced nothing usable — legacy
		// single-chain Snell-trace.
		baseSeeds.clear();
		chainLen = BuildSeedChain(
			pos, lightSample.position,
			scene, caster, seedChain );
		if( chainLen > 0 && !seedChain.empty() ) {
			SeedChainResult lone;
			lone.chain = seedChain;
			lone.proposalPdf = 1.0;
			baseSeeds.push_back( std::move( lone ) );
		}
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		// Fallback: trace along the surface normal.
		const Point3 normalTarget = Point3Ops::mkPoint3(
			pos, normal * 100.0 );
		chainLen = BuildSeedChain(
			pos, normalTarget,
			scene, caster, seedChain );
		if( chainLen > 0 && !seedChain.empty() ) {
			baseSeeds.clear();
			SeedChainResult lone;
			lone.chain = seedChain;
			lone.proposalPdf = 1.0;
			baseSeeds.push_back( std::move( lone ) );
		}
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		// Fallback: try tracing toward the midpoint between pos and
		// the light.  For tilted geometry, the specular object may lie
		// between the two endpoints at a position that neither the
		// light-direction nor the normal-direction ray can reach.
		const Point3 midpoint(
			(pos.x + lightSample.position.x) * 0.5,
			(pos.y + lightSample.position.y) * 0.5,
			(pos.z + lightSample.position.z) * 0.5 );
		const Point3 midTarget = Point3Ops::mkPoint3(
			pos, Vector3Ops::Normalize( Vector3Ops::mkVector3( midpoint, pos ) ) * 100.0 );
		chainLen = BuildSeedChain(
			pos, midTarget,
			scene, caster, seedChain );
		if( chainLen > 0 && !seedChain.empty() ) {
			baseSeeds.clear();
			SeedChainResult lone;
			lone.chain = seedChain;
			lone.proposalPdf = 1.0;
			baseSeeds.push_back( std::move( lone ) );
		}
	}

	// Pure-mirror caster supplemental seeds.
	//
	// The Snell-trace from shading-point toward light cannot find
	// pure-mirror multi-bounce chains (e.g. diacaustic) — the
	// reflection law that determines the true seed positions
	// doesn't lie on the shading→light line.  Snell-mode therefore
	// historically misses cardioid / diacaustic patterns that
	// uniform-mode finds easily by sampling directly on the mirror.
	//
	// Supplement: for each pure-mirror caster (canRefract=false),
	// draw one uniform-area sample on the caster and call
	// BuildSeedChainBranching with end = sampled point.  The
	// resulting chain naturally Snell-continues from the mirror hit
	// (so a 2-bounce diacaustic gets v0 from uniform sampling and
	// v1 from the reflected ray's next specular hit).  Append the
	// resulting chains to `baseSeeds` so the trial loop runs Newton
	// on them.
	//
	// Only applies to PURE MIRROR casters — dielectric reflection
	// branches are handled by the in-`BuildSeedChainBranching`
	// uniform-area resample above.  No-op when `mSpecularCasters`
	// is empty (rasterizer didn't populate it).
	if( config.branchingThreshold < 1.0 )
	{
		for( const IObject* pMirrorCaster : mSpecularCasters )
		{
			if( !pMirrorCaster ) continue;
			const IMaterial* pMat = pMirrorCaster->GetMaterial();
			if( !pMat ) continue;

			// Probe whether this caster is a pure mirror (canRefract=false).
			// A deterministic prand here is fine — the result is a binary
			// caster classification, not a sampling step.
			Point3 probePos;
			Vector3 probeNormal;
			Point2 probeUv;
			pMirrorCaster->UniformRandomPoint(
				&probePos, &probeNormal, &probeUv,
				Point3( 0.5, 0.5, 0.5 ) );
			Ray probeRay( probePos, probeNormal );
			RayIntersectionGeometric probeRig( probeRay, nullRasterizerState );
			probeRig.bHit          = true;
			probeRig.ptIntersection = probePos;
			probeRig.vNormal       = probeNormal;
			probeRig.ptCoord       = probeUv;
			IORStack probeIor( 1.0 );
			SpecularInfo probeSpec = pMat->GetSpecularInfo( probeRig, probeIor );
			if( probeSpec.canRefract ) {
				continue;   // dielectric — handled by BuildSeedChainBranching
			}

			// M independent uniform-area samples per pure-mirror caster
			// (matches uniform-mode's `multi_trials` budget on the
			// caster).  Mitsuba's biased SMS does this same M-trial
			// dedupe-and-sum on every caster shape; we only do it for
			// pure-mirror casters here because dielectrics are already
			// handled by BuildSeedChainBranching's per-vertex split.
			const unsigned int M = std::max( config.multiTrials, 1u );
			for( unsigned int m = 0; m < M; m++ )
			{
				Point3 sp;
				Vector3 sn;
				Point2 sc;
				pMirrorCaster->UniformRandomPoint(
					&sp, &sn, &sc,
					Point3( loopSampler.Get1D(),
					        loopSampler.Get1D(),
					        loopSampler.Get1D() ) );

				// Build chain: start → sampled mirror point.  Branching
				// continues the Snell-trace from the mirror, so
				// reflection-into-second-mirror chains (diacaustic) are
				// captured automatically as k=2+ chains.
				std::vector<SeedChainResult> mirrorChains;
				BuildSeedChainBranching(
					pos, sp, scene, caster, loopSampler, mirrorChains );

				for( SeedChainResult& mc : mirrorChains ) {
					if( !mc.chain.empty() && mc.chain[0].pObject == pMirrorCaster ) {
						baseSeeds.push_back( std::move( mc ) );
					}
				}
			}
		}
	}

	// ========================================================================
	// DIAGNOSTIC: conditional per-pixel trace of the SMS solver.
	// Gated by the file-scope SMS_TRACE_DIAGNOSTIC at the top; default 0.
	// ========================================================================
#if SMS_TRACE_DIAGNOSTIC
	static std::atomic<int> g_smsTraceCount{ 0 };
	// Wider trace window: the entire caustic region.  Gate to only fire
	// on samples that END UP failing (or on the first few successes for
	// comparison).  Uses a second static counter that only increments
	// when valid_trials==0, so we see what's actually broken.
	const bool traceHere =
		( std::fabs( pos.x ) < 0.3 ) &&
		( std::fabs( pos.z ) < 0.3 ) &&
		( pos.y >= -0.02 && pos.y <= 0.02 ) &&
		( g_smsTraceCount.fetch_add( 1, std::memory_order_relaxed ) < 40 );
	if( traceHere ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_TRACE: pos=(%.4f,%.4f,%.4f) baseSeedLen=%u baseSeedEmpty=%d",
			pos.x, pos.y, pos.z,
			chainLen, int( seedChain.empty() ) );
		for( std::size_t i = 0; i < seedChain.size(); i++ ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  v%zu obj=%p pos=(%.4f,%.4f,%.4f) n=(%.3f,%.3f,%.3f) eta=%.3f isExit=%d valid=%d",
				i, (void*)seedChain[i].pObject,
				seedChain[i].position.x, seedChain[i].position.y, seedChain[i].position.z,
				seedChain[i].normal.x, seedChain[i].normal.y, seedChain[i].normal.z,
				seedChain[i].eta, int( seedChain[i].isExiting ), int( seedChain[i].valid ) );
		}
	}
#endif

	if( baseSeeds.empty() ) {
		// No seed chains from any source — Snell-trace, fallbacks,
		// AND pure-mirror caster supplement all came up empty.
		// Pre-fix this check looked only at `seedChain` (the legacy
		// single-chain variable), which would early-return even when
		// the mirror-caster supplement had populated `baseSeeds` —
		// that broke diacaustic-style scenes where the Snell-trace
		// from shading-point to light doesn't hit the mirror at all
		// but uniform-area sampling on the mirror does find chains.
#if SMS_TRACE_DIAGNOSTIC
		if( traceHere ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  EARLY-RETURN: no seed chain" );
		}
#endif
		return result;
	}

	// Synchronise legacy state with the first available base seed.
	// `seedChain`, `chainLen`, `pFirstCaster`, and the surface-sample-
	// fallback heuristic below all read from the legacy variables;
	// repopulating them from baseSeeds[0] ensures the rest of the
	// function (multi-trial loop, photon-aided seeding, etc.) sees a
	// coherent state when the seed came from the supplement path.
	if( seedChain.empty() && !baseSeeds[0].chain.empty() ) {
		seedChain = baseSeeds[0].chain;
		chainLen = static_cast<unsigned int>( seedChain.size() );
	}
	(void)chainLen;

	// Multi-trial Specular Manifold Sampling with PHOTON-AIDED first-vertex
	// seeding (Zeltner et al. 2020 §4.3 biased estimator [Eq. 8] + photon-
	// driven manifold sampling à la Weisstein, Jhang, Chang.
	// "Photon-Driven Manifold Sampling." HPG 2024.  DOI 10.1145/3675375.
	// https://dl.acm.org/doi/10.1145/3675375).
	//
	// A single Snell-traced seed chain + one Newton solve finds ONE root of
	// the specular-manifold constraint.  On a smooth convex refractor that is
	// the unique physically admissible caustic path; on a bumpy / displaced
	// surface there are typically several local manifold solutions per
	// (shading-point, emitter) pair and each is a legitimate caustic path
	// contribution.
	//
	// Local perturbation (first-pass attempt) proved inadequate: Newton's
	// basin around the "direct" Snell root is large enough that perturbed
	// seeds always fall back.  Uniform surface sampling (second-pass) DID
	// uncover more roots but cost O(N) Newton solves per shading point per
	// sample, most of which land on back-faces or non-caustic regions and
	// return zero contribution.  Photon-aided seeding solves both problems:
	// a one-time light-pass at scene-prep time deposits photons on diffuse
	// surfaces AFTER traversing specular chains, and each deposit records
	// the photon's FIRST specular-caster entry.  A render-time fixed-radius
	// kd-tree query "photons whose landing is near this shading point"
	// returns entry points that are KNOWN to produce valid caustic paths
	// ending in the shading-point neighborhood.
	//
	// Seed construction per trial:
	//   - Trial 0: the deterministic Snell-traced baseSeedChain (for
	//     N==1 backward compatibility and as a "guaranteed" good seed).
	//   - Trials 1..N-1: pull up to N-1 photons from the photon map,
	//     filter to those whose entryObject matches the base chain's
	//     first caster, and rebuild the seed chain toward each photon's
	//     recorded entryPoint.  If the photon map is null (no photon pass
	//     was run — sms_photon_count == 0) or the query returns nothing,
	//     the remaining trials are no-ops.
	//
	// Dedupe by first-vertex world position, sum contributions of unique
	// converged chains.  No /N division: contributions per root are
	// well-defined; we're not computing E[f(random seed)] but rather
	// Σ_r f_r restricted to discovered roots.
	//
	// Unbiased mode (config.biased==false) still applies the Bernoulli 1/p
	// weighting per-trial before the dedupe.
	const unsigned int N = ( config.multiTrials > 0 ) ? config.multiTrials : 1;
	// Dedupe threshold: roots whose first specular vertices are within this
	// world-space distance are treated as the same root.  Use the config
	// uniquenessThreshold (default 1e-4) — tight enough that distinct
	// bumps on a displaced surface count as separate roots, loose enough
	// that Newton-iteration round-off doesn't declare the same root
	// different across trials.
	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;
	const std::vector<ManifoldVertex> baseSeedChain = seedChain;

	// The specular caster the base seed belongs to.  We restrict photon
	// seeds to the same object so the Newton solve operates on the chain
	// topology the caller built — crossing to a different caster would
	// require rebuilding the chain from scratch with potentially different
	// k, and changes the IOR stack semantics.
	const IObject* pFirstCaster = baseSeedChain[0].pObject;

	// All Fresnel-branched base seeds run UNCONDITIONALLY — each is a
	// distinct caustic chain that contributes its own energy.  The
	// `multi_trials = N` budget governs only the photon-aided trials
	// that run on top.  Total trials = numBaseSeeds + (N - 1)  where
	// the `-1` accounts for the legacy "trial 0 was the base seed"
	// semantic in the photon budget.
	//
	// Pre-fix bug: the loop iterated 0..N-1 and used baseSeeds[trial]
	// inside, so when N=1 (default `multi_trials=1`) only the first
	// branched chain ran and the rest of Branching's output was
	// silently dropped.  That produced the "diagnostic numbers move
	// but the rendered image doesn't change" symptom — the SMS
	// contribution was only ever counting one branch.
	const std::size_t numBaseSeeds = baseSeeds.size();

	// Pull photon-aided seeds from the rasterizer-owned photon map, if one
	// was built.  Query radius: the config-supplied value if positive,
	// otherwise the map's auto-computed value (bbox-diagonal * 0.01 —
	// analogous to VCM's merge-radius auto-fallback).
	//
	// Photon retrieval is independent of `multi_trials` (N): a user who
	// configures `sms_photon_count > 0` expects photons to be USED, even
	// at the default M=1.  The previous `N > 1` gate coupled the two
	// budgets so that scenes setting only `sms_photon_count` got an
	// empty photon-seed list at the consumption site below — photons
	// stored in the kd-tree but never reaching Newton.
	std::vector<SMSPhoton> photonSeeds;
	if( pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}
		if( r > 0 ) {
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );
		}
	}

	// Store the first specular vertex position of every accepted root so we
	// can dedupe later trials that converge to the same basin.  First-vertex
	// position is a unique enough identifier in practice: distinct roots of
	// the manifold constraint have distinct entry points on the specular
	// surface.
	std::vector<Point3> acceptedRootPositions;
	// Parallel vector of isReflection bitmasks (one bit per chain
	// vertex, lsb = vertex 0).  Two trials with the SAME first-vertex
	// position but DIFFERENT Fresnel-branch patterns (e.g. one chain
	// is refract-refract, sibling is refract-reflect) are distinct
	// roots — without this, the Option-C branching's pair of
	// chain-variants gets collapsed by first-vertex dedupe alone.
	std::vector<unsigned long long> acceptedRootReflectMasks;
	auto buildReflectMask = []( const std::vector<ManifoldVertex>& ch ) -> unsigned long long {
		unsigned long long mask = 0;
		const std::size_t k = std::min<std::size_t>( ch.size(), 64 );
		for( std::size_t i = 0; i < k; i++ ) {
			if( ch[i].isReflection ) mask |= ( 1ull << i );
		}
		return mask;
	};
	RISEPel totalContribution( 0, 0, 0 );
	unsigned int validTrials = 0;

	// Track per-trial geometric term (G_x_v1 × |det dv/dy|) UNCLAMPED so
	// we can apply the config.maxGeometricTerm cap to the SUM across all
	// accepted distinct preimages, instead of to each one independently.
	//
	// Rationale: fold caustics produce multiple preimages at the same
	// pixel, each with a near-singular Jacobian.  A per-trial clamp then
	// saturates every preimage at the cap and sums them — so the clamp
	// scales with the number of trials instead of bounding the pixel.
	// Capping the sum is the well-posed version of the same safeguard:
	// "don't let the sum of specular-transport factors exceed a maximum
	// physical density at any one receiver point".
	std::vector<Scalar> acceptedGeoTerm;   ///< unclamped G·|det|
	std::vector<RISEPel> acceptedPreGeo;   ///< trialContribution / smsGeoUsed

	// photonCursor walks the photonSeeds list (filtering by entryObject as
	// we go) so trial indices don't directly map to photon indices.
	std::size_t photonCursor = 0;

	// Total trials: all branched base seeds + the photon budget + the
	// extra multi-trial budget.  When photons are present we want EVERY
	// queried photon to drive a Newton trial; when they aren't, we fall
	// back to N-1 extra-trial slots that the surface-sample fallback can
	// fill (k=1 mirror-chain case).  Without `+ photonSeeds.size()` here,
	// the trial loop's `trial >= numBaseSeeds` branch never sees most of
	// the queried photons even when QuerySeeds returned hundreds.
	//
	// Legacy parity: with `branchingThreshold == 1.0` and no photon map,
	// `numBaseSeeds == 1` and `photonSeeds.size() == 0`, so totalTrials
	// degenerates to N — the original behaviour.
	const unsigned int totalTrials = static_cast<unsigned int>( numBaseSeeds )
		+ static_cast<unsigned int>( photonSeeds.size() )
		+ ( N > 0 ? N - 1 : 0 );

	// Surface-sampling fallback for purely-reflective single-vertex chains.
	//
	// For a k=1 chain on a perfect-reflector, BuildSeedChain shoots a ray
	// from `pos` toward `lightSample.position` and records its first
	// specular hit.  That hit lies on the straight line shade↔light by
	// construction, so wi (mirror→shade) and wo (mirror→light) are exactly
	// anti-parallel and the half-vector h = wi + wo is identically zero
	// — the constraint is degenerate (||C|| = √2 from the hLen<NEARZERO
	// fallback in EvaluateConstraint), and Newton has no descent direction.
	//
	// Refraction doesn't have this problem because Snell's law bends the
	// ray inside the medium, so a typical refractive caustic chain is k=2
	// (entry+exit) and the second vertex lies OFF the shade↔light line.
	// Reflection produces no such bend, so k=1 reflection always degenerates.
	//
	// The principled remedy: seed each trial by sampling a uniform random
	// point on the reflective caster's surface.  On a smooth curved mirror
	// the basin of attraction around each reflection root is wide, so most
	// surface samples Newton-converge to a valid root — for the diacaustic
	// (a curved tube), the heart-shaped cardioid roots are reachable from
	// generic surface samples.  Photon-aided seeding (trials with photons
	// available) takes precedence; surface sampling fills the remainder.
	const bool surfaceSampleReflectionFallback =
		( baseSeedChain.size() == 1 ) &&
		( !baseSeedChain[0].canRefract ) &&
		( pFirstCaster != nullptr );

	for( unsigned int trial = 0; trial < totalTrials; trial++ )
	{
		std::vector<ManifoldVertex> trialSeed = baseSeedChain;
		Scalar trialProposalPdf = 1.0;
		bool useSurfaceSample = false;

		// Trials 0..numBaseSeeds-1 consume the Fresnel-branched base
		// seeds (multiple chains when branching fires at trial 0).
		// Each base seed carries its own `proposalPdf`; the contribution
		// formula divides by it later to absorb the RR weighting that
		// the chain throughput's Fresnel factor cancels.
		// Subsequent trials consume photon seeds; if photons run out,
		// the surface-sample fallback fires for k=1 mirror chains.
		//
		// NOTE on entryObject: the photon's entryObject is the FIRST
		// caster the LIGHT'S ray hit — for a k>1 chain (e.g. a slab:
		// light hits top plane, then bottom, then floor) that's the caster
		// NEAREST the light.  SMS's chain is built in the opposite
		// direction (shading point -> light), so its first caster is the
		// one NEAREST the receiver.  Those are DIFFERENT objects for any
		// k>1 chain.  We therefore do NOT filter by caster identity; the
		// photon's entryPoint is useful simply as an aim-point: a ray from
		// the shading point toward that world position crosses the chain
		// in the right direction regardless of which side of the chain
		// the photon happened to enter from.
		if( trial < numBaseSeeds )
		{
			trialSeed = baseSeeds[trial].chain;
			trialProposalPdf = baseSeeds[trial].proposalPdf;
			// Surface-sample fallback only fires on the very first base
			// seed when it's the degenerate k=1 mirror case.
			if( trial == 0 && surfaceSampleReflectionFallback ) {
				useSurfaceSample = true;
				trialProposalPdf = 1.0;   // surface-sample = uniform proposal, no Fresnel RR
			}
		}
		else
		{
			if( photonCursor >= photonSeeds.size() ) {
				if( surfaceSampleReflectionFallback ) {
					useSurfaceSample = true;
				} else {
					continue;  // no more photon seeds this trial round
				}
			}
			else
			{
			const SMSPhoton& ph = photonSeeds[photonCursor];
			photonCursor++;

			// Construct the SMS seed chain directly from the photon's
			// recorded chain — this is what fixes the topology-loss
			// problem.  The photon's chain is in photon-direction order
			// (v[0] nearest light, v[k-1] nearest diffuse); SMS walks
			// receiver→light, so we REVERSE the order and FLIP each
			// vertex's exit flag.  Re-tracing via BuildSeedChain from
			// the shading point would force the chain topology back to
			// whatever Snell's law dictates for a straight-line seed
			// target — typically k=2 even when the true caustic path is
			// k=4.  Using the photon's recorded chain preserves the
			// topology the photon itself followed.
			const unsigned int k = ph.chainLen;
			if( k == 0 || k > kSMSMaxPhotonChain ) {
				continue;
			}

			std::vector<ManifoldVertex> newChain( k );
			IORStack queryIor( 1.0 );
			for( unsigned int i = 0; i < k; i++ )
			{
				const SMSPhotonChainVertex& pv = ph.chain[ k - 1 - i ];
				ManifoldVertex& mv = newChain[i];
				mv.position    = pv.position;
				mv.normal      = pv.normal;
				mv.pObject     = pv.pObject;
				mv.pMaterial   = pv.pMaterial;
				mv.eta         = pv.eta;
				// Query the material at this vertex for its actual specular
				// attenuation (a.k.a. refractance color for dielectrics,
				// reflectance for mirrors).  The previous hardcoded white
				// was correct for the typical white-glass test scenes but
				// silently dropped material colour for any coloured specular
				// — a colour / absorption bias in the caustic that only
				// photon-aided multi-trial SMS would expose as an occasional
				// bright-channel-mismatched trial.  BuildSeedChain already
				// does this lookup at line 2303; parity restores it here.
				if( pv.pMaterial ) {
					Ray dummyRay( pv.position, pv.normal );
					RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
					rigLocal.bHit = true;
					rigLocal.ptIntersection = pv.position;
					rigLocal.vNormal = pv.normal;
					SpecularInfo spec = pv.pMaterial->GetSpecularInfo( rigLocal, queryIor );
					mv.attenuation = spec.attenuation;
					mv.canRefract  = spec.canRefract;
				} else {
					mv.attenuation = RISEPel( 1, 1, 1 );
					mv.canRefract  = true;   // safe default: dielectric Fresnel path
				}
				// Chain-vertex semantics recovered from the photon record:
				//   flags bit 0 = photon-direction isExiting (refractions only)
				//   flags bit 1 = isReflection (scatter picked reflection, not
				//                  refraction — no medium change)
				// Photon-direction isExiting flag is FLIPPED: what was
				// ENTERING for the photon is EXITING for the receiver
				// ray going the other way through the same surface.
				// Reflection vertices are direction-independent so no flip.
				mv.isReflection = ( ( pv.flags & 0x2 ) != 0 );
				mv.isExiting    = mv.isReflection
				                ? ( ( pv.flags & 0x1 ) != 0 )    // preserve
				                : ( ( pv.flags & 0x1 ) == 0 );   // flip
				mv.valid     = false;        // derivatives will be re-computed by Solve
				// SMS photon storage doesn't carry the IOR-stack
				// snapshot at each vertex — only `eta` (the surface
				// material's IOR).  mv.etaI and mv.etaT stay at the
				// default 1.0 here, and downstream half-vector /
				// Fresnel math falls back to the air-on-other-side
				// assumption via GetEffectiveEtas.  Correct for
				// single-dielectric-in-air photon caustics; WRONG
				// for nested-dielectric photon-seeded chains.  See
				// the matching comment in EvaluateAtShadingPointNM
				// for the full rationale and the path to fix it
				// (extend SMSPhotonChainVertex storage at emission).
			}
			trialSeed = newChain;
			}  // end photon-aided branch (else of "no more photons")
		}

		// Build a single-vertex seed by uniform-sampling the reflective
		// caster's surface.  Inherits material data from the original
		// baseSeedChain[0]; only position/normal change.  Newton then
		// walks from this random surface point to a true reflection root.
		if( useSurfaceSample )
		{
			Point3 sp;
			Vector3 sn;
			Point2 sc;
			pFirstCaster->UniformRandomPoint(
				&sp, &sn, &sc,
				Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
			ManifoldVertex mv = baseSeedChain[0];
			mv.position = sp;
			mv.normal   = sn;
			mv.uv       = sc;
			mv.dpdu = Vector3(0,0,0);  // Solve will compute via ComputeVertexDerivatives
			mv.dpdv = Vector3(0,0,0);
			mv.dndu = Vector3(0,0,0);
			mv.dndv = Vector3(0,0,0);
			mv.valid = false;
			trialSeed.clear();
			trialSeed.push_back( mv );
		}

		ManifoldResult mResult = Solve(
			pos, normal,
			lightSample.position, lightSample.normal,
			trialSeed, loopSampler );

#if SMS_TRACE_DIAGNOSTIC
		if( traceHere ) {
			if( mResult.valid ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_TRACE:  trial=%u Solve VALID  v0=(%.4f,%.4f,%.4f) jacDet=%.3e contrib=(%.4f,%.4f,%.4f) pdf=%.4f",
					trial,
					mResult.specularChain[0].position.x,
					mResult.specularChain[0].position.y,
					mResult.specularChain[0].position.z,
					mResult.jacobianDet,
					mResult.contribution.r, mResult.contribution.g, mResult.contribution.b,
					mResult.pdf );
			} else {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_TRACE:  trial=%u Solve INVALID", trial );
			}
		}
#endif

		if( !mResult.valid ) continue;

		// Dedupe by (first-vertex world position, isReflection bitmask
		// over all chain vertices).  First-vertex position alone was
		// sufficient before Option-C Fresnel branching landed; with
		// branching, two trials can share a first-vertex but flip
		// reflect/refract on a later vertex — those are physically
		// distinct caustic paths and must contribute separately.
		const Point3& firstPos = mResult.specularChain[0].position;
		const unsigned long long reflectMask =
			buildReflectMask( mResult.specularChain );
		bool duplicate = false;
		for( unsigned int r = 0; r < acceptedRootPositions.size(); r++ )
		{
			if( reflectMask == acceptedRootReflectMasks[r] &&
				Point3Ops::Distance( firstPos, acceptedRootPositions[r] ) < dedupeThr )
			{
				duplicate = true;
				break;
			}
		}
#if SMS_TRACE_DIAGNOSTIC
		if( traceHere && duplicate ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  trial=%u DUPLICATE -> skipped", trial );
		}
#endif
		if( duplicate ) continue;

		// Visibility: check external segments of the specular chain
		const bool visible = CheckChainVisibility( pos, lightSample.position,
			mResult.specularChain, caster );
#if SMS_TRACE_DIAGNOSTIC
		{
			static std::atomic<int> g_visTotal{ 0 };
			static std::atomic<int> g_visBlocked{ 0 };
			const int tot = g_visTotal.fetch_add( 1, std::memory_order_relaxed );
			if( !visible ) g_visBlocked.fetch_add( 1, std::memory_order_relaxed );
			if( (tot & 0x3ffff) == 0 && tot > 0 ) {
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_VIS_STATS: total=%d blocked=%d (%.2f%%)",
					tot, g_visBlocked.load(),
					100.0 * g_visBlocked.load() / tot );
			}
		}
#endif
#if SMS_TRACE_DIAGNOSTIC
		if( traceHere && !visible ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  trial=%u VISIBILITY FAIL", trial );
		}
#endif
		if( !visible ) continue;

		// Direction from shading point toward first specular vertex
		const ManifoldVertex& firstSpec = mResult.specularChain[0];
		Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
			firstSpec.position, pos );
		Scalar distToFirstSpec = Vector3Ops::Magnitude( dirToFirstSpec );
		if( distToFirstSpec < 1e-8 ) continue;
		dirToFirstSpec = dirToFirstSpec * (1.0 / distToFirstSpec);

		const Vector3 wiAtShading = dirToFirstSpec;

		// Evaluate BSDF at shading point
		// RISE convention: ri.ray.Dir() is toward the surface (negate woOutgoing)
		Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
		RayIntersectionGeometric rig( evalRay, nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = pos;
		rig.vNormal = normal;
		rig.onb = onb;

		RISEPel fBSDF = pBSDF->value( wiAtShading, rig );
		if( ColorMath::MaxValue( fBSDF ) <= 0 ) continue;

		// Cosine at shading point: uses the actual direction toward the
		// first specular vertex (where light arrives from after refraction).
		const Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
		if( cosAtShading <= 0 ) continue;

		// Direction and distance from last specular vertex to light
		// (needed for cosine evaluation at the light surface).
		const ManifoldVertex& lastSpec = mResult.specularChain.back();
		Vector3 dirSpecToLight = Vector3Ops::mkVector3(
			lastSpec.position, lightSample.position );
		Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
		if( distSpecToLight < 1e-8 ) continue;
		dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

		// For delta-position lights (point/spot), there is no surface at the
		// light — the geometric coupling has no cosine term.  The emitted
		// radiance must also be re-evaluated for the actual direction from the
		// light toward the last specular vertex, since the LightSample's Le
		// was evaluated at a random photon direction.
		Scalar cosAtLight;
		RISEPel actualLe;
		if( lightSample.isDelta ) {
			cosAtLight = 1.0;
			if( lightSample.pLight ) {
				actualLe = lightSample.pLight->emittedRadiance( dirSpecToLight );
			} else {
				actualLe = lightSample.Le;
			}
		} else {
			cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
			if( cosAtLight <= 0 ) continue;
			actualLe = lightSample.Le;
		}

		// SMS measure-conversion via implicit function theorem.
		//
		// For a k-vertex specular chain x -> v_1 -> ... -> v_k -> y, we
		// sample the light endpoint y on its area and need the Jacobian
		// |dω_x / dA_y|.  Following Zeltner et al. 2020, applying the
		// implicit function theorem to C(v_1, ..., v_k, y) = 0 gives:
		//
		//   |dω_x / dA_y| = G(x, v_1) · |det(δv_1_⊥ / δy_⊥)|
		//
		// where G(x, v_1) = cos(θ_v1_at_x) / dist²(x, v_1) is the standard
		// solid-angle-from-area factor at the first specular vertex, and
		// |det(δv_1/δy)| is the 2x2 Jacobian computed by solving the
		// block-tridiagonal system J_v · δv = -J_y · δy.
		//
		// For k=1 this reduces to G(x, v_1) · |det(∂C/∂y)| / |det(∂C/∂v_1)|,
		// which matches the analytical apparent-depth formula
		// |dω_x/dA_y| = 1/(d_1 + n·d_2)² for a flat refractor at normal
		// incidence.  (d_1 = x-to-v_1, d_2 = v_1-to-y, n = relative IOR.)
		//
		// cos(θ_y) is IMPLICIT in |det(∂C/∂y)| — y-tangent perturbations
		// project onto the wo direction via (I - wo⊗wo), which naturally
		// includes the cos_y factor.  Do NOT multiply by cosAtLight again.
		//
		// The previous formula used 1/dist²(v_k, y) in place of |det(∂C/∂y)|
		// and was off by an eta-dependent factor (e.g. 1.44x for ior=2.2 at
		// normal incidence), producing systematically over-bright caustics
		// except for ior→1.
		const ManifoldVertex& firstSpecForG = mResult.specularChain[0];
		Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpecForG.position, pos );
		const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
		if( distXtoV1 < 1e-8 ) continue;
		const Scalar cosV1atX = fabs( Vector3Ops::Dot( firstSpecForG.normal, dirXtoV1 ) );
		const Scalar G_x_v1 = cosV1atX / (distXtoV1 * distXtoV1);

		const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
			mResult.specularChain, pos, lightSample.position, lightSample.normal );

		const Scalar smsGeometric = G_x_v1 * detDvDy;

		// Defer clamping: we cap the SUM across distinct preimages, not
		// each preimage individually, so fold-caustic pixels that produce
		// many near-singular Jacobians don't accumulate clamp×N fireflies.
		// The per-trial contribution carries the RAW smsGeometric; the
		// final scale-down (if any) is applied post-loop below.
		const Scalar clampedGeometric = smsGeometric;

		RISEPel trialContribution = fBSDF
			* mResult.contribution
			* actualLe * cosAtShading * clampedGeometric
			/ (lightSample.pdfPosition * lightSample.pdfSelect);

		// Silence unused-variable warning if cosAtLight ends up unused;
		// keep its computation above for backface culling (non-delta lights).
		(void)cosAtLight;

		// Fold the SMS sampler's own PDF into the per-trial contribution.
		//   Biased: mResult.pdf == 1.0, no change.
		//   Unbiased: mResult.pdf carries the Bernoulli-estimated 1/p, so
		//   dividing here yields a correctly re-weighted unbiased estimate
		//   for this trial.
		if( mResult.pdf > 1e-20 )
		{
			trialContribution = trialContribution / mResult.pdf;
		}

		// Fresnel-branching proposal-pdf division (Option C — snell-mode
		// trial 0 uses BuildSeedChainBranching which RR-picks one branch
		// at sub-critical dielectric vertices when below threshold; the
		// chain's `proposalPdf` is the product of those RR-pick weights).
		// Dividing here cancels the Fr / (1-Fr) factor that
		// `EvaluateChainThroughput` applies on the picked branch, leaving
		// an unbiased estimator.  For non-branched trials (photons,
		// surface-sample fallback) `trialProposalPdf` stays 1.0 and the
		// division is a no-op.
		if( trialProposalPdf > 1e-20 && trialProposalPdf != 1.0 )
		{
			trialContribution = trialContribution * ( 1.0 / trialProposalPdf );
		}

#if SMS_TRACE_DIAGNOSTIC
		if( traceHere ) {
			GlobalLog()->PrintEx( eLog_Event,
				"SMS_TRACE:  trial=%u ACCEPTED  G_x_v1=%.3e detDvDy=%.3e smsGeo=%.3e clamp=%.2f "
				"cosAtShade=%.3f Le=(%.3f,%.3f,%.3f) fBSDF=(%.3e,%.3e,%.3e) trialContrib=(%.3e,%.3e,%.3e)",
				trial, G_x_v1, detDvDy, smsGeometric, config.maxGeometricTerm,
				cosAtShading,
				actualLe.r, actualLe.g, actualLe.b,
				fBSDF.r, fBSDF.g, fBSDF.b,
				trialContribution.r, trialContribution.g, trialContribution.b );
		}
#endif

#if SMS_TRACE_DIAGNOSTIC
		// Firefly-triggered diagnostic: log when per-trial contribution
		// exceeds 5.0 luminance, regardless of positional gate, capped
		// by an atomic counter to avoid log flooding.  This caught the
		// reflection-vs-refraction photon-chain-flag bug — chainLen=1
		// "refraction" chains whose photon had actually picked the
		// Fresnel reflection branch carry a spurious n²·T ≈ 4.16
		// throughput factor.
		{
			const Scalar trialLum = 0.2126 * trialContribution.r
			                      + 0.7152 * trialContribution.g
			                      + 0.0722 * trialContribution.b;
			if( trialLum > 5.0 ) {
				static std::atomic<int> g_smsFireflyCount{ 0 };
				const int idx = g_smsFireflyCount.fetch_add( 1, std::memory_order_relaxed );
				if( idx < 2000 ) {
					GlobalLog()->PrintEx( eLog_Event,
						"SMS_FIREFLY[%d]: trial=%u pos=(%.4f,%.4f,%.4f) lightP=(%.4f,%.4f,%.4f) "
						"chainLen=%zu G_x_v1=%.3e detDvDy=%.3e smsGeoPre=%.3e smsGeoPost=%.3e "
						"distX_V1=%.4f cosV1atX=%.3f cosAtShade=%.3f "
						"fBSDF=(%.3e,%.3e,%.3e) attenChain=(%.3e,%.3e,%.3e) "
						"Le=(%.1f,%.1f,%.1f) mPdf=%.3e lightPdf=%.3e trialLum=%.3f "
						"trialContrib=(%.3e,%.3e,%.3e)",
						idx, trial,
						pos.x, pos.y, pos.z,
						lightSample.position.x, lightSample.position.y, lightSample.position.z,
						mResult.specularChain.size(),
						G_x_v1, detDvDy, smsGeometric, clampedGeometric,
						distXtoV1, cosV1atX, cosAtShading,
						fBSDF.r, fBSDF.g, fBSDF.b,
						mResult.contribution.r, mResult.contribution.g, mResult.contribution.b,
						actualLe.r, actualLe.g, actualLe.b,
						mResult.pdf, lightSample.pdfPosition,
						trialLum,
						trialContribution.r, trialContribution.g, trialContribution.b );
					for( std::size_t vi = 0; vi < mResult.specularChain.size(); vi++ ) {
						const ManifoldVertex& mv = mResult.specularChain[vi];
						GlobalLog()->PrintEx( eLog_Event,
							"SMS_FIREFLY[%d]:   v%zu obj=%p mat=%p pos=(%.4f,%.4f,%.4f) "
							"n=(%.3f,%.3f,%.3f) eta=%.3f isExit=%d isRefl=%d atten=(%.3f,%.3f,%.3f)",
							idx, vi, (void*)mv.pObject, (void*)mv.pMaterial,
							mv.position.x, mv.position.y, mv.position.z,
							mv.normal.x, mv.normal.y, mv.normal.z,
							mv.eta, int(mv.isExiting), int(mv.isReflection),
							mv.attenuation.r, mv.attenuation.g, mv.attenuation.b );
					}
				}
			}
		}
#endif

		totalContribution = totalContribution + trialContribution;
		acceptedGeoTerm.push_back( smsGeometric );
		acceptedPreGeo.push_back( trialContribution * ( smsGeometric > 1e-20 ? (1.0 / smsGeometric) : 0.0 ) );
		acceptedRootPositions.push_back( firstPos );
		acceptedRootReflectMasks.push_back( reflectMask );
		validTrials++;
	}

	// ------------------------------------------------------------------
	// Sum-level clamp: if the total geometric term across accepted
	// preimages exceeds config.maxGeometricTerm, scale all per-trial
	// contributions down so the sum equals the cap.  This preserves
	// the RELATIVE weighting between preimages (so the caustic "shape"
	// is still resolved) while bounding the pixel from fold-caustic
	// firefly accumulation.
	// ------------------------------------------------------------------
	if( validTrials > 0 && config.maxGeometricTerm > 0 )
	{
		Scalar sumGeoTerm = 0;
		for( Scalar g : acceptedGeoTerm ) sumGeoTerm += g;

		if( sumGeoTerm > config.maxGeometricTerm )
		{
			const Scalar scale = config.maxGeometricTerm / sumGeoTerm;
			totalContribution = RISEPel( 0, 0, 0 );
			for( std::size_t i = 0; i < acceptedPreGeo.size(); i++ )
			{
				totalContribution = totalContribution +
					acceptedPreGeo[i] * ( acceptedGeoTerm[i] * scale );
			}
		}
	}

#if SMS_TRACE_DIAGNOSTIC
	if( traceHere ) {
		GlobalLog()->PrintEx( eLog_Event,
			"SMS_TRACE: FINAL validTrials=%u acceptedRoots=%zu totalContrib=(%.4e,%.4e,%.4e)",
			validTrials, acceptedRootPositions.size(),
			totalContribution.r, totalContribution.g, totalContribution.b );
	}

	// Post-clamp firefly diagnostic: log when the FINAL per-SMS-call
	// contribution exceeds 5.0 luminance.  This catches any remaining
	// SMS-side fireflies that survive the sum-level clamp.
	{
		const Scalar totalLum = 0.2126 * totalContribution.r
		                      + 0.7152 * totalContribution.g
		                      + 0.0722 * totalContribution.b;
		if( totalLum > 5.0 ) {
			static std::atomic<int> g_smsFinalFF{ 0 };
			const int idx = g_smsFinalFF.fetch_add( 1, std::memory_order_relaxed );
			if( idx < 400 ) {
				Scalar sumGeo = 0;
				for( Scalar g : acceptedGeoTerm ) sumGeo += g;
				GlobalLog()->PrintEx( eLog_Event,
					"SMS_FINAL_FF[%d]: pos=(%.4f,%.4f,%.4f) validTrials=%u "
					"sumGeoPre=%.3e cap=%.2f totalLum=%.3f totalContrib=(%.3e,%.3e,%.3e)",
					idx, pos.x, pos.y, pos.z, validTrials,
					sumGeo, config.maxGeometricTerm, totalLum,
					totalContribution.r, totalContribution.g, totalContribution.b );
			}
		}
	}
#endif

	if( validTrials == 0 ) return result;

	// No /N division — we've deduped by root so totalContribution is the
	// sum Σ_r f_r over the distinct roots discovered by the N trials.
	// This is what we want for multi-root caustics; in the single-root
	// case (smooth specular surface, N=1) it reduces exactly to the
	// pre-multi-trial single-solve contribution.
	result.contribution = totalContribution;
	result.misWeight = 1.0;  // PDF weighting already folded into contribution
	result.valid = true;

	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointUniform
//
//   Mitsuba-faithful uniform-on-shape SMS seeding.  Iterates the
//   cached `mSpecularCasters`; per caster, draws a uniform-area
//   sample on the surface, builds a Snell-traced seed chain from
//   the shading point through the sampled point, and (if the chain
//   converges) accumulates the per-caster contribution.  Sums one
//   independent estimate per caster shape — no MIS over caster
//   choice (manifold_ss.cpp lines 32-42 explicitly disclaim
//   shape-picking).
//
//   Phase 4 of the Mitsuba-faithful SMS port: single-trial, biased
//   semantics (no Bernoulli).  Phase 5 will layer the geometric
//   `K = first-success-index` estimator on top, and Phase 7 the
//   photon-aided trial integration.
//
//   See `docs/SMS_UNIFORM_SEEDING_PLAN.md` for the full plan.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContribution ManifoldSolver::EvaluateAtShadingPointUniform(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler
	) const
{
	SMSContribution result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	if( mSpecularCasters.empty() ) {
		// Mitsuba-style uniform mode requires a caster list; without
		// it we have no surfaces to sample on.  Caller already enabled
		// `sms_seeding "uniform"` so silent return-zero is the right
		// behaviour (matches Mitsuba's "no caustic_caster shape" case).
		return result;
	}

	// Light sampling — same as snell mode.
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) ) {
		return result;
	}

	// Sampler-dimension-drift firewall: variable-count internal work
	// (M-trial loop, Bernoulli K-loop, Solve→EstimatePDF) below uses
	// `loopSampler`; the parent sampler advances by a fixed two
	// dimensions for the SMSLoopSampler seed, leaving its LDS stream
	// predictable for downstream callers.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Cross-trial dedupe key: (first-vertex world-space position,
	// chain length).  The previous direction-only key was prone to
	// false-positive merges of distinct k-vs-k+2 chain topologies
	// that happened to share the same shading-point→first-vertex
	// direction.  See the planning doc for the analysis.
	struct RootKey { Point3 pos; unsigned int chainLen; };
	std::vector<RootKey> acceptedRoots;
	acceptedRoots.reserve( mSpecularCasters.size() * 2 + 16 );

	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;

	auto isDuplicate = [&]( const Point3& fp, unsigned int cl ) -> bool {
		for( const RootKey& rk : acceptedRoots ) {
			if( rk.chainLen == cl &&
				Point3Ops::Distance( rk.pos, fp ) < dedupeThr ) {
				return true;
			}
		}
		return false;
	};

	// Helper: uniform-area sample on `pCasterObj`, build a Snell-traced
	// seed chain via BuildSeedChain.  Rejects when the visibility ray
	// doesn't actually hit the sampled caster as the FIRST specular
	// hit (matches Mitsuba's `si_init.shape != shape` rejection).
	// Single-chain seed (refraction-only, deterministic).  Used by the
	// unbiased Bernoulli branch where the geometric estimator requires
	// the trial-proposal distribution to match the main solve's.
	auto buildSeedFromUniformOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<ManifoldVertex>& trialSeed) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		trialSeed.clear();
		const unsigned int chainLen = BuildSeedChain(
			pos, sp, scene, caster, trialSeed );
		if( chainLen == 0 || trialSeed.empty() ) return false;
		if( trialSeed[0].pObject != pCasterObj ) return false;
		return true;
	};

	// Branched seed: uniform-area sample, then `BuildSeedChainBranching`
	// produces one chain per Fresnel-decision combination (gated by
	// `config.branchingThreshold`).  Caller divides each chain's
	// contribution by `proposalPdf` to undo the RR weighting that
	// `EvaluateChainThroughput`'s Fresnel factor cancels.
	auto buildBranchedSeedsOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<SeedChainResult>& outSeeds) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		outSeeds.clear();
		BuildSeedChainBranching( pos, sp, scene, caster, loopSampler, outSeeds );
		// Filter to chains whose first specular hit is the sampled caster
		// (matches Mitsuba's `si_init.shape != shape` rejection).
		outSeeds.erase(
			std::remove_if( outSeeds.begin(), outSeeds.end(),
				[&]( const SeedChainResult& r ) {
					return r.chain.empty() || r.chain[0].pObject != pCasterObj;
				} ),
			outSeeds.end() );
		return !outSeeds.empty();
	};

	RISEPel totalContribution( 0, 0, 0 );
	unsigned int validContributions = 0;

	// Per-caster Mitsuba-style sum (manifold_ss / manifold_ms iterate
	// every caster and accrue one independent estimate per shape).
	for( const IObject* pCasterObj : mSpecularCasters )
	{
		if( !pCasterObj ) continue;

		if( config.biased )
		{
			// M-trial biased mode (Zeltner 2020 §4.3 Algorithm 3 / Eq. 8;
			// Mitsuba `manifold_ss.cpp:142-197`).  Per caster, run M
			// independent uniform-area samples; accept each unique
			// converged solution; sum unweighted.  Cross-caster dedupe
			// key is (first-vertex-pos, chainLen) so a chain rediscovered
			// by a sibling caster contributes exactly once.
			//
			// `BuildSeedChainBranching` may return multiple seed chains
			// per uniform-area sample when a Fresnel-decision-point's
			// running throughput exceeds `config.branchingThreshold`
			// (Option C handling — the same threshold that PT/BDPT use
			// for delta-vertex split).  Each branch is its own
			// Newton-eligible seed; the contribution is divided by the
			// chain's `proposalPdf` to absorb the RR weighting that
			// `EvaluateChainThroughput`'s Fresnel factor would otherwise
			// double-count.
			const unsigned int M = std::max( config.multiTrials, 1u );

			for( unsigned int m = 0; m < M; m++ )
			{
				std::vector<SeedChainResult> seeds;
				if( !buildBranchedSeedsOnCaster( pCasterObj, seeds ) ) continue;

				for( SeedChainResult& seedResult : seeds )
				{
					ManifoldResult mResult = Solve(
						pos, normal,
						lightSample.position, lightSample.normal,
						seedResult.chain, loopSampler );
					if( !mResult.valid ) continue;

					const Point3 firstPos = mResult.specularChain[0].position;
					const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
					if( isDuplicate( firstPos, chainLen ) ) continue;

					Vector3 trialDir;
					RISEPel trialContrib;
					if( !ComputeTrialContribution( pos, normal, onb, woOutgoing,
						pBSDF, lightSample, mResult, caster, trialDir, trialContrib ) )
						continue;

					if( seedResult.proposalPdf > 1e-20 ) {
						trialContrib = trialContrib * ( 1.0 / seedResult.proposalPdf );
					}

					totalContribution = totalContribution + trialContrib;
					acceptedRoots.push_back( RootKey{ firstPos, chainLen } );
					validContributions++;
				}
			}
		}
		else
		{
			// Unbiased mode: 1 main trial + geometric-distribution
			// Bernoulli loop (`K = first-success-index`, `E[K] = 1/p`,
			// contribution scaled by K).  Skip photon-aided seeds in
			// this branch — Bernoulli requires the trial proposal
			// distribution to match the main solve's, and the photon
			// proposal is a different distribution.
			std::vector<ManifoldVertex> trialSeed;
			if( !buildSeedFromUniformOnCaster( pCasterObj, trialSeed ) ) continue;

			ManifoldResult mResult = Solve(
				pos, normal,
				lightSample.position, lightSample.normal,
				trialSeed, loopSampler );
			if( !mResult.valid ) continue;

			const Point3 firstPos = mResult.specularChain[0].position;
			const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
			if( isDuplicate( firstPos, chainLen ) ) continue;

			Vector3 dirMain;
			RISEPel mainContrib;
			if( !ComputeTrialContribution( pos, normal, onb, woOutgoing,
				pBSDF, lightSample, mResult, caster, dirMain, mainContrib ) )
				continue;

			// Geometric Bernoulli K-loop.  Cap on `maxBernoulliTrials`,
			// hard-cap fallback at 1024 if config is 0 (prevents render
			// hangs on casters Newton can never re-discover).
			unsigned int K = 1;
			bool capHit = false;
			const unsigned int hardCap = config.maxBernoulliTrials > 0
				? config.maxBernoulliTrials : 1024u;

			while( true )
			{
				Point3 sp_t;
				Vector3 sn_t;
				Point2 sc_t;
				pCasterObj->UniformRandomPoint(
					&sp_t, &sn_t, &sc_t,
					Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );

				std::vector<ManifoldVertex> trialChain;
				bool match = false;
				if( BuildSeedChain( pos, sp_t, scene, caster, trialChain ) > 0 &&
					!trialChain.empty() && trialChain[0].pObject == pCasterObj )
				{
					ManifoldResult tResult = Solve(
						pos, normal,
						lightSample.position, lightSample.normal,
						trialChain, loopSampler );
					if( tResult.valid ) {
						Vector3 dirT = Vector3Ops::mkVector3(
							tResult.specularChain[0].position, pos );
						if( Vector3Ops::NormalizeMag( dirT ) > 1e-8 ) {
							const Scalar dotProd = Vector3Ops::Dot( dirMain, dirT );
							if( std::fabs( dotProd - 1.0 ) < dedupeThr ) {
								match = true;
							}
						}
					}
				}

				if( match ) break;
				K++;
				if( K > hardCap ) {
					capHit = true;
					break;
				}
			}

			if( capHit ) continue;   // bias toward zero when cap fires

			mainContrib = mainContrib * static_cast<Scalar>( K );
			totalContribution = totalContribution + mainContrib;
			acceptedRoots.push_back( RootKey{ firstPos, chainLen } );
			validContributions++;
		}
	}

	// Photon-aided trial extension (biased mode only).
	// Weisstein, Jhang, Chang. "Photon-Driven Manifold Sampling."
	// HPG 2024. DOI 10.1145/3675375.  Each photon's recorded chain is
	// reversed (light→diffuse → receiver→light) via the helper, run
	// through Newton, deduped against the per-caster set, and summed
	// unweighted (paper Eq. 8 form: `Σ_l f(x₂⁽ˡ⁾)` is consistent for
	// any seed distribution that covers basins with positive density).
	if( config.biased && pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}

		if( r > 0 )
		{
			std::vector<SMSPhoton> photonSeeds;
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );

			for( const SMSPhoton& ph : photonSeeds )
			{
				std::vector<ManifoldVertex> photonChain;
				if( ReversePhotonChainForSeed( ph, photonChain ) == 0 ) continue;

				ManifoldResult mResult = Solve(
					pos, normal,
					lightSample.position, lightSample.normal,
					photonChain, loopSampler );
				if( !mResult.valid ) continue;

				const Point3 firstPos = mResult.specularChain[0].position;
				const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
				if( isDuplicate( firstPos, chainLen ) ) continue;

				Vector3 trialDir;
				RISEPel trialContrib;
				if( !ComputeTrialContribution( pos, normal, onb, woOutgoing,
					pBSDF, lightSample, mResult, caster, trialDir, trialContrib ) )
					continue;

				totalContribution = totalContribution + trialContrib;
				acceptedRoots.push_back( RootKey{ firstPos, chainLen } );
				validContributions++;
			}
		}
	}

	if( validContributions == 0 ) return result;

	result.contribution = totalContribution;
	result.misWeight = 1.0;
	result.valid = true;
	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointNMUniform
//
//   Spectral counterpart of EvaluateAtShadingPointUniform.
//   Mirrors the RGB structure: per-caster Mitsuba-style sum, M-trial
//   biased mode with cross-trial dedupe by (first-vertex-pos,
//   chainLen), geometric Bernoulli when biased=false, photon-aided
//   trial extension on biased mode.  Per-vertex eta is overridden
//   with `GetSpecularInfoNM(nm)` AFTER the chain is built and BEFORE
//   `Solve` runs so dispersive glass converges to the wavelength-
//   specific caustic root.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContributionNM ManifoldSolver::EvaluateAtShadingPointNMUniform(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const Scalar nm
	) const
{
	SMSContributionNM result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	if( mSpecularCasters.empty() ) return result;

	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) ) {
		return result;
	}

	// Sampler-dimension-drift firewall — see EvaluateAtShadingPointUniform
	// for the full rationale.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	struct RootKey { Point3 pos; unsigned int chainLen; };
	std::vector<RootKey> acceptedRoots;
	acceptedRoots.reserve( mSpecularCasters.size() * 2 + 16 );

	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;

	auto isDuplicate = [&]( const Point3& fp, unsigned int cl ) -> bool {
		for( const RootKey& rk : acceptedRoots ) {
			if( rk.chainLen == cl &&
				Point3Ops::Distance( rk.pos, fp ) < dedupeThr ) {
				return true;
			}
		}
		return false;
	};

	// Per-wavelength eta override: re-query each vertex's material with
	// `GetSpecularInfoNM` so the chain `Solve` converges to the correct
	// wavelength-specific caustic root (essential for dispersive glass).
	auto applyNMEtaToChain = [&]( std::vector<ManifoldVertex>& chain ) {
		IORStack queryIor( 1.0 );
		for( ManifoldVertex& v : chain ) {
			if( v.pMaterial ) {
				Ray dummyRay( v.position, v.normal );
				RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
				rigLocal.bHit          = true;
				rigLocal.ptIntersection = v.position;
				rigLocal.vNormal       = v.normal;
				SpecularInfo specNM = v.pMaterial->GetSpecularInfoNM( rigLocal, queryIor, nm );
				v.eta         = specNM.ior;
				v.attenuation = specNM.attenuation;
				v.canRefract  = specNM.canRefract;
			}
			v.valid = false;
		}
	};

	auto buildSeedFromUniformOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<ManifoldVertex>& trialSeed) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		trialSeed.clear();
		const unsigned int chainLen = BuildSeedChain(
			pos, sp, scene, caster, trialSeed );
		if( chainLen == 0 || trialSeed.empty() ) return false;
		if( trialSeed[0].pObject != pCasterObj ) return false;
		applyNMEtaToChain( trialSeed );
		return true;
	};

	// Branched seed (PT-faithful split semantic) — see RGB variant
	// for the full rationale.
	auto buildBranchedSeedsOnCaster = [&](
		const IObject* pCasterObj,
		std::vector<SeedChainResult>& outSeeds) -> bool
	{
		Point3 sp;
		Vector3 sn;
		Point2 sc;
		pCasterObj->UniformRandomPoint(
			&sp, &sn, &sc,
			Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );
		outSeeds.clear();
		BuildSeedChainBranching( pos, sp, scene, caster, loopSampler, outSeeds );
		outSeeds.erase(
			std::remove_if( outSeeds.begin(), outSeeds.end(),
				[&]( const SeedChainResult& r ) {
					return r.chain.empty() || r.chain[0].pObject != pCasterObj;
				} ),
			outSeeds.end() );
		// Per-wavelength eta override on each chain so dispersive
		// caustics converge to the correct wavelength-specific root.
		for( SeedChainResult& r : outSeeds ) {
			applyNMEtaToChain( r.chain );
		}
		return !outSeeds.empty();
	};

	Scalar totalContribution = 0.0;
	unsigned int validContributions = 0;

	for( const IObject* pCasterObj : mSpecularCasters )
	{
		if( !pCasterObj ) continue;

		if( config.biased )
		{
			const unsigned int M = std::max( config.multiTrials, 1u );

			for( unsigned int m = 0; m < M; m++ )
			{
				std::vector<SeedChainResult> seeds;
				if( !buildBranchedSeedsOnCaster( pCasterObj, seeds ) ) continue;

				for( SeedChainResult& seedResult : seeds )
				{
					ManifoldResult mResult = Solve(
						pos, normal,
						lightSample.position, lightSample.normal,
						seedResult.chain, loopSampler );
					if( !mResult.valid ) continue;

					const Point3 firstPos = mResult.specularChain[0].position;
					const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
					if( isDuplicate( firstPos, chainLen ) ) continue;

					Vector3 trialDir;
					Scalar trialContrib;
					if( !ComputeTrialContributionNM( pos, normal, onb, woOutgoing,
						pBSDF, lightSample, mResult, caster, nm, trialDir, trialContrib ) )
						continue;

					if( seedResult.proposalPdf > 1e-20 ) {
						trialContrib *= ( 1.0 / seedResult.proposalPdf );
					}

					totalContribution += trialContrib;
					acceptedRoots.push_back( RootKey{ firstPos, chainLen } );
					validContributions++;
				}
			}
		}
		else
		{
			std::vector<ManifoldVertex> trialSeed;
			if( !buildSeedFromUniformOnCaster( pCasterObj, trialSeed ) ) continue;

			ManifoldResult mResult = Solve(
				pos, normal,
				lightSample.position, lightSample.normal,
				trialSeed, loopSampler );
			if( !mResult.valid ) continue;

			const Point3 firstPos = mResult.specularChain[0].position;
			const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
			if( isDuplicate( firstPos, chainLen ) ) continue;

			Vector3 dirMain;
			Scalar mainContrib;
			if( !ComputeTrialContributionNM( pos, normal, onb, woOutgoing,
				pBSDF, lightSample, mResult, caster, nm, dirMain, mainContrib ) )
				continue;

			unsigned int K = 1;
			bool capHit = false;
			const unsigned int hardCap = config.maxBernoulliTrials > 0
				? config.maxBernoulliTrials : 1024u;

			while( true )
			{
				Point3 sp_t;
				Vector3 sn_t;
				Point2 sc_t;
				pCasterObj->UniformRandomPoint(
					&sp_t, &sn_t, &sc_t,
					Point3( loopSampler.Get1D(), loopSampler.Get1D(), loopSampler.Get1D() ) );

				std::vector<ManifoldVertex> trialChain;
				bool match = false;
				if( BuildSeedChain( pos, sp_t, scene, caster, trialChain ) > 0 &&
					!trialChain.empty() && trialChain[0].pObject == pCasterObj )
				{
					applyNMEtaToChain( trialChain );
					ManifoldResult tResult = Solve(
						pos, normal,
						lightSample.position, lightSample.normal,
						trialChain, loopSampler );
					if( tResult.valid ) {
						Vector3 dirT = Vector3Ops::mkVector3(
							tResult.specularChain[0].position, pos );
						if( Vector3Ops::NormalizeMag( dirT ) > 1e-8 ) {
							const Scalar dotProd = Vector3Ops::Dot( dirMain, dirT );
							if( std::fabs( dotProd - 1.0 ) < dedupeThr ) {
								match = true;
							}
						}
					}
				}

				if( match ) break;
				K++;
				if( K > hardCap ) {
					capHit = true;
					break;
				}
			}

			if( capHit ) continue;

			mainContrib *= static_cast<Scalar>( K );
			totalContribution += mainContrib;
			acceptedRoots.push_back( RootKey{ firstPos, chainLen } );
			validContributions++;
		}
	}

	// Photon-aided trial extension (biased only).  Photon chain is
	// reversed via the helper; per-vertex NM eta is then re-applied
	// for dispersion correctness before Solve.
	if( config.biased && pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}

		if( r > 0 )
		{
			std::vector<SMSPhoton> photonSeeds;
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );

			for( const SMSPhoton& ph : photonSeeds )
			{
				std::vector<ManifoldVertex> photonChain;
				if( ReversePhotonChainForSeed( ph, photonChain ) == 0 ) continue;

				applyNMEtaToChain( photonChain );

				ManifoldResult mResult = Solve(
					pos, normal,
					lightSample.position, lightSample.normal,
					photonChain, loopSampler );
				if( !mResult.valid ) continue;

				const Point3 firstPos = mResult.specularChain[0].position;
				const unsigned int chainLen = static_cast<unsigned int>( mResult.specularChain.size() );
				if( isDuplicate( firstPos, chainLen ) ) continue;

				Vector3 trialDir;
				Scalar trialContrib;
				if( !ComputeTrialContributionNM( pos, normal, onb, woOutgoing,
					pBSDF, lightSample, mResult, caster, nm, trialDir, trialContrib ) )
					continue;

				totalContribution += trialContrib;
				acceptedRoots.push_back( RootKey{ firstPos, chainLen } );
				validContributions++;
			}
		}
	}

	if( validContributions == 0 ) return result;

	result.contribution = totalContribution;
	result.misWeight = 1.0;
	result.valid = true;
	return result;
}

//////////////////////////////////////////////////////////////////////
// EvaluateAtShadingPointNM
//
//   Spectral variant of EvaluateAtShadingPoint.
//   Uses per-wavelength IOR for dispersion and scalar evaluation
//   throughout.
//////////////////////////////////////////////////////////////////////

ManifoldSolver::SMSContributionNM ManifoldSolver::EvaluateAtShadingPointNM(
	const Point3& pos,
	const Vector3& normal,
	const OrthonormalBasis3D& onb,
	const IMaterial* pMaterial,
	const Vector3& woOutgoing,
	const IScene& scene,
	const IRayCaster& caster,
	ISampler& sampler,
	const Scalar nm
	) const
{
	// Mitsuba-faithful uniform-on-shape seeding (opt-in via
	// `sms_seeding "uniform"`).  Spectral-path counterpart of
	// `EvaluateAtShadingPointUniform`.
	if( config.seedingMode == ManifoldSolverConfig::eSeedingUniform )
	{
		return EvaluateAtShadingPointNMUniform(
			pos, normal, onb, pMaterial, woOutgoing,
			scene, caster, sampler, nm );
	}

	SMSContributionNM result;

	if( !pMaterial ) return result;

	const IBSDF* pBSDF = pMaterial->GetBSDF();
	if( !pBSDF ) return result;

	// Use the caster's prepared LightSampler
	const LightSampler* pLS = caster.GetLightSampler();
	if( !pLS ) return result;

	const ILuminaryManager* pLumMgr = caster.GetLuminaries();
	LuminaryManager::LuminariesList emptyList;
	const LuminaryManager* pLumManager = dynamic_cast<const LuminaryManager*>( pLumMgr );
	const LuminaryManager::LuminariesList& luminaries = pLumManager ?
		const_cast<LuminaryManager*>(pLumManager)->getLuminaries() : emptyList;

	LightSample lightSample;
	if( !pLS->SampleLight( scene, luminaries, sampler, lightSample ) )
		return result;

	// Sampler-dimension-drift firewall — see EvaluateAtShadingPointUniform.
	SMSLoopSampler loopScope( sampler );
	ISampler& loopSampler = loopScope.sampler;

	// Build seed chain toward light, with fallbacks (see RGB variant).
	std::vector<ManifoldVertex> seedChain;
	unsigned int chainLen = BuildSeedChain(
		pos, lightSample.position,
		scene, caster, seedChain );

	if( chainLen == 0 || seedChain.empty() )
	{
		const Point3 normalTarget = Point3Ops::mkPoint3(
			pos, normal * 100.0 );
		chainLen = BuildSeedChain(
			pos, normalTarget,
			scene, caster, seedChain );
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		const Point3 midpoint(
			(pos.x + lightSample.position.x) * 0.5,
			(pos.y + lightSample.position.y) * 0.5,
			(pos.z + lightSample.position.z) * 0.5 );
		const Point3 midTarget = Point3Ops::mkPoint3(
			pos, Vector3Ops::Normalize( Vector3Ops::mkVector3( midpoint, pos ) ) * 100.0 );
		chainLen = BuildSeedChain(
			pos, midTarget,
			scene, caster, seedChain );
	}

	if( chainLen == 0 || seedChain.empty() )
	{
		return result;
	}

	// Override each vertex's IOR with the wavelength-dependent value.
	// This is what makes dispersion work — the Newton solver will find
	// a different position for each wavelength due to the different IOR.
	IORStack queryIor( 1.0 );
	for( unsigned int i = 0; i < seedChain.size(); i++ )
	{
		if( seedChain[i].pMaterial )
		{
			// Build a minimal RayIntersectionGeometric for the query
			Ray dummyRay( seedChain[i].position, seedChain[i].normal );
			RayIntersectionGeometric rig( dummyRay, nullRasterizerState );
			rig.bHit = true;
			rig.ptIntersection = seedChain[i].position;
			rig.vNormal = seedChain[i].normal;

			SpecularInfo specNM = seedChain[i].pMaterial->GetSpecularInfoNM(
				rig, queryIor, nm );
			seedChain[i].eta = specNM.ior;
			// Also update the wavelength-dependent side of the
			// (etaI, etaT) pair populated by BuildSeedChain.  The
			// vertex's "outgoing-medium IOR" for entering, or
			// "incoming-medium IOR" for exiting, IS the surface
			// material's IOR — which is what specNM.ior gives us
			// per wavelength (dispersion).  The OPPOSITE side's IOR
			// (the surrounding medium) is left as set by the RGB
			// BuildSeedChain pass; for typical SMS scenes (single
			// dielectric in air, surrounding = 1.0) this is
			// wavelength-independent and correct.  For doubly-
			// nested dispersive scenes (e.g. dispersive-glass inside
			// dispersive-glass) the surrounding side would also be
			// wavelength-dependent and needs a separate per-vertex
			// stack-of-NM-IORs to track exactly — left for a future
			// extension when such a scene exists.
			if( seedChain[i].isExiting ) {
				seedChain[i].etaI = specNM.ior;
			} else {
				seedChain[i].etaT = specNM.ior;
			}
		}
	}

	// Multi-trial SMS with photon-aided seeding + root-dedupe — see
	// EvaluateAtShadingPoint (RGB) for the full rationale.
	//
	// For NM (spectral): each trial's new seed chain is built by
	// BuildSeedChain WITHOUT the wavelength-dependent eta override.  We
	// re-apply the per-wavelength eta to the new chain inside the trial
	// loop so every converged root uses the correct dispersion IOR.
	const unsigned int N = ( config.multiTrials > 0 ) ? config.multiTrials : 1;
	const Scalar dedupeThr = ( config.uniquenessThreshold > 0.0 )
		? config.uniquenessThreshold : 1e-4;
	const std::vector<ManifoldVertex> baseSeedChain = seedChain;

	// See the RGB EvaluateAtShadingPoint companion: photon retrieval is
	// independent of `multi_trials` (N) so a scene that only sets
	// `sms_photon_count > 0` still gets photons fed into Newton.
	std::vector<SMSPhoton> photonSeeds;
	if( pPhotonMap && pPhotonMap->IsBuilt() )
	{
		Scalar r = config.photonSearchRadius;
		if( r <= 0 ) {
			r = pPhotonMap->GetAutoRadius();
		}
		if( r > 0 ) {
			pPhotonMap->QuerySeeds( pos, r * r, photonSeeds );
			RandomSubsamplePhotonSeeds( photonSeeds,
				config.maxPhotonSeedsPerShadingPoint, loopSampler );
		}
	}

	std::vector<Point3> acceptedRootPositions;
	Scalar totalContribution = 0.0;
	unsigned int validTrials = 0;

	std::size_t photonCursor = 0;

	// Total trial budget: 1 base seed + photon trials + (N - 1) extras.
	// Mirrors the RGB site's totalTrials.  The `trial > 0` branch below
	// consumes photons; without `+ photonSeeds.size()` here, only the
	// (N-1) extra slots could draw from the photon list — the rest of
	// the queried photons would be silently discarded.
	const unsigned int totalTrials = 1u
		+ static_cast<unsigned int>( photonSeeds.size() )
		+ ( N > 0 ? N - 1 : 0 );

	for( unsigned int trial = 0; trial < totalTrials; trial++ )
	{
		std::vector<ManifoldVertex> trialSeed = baseSeedChain;

		// See RGB variant for the rationale: use photon's stored chain
		// directly (reversed and with flipped isExiting) to preserve
		// the topology the photon actually traversed.
		if( trial > 0 )
		{
			if( photonCursor >= photonSeeds.size() ) {
				continue;
			}
			const SMSPhoton& ph = photonSeeds[photonCursor];
			photonCursor++;

			const unsigned int k = ph.chainLen;
			if( k == 0 || k > kSMSMaxPhotonChain ) {
				continue;
			}

			std::vector<ManifoldVertex> newChain( k );
			IORStack queryIor( 1.0 );
			for( unsigned int i = 0; i < k; i++ )
			{
				const SMSPhotonChainVertex& pv = ph.chain[ k - 1 - i ];
				ManifoldVertex& mv = newChain[i];
				mv.position    = pv.position;
				mv.normal      = pv.normal;
				mv.pObject     = pv.pObject;
				mv.pMaterial   = pv.pMaterial;
				// mv.attenuation is set alongside mv.eta below from the
				// per-wavelength SpecularInfo (dropping the hardcoded white
				// that previously bypassed material colour).
				// Chain-vertex semantics recovered from the photon record;
				// see the RGB path above for the full rationale.
				mv.isReflection = ( ( pv.flags & 0x2 ) != 0 );
				mv.isExiting    = mv.isReflection
				                ? ( ( pv.flags & 0x1 ) != 0 )    // preserve
				                : ( ( pv.flags & 0x1 ) == 0 );   // flip
				mv.valid       = false;

				// Per-wavelength eta override (dispersion).  Also take the
				// per-wavelength attenuation at this vertex — a coloured or
				// absorbing glass's caustic otherwise comes out white/too-
				// bright whenever a multi-trial round discovers a root
				// through photon-aided seeding.
				if( pv.pMaterial )
				{
					Ray dummyRay( pv.position, pv.normal );
					RayIntersectionGeometric rigLocal( dummyRay, nullRasterizerState );
					rigLocal.bHit = true;
					rigLocal.ptIntersection = pv.position;
					rigLocal.vNormal = pv.normal;
					SpecularInfo specNM = pv.pMaterial->GetSpecularInfoNM(
						rigLocal, queryIor, nm );
					mv.eta = specNM.ior;
					mv.attenuation = specNM.attenuation;
					mv.canRefract  = specNM.canRefract;
				} else {
					mv.eta = pv.eta;
					mv.attenuation = RISEPel( 1, 1, 1 );
					mv.canRefract  = true;
				}
				// Photon-aided seed reconstruction does not currently
				// store the IOR-stack snapshot at each vertex — the
				// SMSPhoton record only carries `eta` (the surface
				// material's IOR).  As a result, mv.etaI and mv.etaT
				// stay at their default 1.0 here, and downstream
				// math (EvaluateConstraint, BuildJacobian, etc.) falls
				// back to the air-on-other-side assumption via
				// GetEffectiveEtas.  Correct for single-dielectric-in-
				// air photon caustics (the typical SMS photon use
				// case); WRONG for nested-dielectric scenes seeded via
				// photons.  Fixing this requires extending SMSPhoton
				// per-vertex storage with (etaIncidentRGB, etaT) at
				// emission time — left as a future extension for when
				// nested-dielectric scenes actually use SMS photon
				// seeding (PathMLT defaults to photonCount=0).
			}
			trialSeed = newChain;
		}

		ManifoldResult mResult = Solve(
			pos, normal,
			lightSample.position, lightSample.normal,
			trialSeed, loopSampler );

		if( !mResult.valid ) continue;

		// Dedupe: skip if we've already accepted a root at this first-vertex.
		const Point3& firstPos = mResult.specularChain[0].position;
		bool duplicate = false;
		for( unsigned int r = 0; r < acceptedRootPositions.size(); r++ )
		{
			if( Point3Ops::Distance( firstPos, acceptedRootPositions[r] ) < dedupeThr )
			{
				duplicate = true;
				break;
			}
		}
		if( duplicate ) continue;

		// Visibility: check external segments of the specular chain
		if( !CheckChainVisibility( pos, lightSample.position,
			mResult.specularChain, caster ) ) continue;

		// Direction from shading point toward first specular vertex
		const ManifoldVertex& firstSpec = mResult.specularChain[0];
		Vector3 dirToFirstSpec = Vector3Ops::mkVector3(
			firstSpec.position, pos );
		Scalar distToFirstSpec = Vector3Ops::Magnitude( dirToFirstSpec );
		if( distToFirstSpec < 1e-8 ) continue;
		dirToFirstSpec = dirToFirstSpec * (1.0 / distToFirstSpec);

		const Vector3 wiAtShading = dirToFirstSpec;

		// Evaluate BSDF at shading point (spectral)
		Ray evalRay( pos, Vector3( -woOutgoing.x, -woOutgoing.y, -woOutgoing.z ) );
		RayIntersectionGeometric rig( evalRay, nullRasterizerState );
		rig.bHit = true;
		rig.ptIntersection = pos;
		rig.vNormal = normal;
		rig.onb = onb;

		Scalar fBSDF = pBSDF->valueNM( wiAtShading, rig, nm );
		if( fBSDF <= 0 ) continue;

		// Cosine at shading point
		Scalar cosAtShading = fabs( Vector3Ops::Dot( normal, wiAtShading ) );
		if( cosAtShading <= 0 ) continue;

		// Chain throughput (spectral — per-wavelength Fresnel)
		Scalar chainThroughput = EvaluateChainThroughputNM(
			pos, lightSample.position, mResult.specularChain, nm );

		// Direction from last specular vertex to light (for cosine eval)
		const ManifoldVertex& lastSpec = mResult.specularChain.back();
		Vector3 dirSpecToLight = Vector3Ops::mkVector3(
			lastSpec.position, lightSample.position );
		Scalar distSpecToLight = Vector3Ops::Magnitude( dirSpecToLight );
		if( distSpecToLight < 1e-8 ) continue;
		dirSpecToLight = dirSpecToLight * (1.0 / distSpecToLight);

		Scalar cosAtLight;
		Scalar Le;
		if( lightSample.isDelta ) {
			cosAtLight = 1.0;
			if( lightSample.pLight ) {
				Le = ColorMath::Luminance(
					lightSample.pLight->emittedRadiance( dirSpecToLight ) );
			} else {
				Le = ColorMath::Luminance( lightSample.Le );
			}
		} else {
			cosAtLight = fabs( Vector3Ops::Dot( lightSample.normal, dirSpecToLight ) );
			if( cosAtLight <= 0 ) continue;
			Le = ColorMath::Luminance( lightSample.Le );
		}

		// SMS measure-conversion factor — must match the RGB path exactly
		// (see the long comment in EvaluateAtShadingPoint for derivation).
		// The previous `cosAtLight * chainGeom / jacobianDet` formulation is
		// obsolete: it double-counts distance terms via the chainGeom
		// product and applies cos(θ_y) explicitly even though cos(θ_y) is
		// implicit in |det(∂C/∂y)|.  Replacing with G(x, v_1) · |det(δv_1/δy)|
		// keeps the spectral caustic intensity consistent with the RGB path
		// (important for HWSS regressions and spectral fireflies that would
		// otherwise leak in only at certain wavelengths).
		const ManifoldVertex& firstSpecForG = mResult.specularChain[0];
		Vector3 dirXtoV1 = Vector3Ops::mkVector3( firstSpecForG.position, pos );
		const Scalar distXtoV1 = Vector3Ops::NormalizeMag( dirXtoV1 );
		if( distXtoV1 < 1e-8 ) continue;
		const Scalar cosV1atX = fabs( Vector3Ops::Dot( firstSpecForG.normal, dirXtoV1 ) );
		const Scalar G_x_v1 = cosV1atX / (distXtoV1 * distXtoV1);
		const Scalar detDvDy = ComputeLightToFirstVertexJacobianDet(
			mResult.specularChain, pos, lightSample.position, lightSample.normal );
		const Scalar smsGeometric = G_x_v1 * detDvDy;
		const Scalar clampedGeometric = fmin( smsGeometric, config.maxGeometricTerm );

		// cosAtLight is no longer multiplied here — it is implicit in the
		// Jacobian.  Keep its evaluation above for the backface-cull guard
		// on non-delta lights; silence the unused-variable warning:
		(void)cosAtLight;

		Scalar trialContribution = fBSDF
			* chainThroughput
			* Le * cosAtShading * clampedGeometric
			/ (lightSample.pdfPosition * lightSample.pdfSelect);

		if( mResult.pdf > 1e-20 )
		{
			trialContribution = trialContribution / mResult.pdf;
		}

		totalContribution += trialContribution;
		acceptedRootPositions.push_back( firstPos );
		validTrials++;
	}

	if( validTrials == 0 ) return result;

	result.contribution = totalContribution;  // Σ_r f_r over unique roots
	result.misWeight = 1.0;  // PDF weighting already folded into contribution
	result.valid = true;

	return result;
}

//////////////////////////////////////////////////////////////////////
// CheckChainVisibility
//
//   Tests whether the external segments of an SMS specular chain
//   are unoccluded.  Only the two external segments are tested:
//
//     1. Shading point  ->  first specular vertex
//     2. Last specular vertex  ->  light source
//
//   Inter-specular segments (through refractive geometry) are not
//   tested because CastShadowRay tests all objects, including the
//   glass surfaces themselves, which would always report a hit.
//   The external-segment checks catch the dominant occlusion cases
//   (opaque walls between receiver and glass, or between glass
//   and light).
//////////////////////////////////////////////////////////////////////

namespace
{
	// Chain-aware visibility: the segment is considered BLOCKED if the ray
	// passes through a specular caster that is NOT in the allowed set
	// (the chain's caster objects).  Specular hits on allowed casters
	// are traversed like above (the chain already accounts for them);
	// hits on *unrelated* specular objects mean the physical photon
	// path would also refract there, making the k-vertex chain an
	// incomplete description of the transport.  Rejecting them prevents
	// computing light through a chain topology shorter than the actual
	// physics would demand — a major firefly source on
	// intersecting-glass scenes (torus_cross) where shadow rays through
	// the caustic center graze the other torus.
	bool SegmentOccludedByNonChainSpeculars(
		const Point3& start,
		const Vector3& dir,
		const Scalar maxDist,
		const IRayCaster& caster,
		const std::vector<const IObject*>& allowedSpecularObjects )
	{
		const IScene* pScene = caster.GetAttachedScene();
		if( !pScene ) {
			return caster.CastShadowRay( Ray( start, dir ), maxDist );
		}
		const IObjectManager* pObjMgr = pScene->GetObjects();
		if( !pObjMgr ) {
			return caster.CastShadowRay( Ray( start, dir ), maxDist );
		}

		const unsigned int kMaxSpecularTraversals = 8;
		Point3 curOrigin = start;
		Scalar distRemaining = maxDist;

		for( unsigned int it = 0; it < kMaxSpecularTraversals; it++ )
		{
			Ray ray( curOrigin, dir );
			RayIntersection ri( ray, nullRasterizerState );
			pObjMgr->IntersectRay( ri, true, true, false );

			if( !ri.geometric.bHit ) return false;
			if( ri.geometric.range > distRemaining ) return false;

			const IMaterial* pMat = ri.pMaterial;
			bool isPureSpecular = false;
			if( pMat ) {
				IORStack dummyStack( 1.0 );
				SpecularInfo specInfo = pMat->GetSpecularInfo( ri.geometric, dummyStack );
				isPureSpecular = specInfo.isSpecular;
			}
			if( !isPureSpecular ) {
				return true;  // Opaque blocker
			}

			// Is this specular hit on one of the chain's casters?
			bool inChain = false;
			for( std::size_t a = 0; a < allowedSpecularObjects.size(); a++ )
			{
				if( ri.pObject == allowedSpecularObjects[a] ) { inChain = true; break; }
			}
			if( !inChain ) {
				// Specular object not in the chain — the physical path
				// through here requires additional refractions we don't
				// have.  Treat as blocked.
				return true;
			}

			// Allowed specular: traverse past and keep going.
			const Scalar step = ri.geometric.range + 1e-4;
			if( step >= distRemaining ) return false;
			curOrigin = Point3Ops::mkPoint3( curOrigin, dir * step );
			distRemaining -= step;
		}
		return false;
	}
}

bool ManifoldSolver::CheckChainVisibility(
	const Point3& shadingPoint,
	const Point3& lightPoint,
	const std::vector<ManifoldVertex>& chain,
	const IRayCaster& caster
	) const
{
	if( chain.empty() ) return true;

#if SMS_TRACE_DIAGNOSTIC
	static std::atomic<int> g_visTraceCount{ 0 };
	const bool visTrace =
		( std::fabs( shadingPoint.x ) < 0.3 ) &&
		( std::fabs( shadingPoint.z ) < 0.3 ) &&
		( shadingPoint.y >= -0.02 && shadingPoint.y <= 0.02 ) &&
		( g_visTraceCount.fetch_add( 1, std::memory_order_relaxed ) < 30 );
#endif

	// Build the "allowed specular objects" list from the chain's vertex
	// casters.  Any specular hit on an object NOT in this list on an
	// external segment means the straight-line shadow path would refract
	// there — the k-vertex chain is incomplete and should be rejected.
	// See the block comment on SegmentOccludedByNonChainSpeculars.
	std::vector<const IObject*> chainCasters;
	chainCasters.reserve( chain.size() );
	for( std::size_t i = 0; i < chain.size(); i++ ) {
		const IObject* o = chain[i].pObject;
		if( o ) {
			bool have = false;
			for( std::size_t j = 0; j < chainCasters.size(); j++ ) {
				if( chainCasters[j] == o ) { have = true; break; }
			}
			if( !have ) chainCasters.push_back( o );
		}
	}

	// Segment 1: shading point to first specular vertex
	//
	// The shading point sits on a non-specular surface.  The first
	// specular vertex is on the glass surface facing the shading
	// point.  We offset the ray endpoint by a small amount to
	// avoid hitting the glass surface at the target vertex.
	{
		const ManifoldVertex& vFirst = chain.front();

		// Determine the outward normal at the first vertex (pointing
		// toward the shading point, i.e. away from the glass interior)
		Vector3 dirToShading = Vector3Ops::mkVector3( shadingPoint, vFirst.position );
		const Scalar nDotDir = Vector3Ops::Dot( vFirst.normal, dirToShading );
		const Vector3 outwardN = (nDotDir >= 0)
			? vFirst.normal
			: Vector3( -vFirst.normal.x, -vFirst.normal.y, -vFirst.normal.z );

		// Biased endpoint: pull back from the glass surface along its
		// outward normal.  This prevents the shadow ray from hitting
		// the glass mesh at or near the target vertex.  Use a generous
		// bias to clear displacement bumps on the glass surface.
		const Scalar endBias = 5e-2;
		const Point3 biasedEnd = Point3Ops::mkPoint3(
			vFirst.position, outwardN * endBias );

		Vector3 dir = Vector3Ops::mkVector3( biasedEnd, shadingPoint );
		Scalar dist = Vector3Ops::NormalizeMag( dir );
		if( dist > 1e-4 )
		{
			Point3 origin = Point3Ops::mkPoint3( shadingPoint, dir * 1e-4 );
			const bool blocked = SegmentOccludedByNonChainSpeculars(
				origin, dir, dist - 2e-4, caster, chainCasters );
#if SMS_TRACE_DIAGNOSTIC
			if( visTrace ) {
				GlobalLog()->PrintEx( eLog_Event,
					"VIS_SEG1: shading=(%.4f,%.4f,%.4f) biasedEnd=(%.4f,%.4f,%.4f) dist=%.4f blocked=%d",
					shadingPoint.x, shadingPoint.y, shadingPoint.z,
					biasedEnd.x, biasedEnd.y, biasedEnd.z, dist, int( blocked ) );
			}
#endif
			if( blocked )
				return false;
		}
	}

	// Segment 2: last specular vertex to light source
	//
	// The last specular vertex sits on the glass surface facing the
	// light.  On a displaced mesh, nearby bumps can easily block a
	// ray launched with only a tiny directional bias.  We offset the
	// ray origin along the surface normal at the vertex to clear the
	// local surface geometry before testing for external occlusion.
	{
		const ManifoldVertex& vLast = chain.back();

		// Outward normal: pointing toward the light (away from glass)
		Vector3 dirToLight = Vector3Ops::mkVector3( lightPoint, vLast.position );
		const Scalar nDotDir = Vector3Ops::Dot( vLast.normal, dirToLight );
		const Vector3 outwardN = (nDotDir >= 0)
			? vLast.normal
			: Vector3( -vLast.normal.x, -vLast.normal.y, -vLast.normal.z );

		// Offset start along outward normal to clear displaced surface.
		// Use a generous bias for rough displacement.
		const Scalar normalBias = 5e-2;
		const Point3 biasedStart = Point3Ops::mkPoint3(
			vLast.position, outwardN * normalBias );

		Vector3 newDir = Vector3Ops::mkVector3( lightPoint, biasedStart );
		Scalar newDist = Vector3Ops::NormalizeMag( newDir );
		if( newDist > 1e-4 )
		{
			const bool blocked = SegmentOccludedByNonChainSpeculars(
				biasedStart, newDir, newDist - 1e-4, caster, chainCasters );
#if SMS_TRACE_DIAGNOSTIC
			if( visTrace ) {
				GlobalLog()->PrintEx( eLog_Event,
					"VIS_SEG2: biasedStart=(%.4f,%.4f,%.4f) light=(%.4f,%.4f,%.4f) dist=%.4f blocked=%d",
					biasedStart.x, biasedStart.y, biasedStart.z,
					lightPoint.x, lightPoint.y, lightPoint.z, newDist, int( blocked ) );
			}
#endif
			if( blocked )
				return false;
		}
	}

	// Segment I: inter-specular AIR segments between consecutive
	// chain vertices.  A chain whose topology traverses two or more
	// DISTINCT casters (e.g. the k=2×2 path through two different
	// toruses) has air segments in between each pair of casters:
	//
	//   shading  -[air]-  v_i (enter cast A)  -[A glass]-  v_{i+1}
	//              (exit A)  -[air]-  v_{i+2} (enter cast B)  -[B glass]-
	//              v_{i+3} (exit B)  -[air]-  light
	//
	// A segment v_i → v_{i+1} is in AIR when v_i.isExiting==true
	// (receiver ray leaves glass at v_i) AND v_{i+1}.isExiting==false
	// (receiver ray enters glass at v_{i+1}).  These segments need
	// the same chain-aware visibility check as the outer two: if they
	// cross a specular caster NOT in the chain, the k-vertex
	// description is missing refractions that the physical photon
	// would have to undergo, and the chain should be rejected.
	// (The complementary topology — v_i entering glass, v_{i+1}
	// exiting same object — is an IN-GLASS segment and intentionally
	// skipped: the chain accounts for the refractions bracketing it.)
	for( std::size_t i = 0; i + 1 < chain.size(); i++ )
	{
		const ManifoldVertex& a = chain[i];
		const ManifoldVertex& b = chain[i+1];
		if( !a.isExiting ) continue;   // segment starts INSIDE glass
		if( b.isExiting )  continue;   // segment ends INSIDE glass

		// Both endpoints are on the AIR side of their surface —
		// segment is in air.  Bias the origin out along a's outward
		// normal (away from its glass), endpoint back along b's
		// outward normal (away from its glass).
		Vector3 outN_a = a.normal;
		{
			Vector3 dirOutA = Vector3Ops::mkVector3( b.position, a.position );
			if( Vector3Ops::Dot( outN_a, dirOutA ) < 0 )
				outN_a = Vector3( -outN_a.x, -outN_a.y, -outN_a.z );
		}
		Vector3 outN_b = b.normal;
		{
			Vector3 dirOutB = Vector3Ops::mkVector3( a.position, b.position );
			if( Vector3Ops::Dot( outN_b, dirOutB ) < 0 )
				outN_b = Vector3( -outN_b.x, -outN_b.y, -outN_b.z );
		}

		const Scalar biasEps = 5e-2;
		const Point3 biasedStart = Point3Ops::mkPoint3(
			a.position, outN_a * biasEps );
		const Point3 biasedEnd = Point3Ops::mkPoint3(
			b.position, outN_b * biasEps );

		Vector3 segDir = Vector3Ops::mkVector3( biasedEnd, biasedStart );
		const Scalar segDist = Vector3Ops::NormalizeMag( segDir );
		if( segDist < 1e-4 ) continue;

		const bool blocked = SegmentOccludedByNonChainSpeculars(
			biasedStart, segDir, segDist - 1e-4, caster, chainCasters );
#if SMS_TRACE_DIAGNOSTIC
		if( visTrace ) {
			GlobalLog()->PrintEx( eLog_Event,
				"VIS_SEGI[%zu-%zu]: a=(%.4f,%.4f,%.4f) b=(%.4f,%.4f,%.4f) dist=%.4f blocked=%d",
				i, i+1,
				a.position.x, a.position.y, a.position.z,
				b.position.x, b.position.y, b.position.z,
				segDist, int( blocked ) );
		}
#endif
		if( blocked )
			return false;
	}

	return true;
}
