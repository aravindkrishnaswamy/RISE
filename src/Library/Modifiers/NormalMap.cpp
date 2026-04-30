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
	// conversion -- in today's RISE that means `color_space
	// ROMMRGB_Linear` on the png_painter (RISEPel == ROMMRGBPel, see
	// Color.h, so ROMMRGB_Linear is a verbatim store).  Picking sRGB
	// would gamma-decode the bytes and break the [0,1] domain;
	// picking Rec709RGB_Linear would skip gamma but still apply a
	// Rec709 -> ROMM matrix that warps the encoded vector.  Either
	// produces subtly-wrong normals.  See NormalMap.h for the full
	// rationale.
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
		B = Vector3Ops::Cross( N, T );
	} else {
		// Last-ditch fallback: no TANGENT and no surface derivatives
		// (some non-triangle geometry, or future geometry types that
		// don't populate derivatives.valid).  ri.onb.u()/v() are an
		// arbitrary tangent frame derived from N alone; correct only
		// when the normal map happens to be authored against an
		// ONB-aligned UV space (which is essentially never).  Fire the
		// warning so the user knows what's gone wrong.
		if( !g_warnedNoFrame.exchange( true ) ) {
			GlobalLog()->PrintEasyWarning(
				"NormalMap modifier: hit has neither imported TANGENT nor valid "
				"surface derivatives (ri.derivatives.valid).  Falling back to "
				"ONB-derived tangents, which is correct only when the normal "
				"map's UV axes happen to align with the arbitrary ONB frame -- "
				"i.e. essentially never.  Re-export the source asset with "
				"TANGENT included, or attach the modifier to a triangle-mesh "
				"geometry (which populates derivatives).  This warning fires "
				"once per process; subsequent fallbacks are silent." );
		}
		T = ri.onb.u();
		B = ri.onb.v();
	}

	// World-space perturbed normal = T*nx + B*ny + N*nz, normalized.
	Vector3 perturbed = Vector3Ops::Normalize(
		T * nx + B * ny + N * nz );

	ri.vNormal = perturbed;
	// Rebuild the ONB so SPFs (refraction / reflection) sample around
	// the perturbed normal, not the original geometric one.
	ri.onb.CreateFromW( ri.vNormal );
}
