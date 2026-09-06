//////////////////////////////////////////////////////////////////////
//
//  ReliefModifier.cpp - Implementation of the painter-driven
//  shading-normal micro-relief modifier.  See ReliefModifier.h for the
//  what and the why, and docs/RELIEF_MODIFIER_DESIGN.md 3.2/3.3 for the
//  full derivation.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 5, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ReliefModifier.h"
#include "ModifierFrame.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Math3D/Math3D.h"

#include <atomic>
#include <cmath>

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	//! The automatic surface-domain half-step floor (design 3.3).  A
	//! derivative-estimator step in double precision, NOT a scene-scale
	//! guess: on a scene whose features are below 1e-3 world units the
	//! author sets `step` explicitly, and the descriptor says so.  This is
	//! the one new constant in the design and it is disclosed as such.
	const Scalar RELIEF_AUTO_STEP_SURFACE = Scalar( 1e-3 );

	//! The automatic UV-domain half-step (design 3.3).  Matches
	//! `bumpmap_modifier`'s `windowsize` default, so a migrated scene that
	//! omitted the window lands on the same span.
	const Scalar RELIEF_AUTO_STEP_UV = Scalar( 0.01 );

	//! Relative conditioning gate on the UV chain rule's normal equations.
	//! Below this the (dpdu, dpdv) pair is degenerate or near-parallel and
	//! the least-squares (du, dv) is numerically meaningless; leave ptCoord
	//! alone rather than move it by an amplified-noise amount.
	const Scalar RELIEF_UV_SOLVE_REL_EPS = Scalar( 1e-12 );

	//! Fires once per process when a surface-domain relief evaluation has
	//! to move `ptObjIntersec` by the WORLD step because the hit carries no
	//! world->object map (a CSG hit, or a record built outside
	//! Object::IntersectRay).  See RayIntersectionGeometric::
	//! pmxWorldToObject for why that pointer can be legitimately absent.
	std::atomic<bool> g_warnedNoObjectTransform( false );
}

ReliefModifier::ReliefModifier(
	const IScalarPainter& height_,
	const Scalar scale_,
	const ReliefDomain domain_,
	const Scalar step_
	) :
  height( height_ ), dScale( scale_ ), domain( domain_ ), dStep( step_ )
{
	height.addref();
}

ReliefModifier::~ReliefModifier()
{
	height.release();
}

