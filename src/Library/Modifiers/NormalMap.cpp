//////////////////////////////////////////////////////////////////////
//
//  NormalMap.cpp - Implementation of the tangent-space normal-map
//  modifier.  See NormalMap.h for the design.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 30, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "NormalMap.h"
#include "ModifierFrame.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Math3D/Math3D.h"

#include <atomic>
#include <cmath>
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

NormalMap::NormalMap( const IPainter& painter, const Scalar scale ) :
  pNormalMap( painter ),
  dScale( scale )
{
	pNormalMap.addref();
}

NormalMap::~NormalMap()
{
	pNormalMap.release();
}

namespace
{
	// One-shot warning when the modifier is asked to operate on a hit
	// whose geometry has neither imported TANGENT nor valid surface
	// derivatives (dpdu / dpdv).  In that situation we fall back to the
	// orthonormal-basis tangents (ri.onb.u() / .v()), which is wrong for
	// any normal map whose UV axes don't happen to align with the ONB
	// (and the ONB orientation is arbitrary -- there's no guarantee the
	// alignment is even consistent triangle-to-triangle).
	//
	// The far-more-common no-TANGENT case (triangle mesh, source asset
	// shipped no TANGENT attribute) is silent: triangle meshes populate
	// ri.derivatives.dpdu / dpdv during intersection, and we use those
	// to build a UV-aligned tangent frame.  That's correct for any
	// connected UV chart; only mirrored regions can render wrong, and
	// that's an asset-design limitation an author resolves by adding
	// TANGENT to the source mesh.  The warning fires once per process
	// to avoid log-flooding the hot path.
	std::atomic<bool> g_warnedNoFrame{ false };
}

