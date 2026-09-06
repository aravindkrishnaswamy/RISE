//////////////////////////////////////////////////////////////////////
//
//  ModifierFrame.h - The ONE home for the shading-frame rebuild every
//    normal-perturbing IRayIntersectionModifier performs after it has
//    decided on a new shading normal.
//
//  WHY THIS EXISTS.  BumpMap, NormalMap and GlintModifier each carried
//  a byte-identical copy of the same fifteen-line block, and the block
//  is not obvious: it encodes two separate, hard-won corrections that a
//  naive `ri.onb.CreateFromW( newN )` silently undoes.
//
//    1. TANGENT PRESERVATION.  When the hit carries a geometry-supplied
//       coherent tangent (HairGeometry's fibre tangent, SDFGeometry's
//       heightfield mode, an analytic primitive's dpdu, a UV-mapped
//       mesh's dpdu, an imported glTF TANGENT), `ri.onb.u()` at modifier
//       time ALREADY IS that tangent -- Object::IntersectRay /
//       CSGObject::IntersectRay promoted it to world space and built the
//       ONB from it (CreateFromWU) before any modifier ran.  A plain
//       CreateFromW discards it for an arbitrary canonical-axis pick and
//       breaks the coherent frame HairBSDF and anisotropic
//       `tangent_rotation` depend on.  So: project the CURRENT u into
//       the new normal's tangent plane and rebuild with CreateFromWU,
//       falling back to CreateFromW only when that projection
//       degenerates (the perturbed normal swung onto the old tangent).
//
//    2. HANDEDNESS.  CreateFromWU ALWAYS emits a right-handed (u,v,w)
//       triple, but a mirrored-instance hit's incoming `ri.onb` may
//       deliberately be LEFT-handed -- Object::IntersectRay /
//       CSGObject::IntersectRay flip `v` (OrthonormalBasis3D::FlipV) to
//       correct `tangent_rotation`'s sense under a negative-determinant
//       transform (docs/CLOTH_FABRIC_DESIGN.md 9.9 fix round, P1
//       follow-on).  Rebuilding without restoring that flip silently
//       discards the mirror correction for any mirrored, tangent-bearing
//       hit that also carries a modifier.  So: capture the incoming
//       handedness (sign of u.(v x w)) and restore it after the rebuild.
//       NOT applied to the CreateFromW degenerate fallback: that branch
//       discards u entirely for an arbitrary canonical-axis pick, so
//       there is no supplied handedness left to preserve.
//
//  WHAT THIS HELPER DOES *NOT* DECIDE.  Whether to run the projection at
//  all.  BumpMap and NormalMap gate it on `ri.bHasShadingTangent` (when
//  false they call CreateFromW directly, which is byte-identical to
//  their pre-tangent-fix behaviour); GlintModifier runs it
//  unconditionally (preserving even the arbitrary CreateFromW tangent
//  across facets, so a facet field does not make the anisotropic frame
//  jitter).  Those are genuinely different, observable behaviours -- for
//  a hit with bHasShadingTangent == false the two produce different
//  u/v axes (same w) -- so the gate stays at each call site and this
//  helper is the shared BODY, not the shared policy.  ReliefModifier
//  follows BumpMap/NormalMap (it is a height-gradient tilt, the same
//  family), which is what docs/RELIEF_MODIFIER_DESIGN.md 3.2 means by
//  "verbatim from NormalMap.cpp:220-234".
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 5, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MODIFIER_FRAME_
#define MODIFIER_FRAME_

#include "../Intersection/RayIntersectionGeometric.h"

namespace RISE
{
	namespace Implementation
	{
		namespace ModifierFrame
		{
			//! Set `ri.vNormal` to `newN` and rebuild `ri.onb` about it,
			//! preserving the incoming tangent direction and handedness.
			//! See the file header for why both of those matter.
			//!
			//! `newN` MUST be unit length (every caller normalizes).
			//! Safe to call with `ri.vNormal` itself as the argument: the
			//! value is copied into a local before anything is written.
			inline void RebuildPreservingTangent(
				RayIntersectionGeometric& ri,		///< [in/out] The hit whose frame is rebuilt
				const Vector3& newN					///< [in] The new (unit) shading normal
				)
			{
				// Local copy FIRST: callers legitimately pass `ri.vNormal`
				// (BumpMap computes the perturbed normal in place), and the
				// projection below must read the new normal, not a value the
				// write-back may have aliased.
				const Vector3 n = newN;

				const Vector3 oldU = ri.onb.u();
				const Scalar oldHandedness = Vector3Ops::Dot( oldU,
					Vector3Ops::Cross( ri.onb.v(), ri.onb.w() ) );

				ri.vNormal = n;

				const Vector3 uProj = oldU - n * Vector3Ops::Dot( oldU, n );
				if( Vector3Ops::SquaredModulus( uProj ) > Scalar(1e-12) ) {
					ri.onb.CreateFromWU( n, uProj );
					if( oldHandedness < Scalar(0) ) {
						ri.onb.FlipV();
					}
				} else {
					ri.onb.CreateFromW( n );
				}
			}
		}
	}
}

#endif