void ReliefModifier::Modify( RayIntersectionGeometric& ri ) const
{
	// INERT on a zero or non-finite amplitude.  `!(x != 0)` is true for
	// NaN as well as for 0, so this one test covers both without a
	// separate isnan branch.  An inert modifier leaves the hit exactly as
	// the geometry produced it -- no normal write, no frame rebuild.
	if( !( dScale != Scalar(0) ) || !std::isfinite( dScale ) ) {
		return;
	}

	// The frame the material will shade in.  On a hit that carries a
	// geometry-supplied coherent tangent this IS that tangent (Object::
	// IntersectRay built the ONB with CreateFromWU before any modifier
	// ran); on tangent-less geometry it is CreateFromW's arbitrary pick,
	// which for a 3D field is not a compromise -- the tangent-plane
	// gradient of a 3D field is frame-independent, so N' comes out the
	// same whichever tangent was picked (design 3.2).
	const Vector3 T = ri.onb.u();
	const Vector3 B = ri.onb.v();
	const Vector3 N = ri.vNormal;

	Scalar dT = 0, dB = 0;		// raw central differences, H(+s) - H(-s)
	Scalar invSpan = 0;			// 1 / (2s)

	if( domain == ReliefDomain::UV ) {
		// UV DOMAIN.  Step on ptCoord only, along (u, v).  Sampling
		// geometry is byte-for-byte `bumpmap_modifier`'s: same four
		// evaluations at the same coordinates, in the same order.  That is
		// what makes the migrator's scale fold (design 7.2) exact rather
		// than approximate.
		const Scalar s = ( dStep > Scalar(0) ) ? dStep : RELIEF_AUTO_STEP_UV;
		invSpan = Scalar(1) / ( Scalar(2) * s );

		RayIntersectionGeometric ri2 = ri;

		ri2.ptCoord.x = ri.ptCoord.x + s;
		const Scalar uPlus  = height.GetValuesAt( ri2 ).v[0];
		ri2.ptCoord.x = ri.ptCoord.x - s;
		const Scalar uMinus = height.GetValuesAt( ri2 ).v[0];
		ri2.ptCoord.x = ri.ptCoord.x;

		ri2.ptCoord.y = ri.ptCoord.y + s;
		const Scalar vPlus  = height.GetValuesAt( ri2 ).v[0];
		ri2.ptCoord.y = ri.ptCoord.y - s;
		const Scalar vMinus = height.GetValuesAt( ri2 ).v[0];

		dT = uPlus - uMinus;
		dB = vPlus - vMinus;
	} else {
		// SURFACE DOMAIN.  Step in the tangent plane, in world units.
		//
		// STEP RULE (design 3.3):
		//     s = max( user > 0 ? user : 1e-3, footprint.valid ? fw : 0 )
		//
		// The max against the pixel footprint is not a safety clamp, it is
		// the antialiasing: a central difference over a span SMALLER than
		// the footprint measures sub-pixel slope and sparkles at distance,
		// while a difference over a span OF the footprint measures the
		// footprint-averaged slope, whose magnitude is bounded by
		// max|H| / fw and therefore DECAYS as the footprint grows.  That
		// gives fields with no octave fade of their own (checker, voronoi,
		// images) the same fade-to-flat at distance the noise builtins get
		// from their own fw-driven octave fade -- on which the max is a
		// no-op at the relevant scales, since they are already
		// band-limited.
		Scalar s = ( dStep > Scalar(0) ) ? dStep : RELIEF_AUTO_STEP_SURFACE;
		if( ri.txFootprint.valid && ri.txFootprint.worldWidth > s ) {
			s = ri.txFootprint.worldWidth;
		}
		invSpan = Scalar(1) / ( Scalar(2) * s );

		// ptCoord chain rule, hoisted out of the four evaluations: the
		// local map is linear, so the (du, dv) that realises a world step
		// dp is the least-squares solution of dpdu*du + dpdv*dv = dp, and
		// the 2x2 normal-equation matrix depends only on dpdu/dpdv -- the
		// same for all four offsets.  Precompute its inverse once and
		// apply it per offset.
		//
		// When derivatives are absent the UV offset is simply not applied,
		// and a UV-parameterised painter (image, perlin2d, checker) reads
		// FLAT in surface mode.  That is the documented behaviour, and the
		// descriptor names `domain uv` as the route for that case.
		bool haveUVSolve = false;
		Scalar m00 = 0, m01 = 0, m10 = 0, m11 = 0;	// rows of the pseudo-inverse applied to (r1, r2)
		if( ri.derivatives.valid ) {
			const Vector3& dpdu = ri.derivatives.dpdu;
			const Vector3& dpdv = ri.derivatives.dpdv;
			const Scalar a = Vector3Ops::Dot( dpdu, dpdu );
			const Scalar b = Vector3Ops::Dot( dpdu, dpdv );
			const Scalar c = Vector3Ops::Dot( dpdv, dpdv );
			const Scalar det = a * c - b * b;
			// Relative gate: `det` carries (length^4), so compare it to
			// a*c rather than to an absolute epsilon, or the test would be
			// scene-scale dependent (the precision-fix-the-formulation
			// discipline).
			if( det > RELIEF_UV_SOLVE_REL_EPS * a * c && a > Scalar(0) && c > Scalar(0) ) {
				const Scalar invDet = Scalar(1) / det;
				m00 =  c * invDet;  m01 = -b * invDet;		// du = m00*r1 + m01*r2
				m10 = -b * invDet;  m11 =  a * invDet;		// dv = m10*r1 + m11*r2
				haveUVSolve = true;
			}
		}

		// Whether the world->object map is available decides how
		// ptObjIntersec moves.  Absent, we move it by the WORLD step --
		// exact for a pure translation, wrong under rotation or scale --
		// and say so once.  See the field's doc comment for when it is
		// legitimately absent (CSG hits; records not built by
		// Object::IntersectRay).
		const Matrix4* const pW2O = ri.pmxWorldToObject;
		if( !pW2O && !g_warnedNoObjectTransform.exchange( true ) ) {
			GlobalLog()->PrintEasyWarning(
				"relief_modifier: hit carries no world->object transform "
				"(ri.pmxWorldToObject) -- this happens on CSG composite hits, "
				"whose ptObjIntersec is the child operand's own object-space "
				"point, and on hit records not produced by "
				"Object::IntersectRay.  The object-space point is being "
				"offset by the WORLD step instead, which is exact only when "
				"the object's transform is a pure translation; an "
				"OBJECT-SPACE height field (mapping_painter space object, "
				"voronoi3d space object, `Po` in an expression) on a rotated "
				"or scaled object will therefore read a slightly wrong "
				"gradient.  World-space and UV-space height fields are "
				"unaffected.  This warning fires once per process." );
		}

		// The four offset evaluations.  Copy-then-offset is the
		// MappingPainter idiom (MappingPainter.cpp:96-125): every point
		// domain a painter can read moves consistently, or a painter in an
		// un-moved domain sees a flat field and contributes no gradient.
		//
		// DELIBERATELY LEFT ALONE: vNormal, derivatives, signals, and
		// txFootprint.  The first three are geometry-derived and treated
		// as locally constant (so a height expression that IS `curv`
		// produces zero relief -- documented semantics, not a bug; the
		// signals arc holds them invariant under shading-normal
		// modifiers).  txFootprint is kept VALID on purpose: the four
		// offsets describe the same pixel footprint, so an fbm height
		// fades its octaves at the offsets exactly as at the centre, and
		// the gradient of a fully-faded field is zero -- relief inherits
		// the fade-to-mean discipline for free.
		RayIntersectionGeometric ri2 = ri;
		const Vector3 offsets[4] = { T * s, T * (-s), B * s, B * (-s) };
		Scalar h[4] = { 0, 0, 0, 0 };
		for( int i = 0; i < 4; i++ ) {
			const Vector3& dp = offsets[i];

			ri2.ptIntersection = Point3Ops::mkPoint3( ri.ptIntersection, dp );

			// A step is a DIRECTION: Vector3Ops::Transform applies the
			// linear part and drops the translation column, which is what
			// a difference of two transformed points reduces to.
			ri2.ptObjIntersec = Point3Ops::mkPoint3( ri.ptObjIntersec,
				pW2O ? Vector3Ops::Transform( *pW2O, dp ) : dp );

			if( haveUVSolve ) {
				const Scalar r1 = Vector3Ops::Dot( ri.derivatives.dpdu, dp );
				const Scalar r2 = Vector3Ops::Dot( ri.derivatives.dpdv, dp );
				ri2.ptCoord.x = ri.ptCoord.x + ( m00 * r1 + m01 * r2 );
				ri2.ptCoord.y = ri.ptCoord.y + ( m10 * r1 + m11 * r2 );
			}

			h[i] = height.GetValuesAt( ri2 ).v[0];
		}

		dT = h[0] - h[1];
		dB = h[2] - h[3];
	}

	// NON-FINITE GUARD (design 3.2).  A height field that returns NaN or
	// Inf anywhere in the stencil leaves the hit UNTOUCHED rather than
	// writing a NaN normal that would poison every material downstream.
	// std::isfinite is honest here: the macOS build pairs -ffast-math with
	// -fno-finite-math-only precisely so these calls work (CLAUDE.md
	// High-Value Facts, 2026-07-29).
	//
	// MEASURED REDUNDANCY (ReliefModifierTest red-proof (c)): this gate
	// and the `mag2` gate below each catch a non-finite height on their
	// own -- removing EITHER alone leaves ReliefModifierTest green, and
	// only removing BOTH fails it.  That is a fact about the two guards,
	// not a licence to delete one: this one states the intent where the
	// value is produced, is what the design specifies, and short-circuits
	// before the perturbation arithmetic.
	if( !std::isfinite( dT ) || !std::isfinite( dB ) ) {
		return;
	}

	const Scalar hT = dT * invSpan;
	const Scalar hB = dB * invSpan;

	// BLINN SIGN.  N' = N - h_u*t - h_v*b is the exact normal of
	// p' = p + h*n to first order in h for an orthonormal (t, b, n).  It
	// holds for EITHER handedness -- the overall sign of t x b flips, the
	// perturbation relative to n does not -- which is what lets the
	// mirrored-instance frames pass through unchanged.
	const Vector3 perturbed = N - ( T * hT + B * hB ) * dScale;

	// A gradient large enough to cancel N leaves nothing to normalize.
	// Bail rather than hand Normalize a zero vector: no clamp, no flat
	// spot, just "this sample had no usable normal".  (There is
	// deliberately NO geometric-horizon clamp -- like BumpMap/NormalMap
	// and PBRT's bump mapping, a large `scale` may push N' below the
	// geometric plane and the materials' own horizon gates handle that
	// continuously.  GlintModifier's rejection is a DISCRETE facet
	// decision and does not transfer.)
	const Scalar mag2 = Vector3Ops::SquaredModulus( perturbed );
	if( !( mag2 > Scalar(1e-12) ) || !std::isfinite( mag2 ) ) {
		return;
	}

	// Frame rebuild.  Gated on bHasShadingTangent exactly as BumpMap and
	// NormalMap do -- relief is a height-gradient tilt, the same family --
	// so on tangent-less geometry the rebuild is a plain CreateFromW.  See
	// ModifierFrame.h for both corrections the helper encodes and for why
	// GlintModifier makes the opposite choice.
	const Vector3 newN = Vector3Ops::Normalize( perturbed );
	if( ri.bHasShadingTangent ) {
		ModifierFrame::RebuildPreservingTangent( ri, newN );
	} else {
		ri.vNormal = newN;
		ri.onb.CreateFromW( ri.vNormal );
	}
}