void NormalMap::Modify( RayIntersectionGeometric& ri ) const
{
	// Sample the normal map at the hit's primary UV.  The painter is
	// expected to be backed by an image loaded with NO color-matrix
	// conversion -- post Stage B colour-space migration that means
	// `color_space Rec709RGB_Linear` on the png_painter
	// (RISEPel == Rec709RGBPel, see Color.h, so Rec709RGB_Linear is
	// the verbatim store).  Picking sRGB would gamma-decode the bytes
	// and break the [0,1] domain; picking ROMMRGB_Linear would skip
	// gamma but apply a Rec.709 → ROMM matrix that warps the encoded
	// vector.  Either produces wrong normals.  See NormalMap.h for
	// the full rationale.
	const RISEPel encoded = pNormalMap.GetColor( ri );

	// Decode RGB in [0,1] to a tangent-space normal in [-1,1].  The
	// glTF spec scales only the xy components, then reconstructs z so
	// the resulting vector is on the unit hemisphere; this avoids
	// "flat" looks when scale << 1 and avoids producing inward-facing
	// normals when scale > 1.
	Scalar nx = (Scalar( 2 ) * encoded.r - Scalar( 1 )) * dScale;
	Scalar ny = (Scalar( 2 ) * encoded.g - Scalar( 1 )) * dScale;
	Scalar nzSqr = Scalar( 1 ) - nx*nx - ny*ny;
	Scalar nz = (nzSqr > 0) ? std::sqrt( nzSqr ) : Scalar( 0 );

	// Build the world-space tangent / bitangent.  Prefer the imported
	// per-vertex TANGENT (correct across UV seams and mirrored regions);
	// fall back to the ONB-derived tangents when the source had none.
	Vector3 T, B;
	const Vector3 N = ri.vNormal;
	if( ri.bHasTangent ) {
		// Best path: imported per-vertex TANGENT.  Honours mirrored
		// UVs via the bitangent sign (asset-author intent preserved).
		T = ri.vTangent;
		// Re-orthogonalise against N so floating-point drift in the
		// tangent doesn't tilt the bitangent off the surface plane.
		T = Vector3Ops::Normalize( T - N * Vector3Ops::Dot( T, N ) );
		// glTF 2.0 §3.7.2.1.4: B = cross(N, T) * tangent.w.  Source
		// assets that ship TANGENT.w are designed against this exact
		// formula -- swapping the cross order silently fixes one
		// rendering symptom (mirror inversion) while creating others.
		// If you see mirror artefacts, the issue is almost certainly
		// upstream of this line: confirm `flip_v TRUE` on
		// gltfmesh_geometry (or that the glTF loader's default is
		// flipping V) so RISE's V-down TexturePainter sampling lines
		// up with the asset's V-up authoring convention.  Object-level
		// orientation flips (e.g. `scale -1 1 1`) are handled via
		// m_tangentFrameSign in Object::IntersectRay, which folds the
		// transform handedness into ri.bitangentSign before this line
		// sees it.
		B = Vector3Ops::Cross( N, T ) * ri.bitangentSign;
	} else if( ri.derivatives.valid ) {
		// No imported TANGENT, but the geometry populated UV-derived
		// dpdu / dpdv during intersection (triangle mesh with at least
		// one TEXCOORD_0 vertex set).  These are the correct tangent
		// frame in the absence of authored TANGENT data: dpdu points
		// along +U in world space, dpdv along +V, and {dpdu, dpdv, N}
		// span the tangent plane.  Using these is qualitatively
		// correct on any connected UV chart -- the only failure mode
		// is across mirrored UV seams, where authored TANGENT.w is the
		// only signal that recovers the chirality flip (and that
		// signal is what's missing here).  Asset authors can fix that
		// by re-exporting with TANGENT included.
		//
		// Project dpdu onto the tangent plane (drop any N component
		// from float drift), normalise, then derive the bitangent from
		// the right-handed cross product.  This gives a consistent,
		// orientation-stable frame across triangles that share UV
		// charts -- much better than the ONB fallback below, which is
		// rotated arbitrarily relative to UV.
		T = Vector3Ops::Normalize(
			ri.derivatives.dpdu - N * Vector3Ops::Dot( ri.derivatives.dpdu, N ) );
		// `* ri.bitangentSign` (doc 89 slice C).  With no imported TANGENT the sign
		// carries only ONE thing -- the object transform's handedness, folded in
		// unconditionally by Object::IntersectRay -- and this cross product is
		// precisely what a negative determinant flips: N comes through the
		// inverse-transpose while dpdu comes through the forward matrix, so under a
		// reflection `cross(N_world, T_world)` points opposite the transformed
		// original bitangent.  Omitting it inverts the normal map's green channel on
		// every mirrored asset that carries derivatives but no TANGENT accessor --
		// i.e. every lathe / sweep / skin bake, which is the asset class `mirror`
		// exists to serve.  For an un-mirrored object the sign is +1 and this is
		// byte-identical to the previous expression.
		B = Vector3Ops::Cross( N, T ) * ri.bitangentSign;
	} else if( ModifierFrame::HasCoherentTangent( ri ) ) {
		// P2 fix (docs/CLOTH_FABRIC_DESIGN.md 9.9 fix round): no imported
		// TANGENT and no `ri.derivatives` (e.g. ClippedPlaneGeometry, which
		// writes a geometry-supplied shading tangent but by design never
		// populates `ri.derivatives` at all -- section 9.1), but the hit
		// still carries a COHERENT, geometry-supplied tangent.
		// Object::IntersectRay / CSGObject::IntersectRay already built
		// `ri.onb` from exactly that tangent (via CreateFromWU, including
		// this round's mirrored-transform V-sign correction), so
		// `ri.onb.u()/v()` here are NOT the arbitrary CreateFromW frame the
		// last-ditch warning below describes -- they are the surface's own
		// UV/fiber-aligned frame, already correctly signed.  Use them
		// directly, no warning.
		//
		// The predicate is ModifierFrame::HasCoherentTangent -- the SAME
		// flag pair Object::IntersectRay branches on to build that frame
		// (bShadingTangentFromGeometry, OR the hair-only bHasShadingTangent)
		// -- not `bHasShadingTangent` alone: an SDF heightfield hit sets only
		// the former, and gating on the latter sent it to the last-ditch
		// branch below, whose VALUES are identical (same onb.u()/v()) but
		// whose once-per-process warning fired on exactly such a hit
		// (relief-modifier fix round 1 residual, closed there).
		//
		// FIX ROUND 2, P2-B -- what "no warning here" actually claims for
		// the SDF-heightfield case, precisely (round 1's "false positive on
		// exactly that hit" overstated it): SDFGeometry's heightfield mode
		// parameterises `ptCoord` from the OBJECT-space hit point
		// (SDFGeometry.cpp, the `m_isHeightfield` branch of IntersectRay --
		// `(hp.x+R)/2R, (hp.y+R)/2R`), while the coherent tangent
		// `ri.onb.u()/v()` traces back to Object::IntersectRay's fallback
		// sub-case (no real supplied tangent), which projects WORLD-X --
		// not the object-space +X the UV is actually built from -- into the
		// world-space shading-normal plane.  Those two agree only when the
		// instance's linear part maps object +X to world +X, i.e. no
		// rotation, shear, or orientation-reversing (negative) scale; a
		// positive uniform or non-uniform scale and translation are fine.
		// On an UNROTATED SDF-heightfield instance suppressing the warning
		// is correct: the values genuinely are the UV-aligned frame. On a
		// ROTATED one, `onb.u()/v()` is no longer aligned with the
		// heightfield's own U axis, a normal map applied through this
		// branch is shaded against the wrong basis, and the diagnostic gap
		// this branch's silence creates is real, not cosmetic -- nothing
		// here fixes that misalignment, only ceases to warn about the
		// unrotated case where there was nothing to warn about.  The VALUES
		// are unchanged either way; only the disclosure is corrected.
		T = ri.onb.u();
		B = ri.onb.v();
	} else {
		// Last-ditch fallback: no TANGENT, no surface derivatives, and no
		// geometry-supplied shading tangent (some non-triangle geometry, or
		// future geometry types that populate none of the three).
		// ri.onb.u()/v() are an arbitrary tangent frame derived from N
		// alone; correct only when the normal map happens to be authored
		// against an ONB-aligned UV space (which is essentially never).
		// Fire the warning so the user knows what's gone wrong.
		if( !g_warnedNoFrame.exchange( true ) ) {
			GlobalLog()->PrintEasyWarning(
				"NormalMap modifier: hit has neither imported TANGENT, valid "
				"surface derivatives (ri.derivatives.valid), nor a geometry-"
				"supplied shading tangent.  Falling "
				"back to ONB-derived tangents, which is correct only when the "
				"normal map's UV axes happen to align with the arbitrary ONB "
				"frame -- i.e. essentially never.  Re-export the source asset "
				"with TANGENT included, or attach the modifier to a triangle-"
				"mesh geometry (which populates derivatives).  This warning "
				"fires once per process; subsequent fallbacks are silent." );
		}
		T = ri.onb.u();
		B = ri.onb.v();
	}

	// World-space perturbed normal = T*nx + B*ny + N*nz, normalized.
	const Vector3 perturbed = Vector3Ops::Normalize(
		T * nx + B * ny + N * nz );

	// Rebuild the ONB so SPFs (refraction / reflection) sample around the
	// perturbed normal, not the original geometric one.  The rebuild body
	// -- project the CURRENT u into the new normal's tangent plane,
	// CreateFromWU, restore the incoming handedness with FlipV, fall back
	// to CreateFromW on a degenerate projection -- lives in
	// ModifierFrame::RebuildPreservingTangent, which carries the full
	// rationale for BOTH corrections it encodes (geometry-supplied tangent
	// preservation; the mirrored-instance FlipV fix).
	//
	// The GATE stays here, and is deliberately NOT inside the helper: when
	// the hit carries no coherent tangent frame at all
	// (ModifierFrame::HasCoherentTangent false) this modifier rebuilds with
	// a plain CreateFromW, which is byte-identical to its behaviour before
	// the tangent fix existed.  The predicate -- not `ri.bHasShadingTangent`
	// alone, which is only the sub-case that ALSO supplies a real tangent
	// vector -- is what mirrors Object::IntersectRay's coherent-frame
	// branch; see its comment for the SDFGeometry-heightfield case the bare
	// flag misses.  (The T/B selection above now keys on the SAME
	// predicate, not the bare flag: three tangent-source branches --
	// imported TANGENT, UV-derived dpdu/dpdv, and
	// `ModifierFrame::HasCoherentTangent` for a geometry-supplied coherent
	// tangent with neither of the first two -- feed a rebuild gated on that
	// identical predicate, so an SDF-heightfield hit takes the third branch
	// (T = ri.onb.u(), B = ri.onb.v(), the same values the last-ditch
	// fallback would have used) and the rebuild below.  The last-ditch
	// branch, and its once-per-process warning, now fires only on a hit
	// that is genuinely tangent-less by all three tests -- fix round 2,
	// P1, closing the round-1 residual this paragraph used to describe.)
	// GlintModifier makes the opposite choice for its own reasons -- see
	// the helper's header comment.
	if( ModifierFrame::HasCoherentTangent( ri ) ) {
		ModifierFrame::RebuildPreservingTangent( ri, perturbed );
	} else {
		ri.vNormal = perturbed;
		ri.onb.CreateFromW( ri.vNormal );
	}
}
