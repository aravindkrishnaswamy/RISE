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
//    1. TANGENT PRESERVATION.  When the hit carries a COHERENT tangent
//       frame (HairGeometry's fibre tangent, SDFGeometry's heightfield
//       mode, an analytic primitive's dpdu, a UV-mapped mesh's dpdu, an
//       imported glTF TANGENT), `ri.onb.u()` at modifier time ALREADY IS
//       that tangent -- Object::IntersectRay / CSGObject::IntersectRay
//       promoted it to world space and built the ONB from it
//       (CreateFromWU) before any modifier ran.  A plain CreateFromW
//       discards it for an arbitrary canonical-axis pick and breaks the
//       coherent frame HairBSDF and anisotropic `tangent_rotation`
//       depend on.  So: project the CURRENT u into the new normal's
//       tangent plane and rebuild with CreateFromWU, falling back to
//       CreateFromW only when that projection degenerates (the perturbed
//       normal swung onto the old tangent).
//
//       WHICH FLAG SAYS "COHERENT" is `HasCoherentTangent` below, NOT
//       `ri.bHasShadingTangent` alone -- see that predicate's comment.
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
//  all.  BumpMap, NormalMap and ReliefModifier gate it on
//  `HasCoherentTangent( ri )` (when that is false they call CreateFromW
//  directly, which is byte-identical to their pre-tangent-fix
//  behaviour); GlintModifier runs it unconditionally (preserving even
//  the arbitrary CreateFromW tangent across facets, so a facet field
//  does not make the anisotropic frame jitter).
//
//  BOTH POLICIES ARE CORRECT ON A COHERENT-TANGENT HIT -- there they do
//  the same thing, because that is exactly when the projection is the
//  right answer.  They differ ONLY on a TANGENT-LESS hit (predicate
//  false), where the incoming u is itself CreateFromW's arbitrary
//  canonical-axis pick: Glint keeps projecting it (continuity of an
//  arbitrary-but-stable frame across sub-pixel facets beats matching a
//  legacy value), while Bump/Normal/Relief re-run CreateFromW (exact
//  byte-compatibility with their pre-tangent-fix output, pinned by
//  HairTangentPlumbingTest tests 10 and 12).  For such a hit the two
//  produce different u/v axes (same w), so the gate stays at each call
//  site and this helper is the shared BODY, not the shared policy.
//  ReliefModifier follows BumpMap/NormalMap (it is a height-gradient
//  tilt, the same family), which is what
//  docs/RELIEF_MODIFIER_DESIGN.md 3.2 means by "verbatim from
//  NormalMap.cpp:220-234".
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
			//! Does this hit's incoming `ri.onb` carry a COHERENT tangent
			//! -- i.e. did the intersection code build it with
			//! `CreateFromWU` from a meaningful surface direction (and
			//! possibly `FlipV` it for a mirrored instance), rather than
			//! with `CreateFromW`'s arbitrary canonical-axis pick?
			//!
			//! This MIRRORS the branch condition in
			//! `Object::IntersectRay` (src/Library/Objects/Object.cpp:699,
			//! `if( ri.geometric.bShadingTangentFromGeometry )`, and the
			//! byte-identical block in `CSGObject::IntersectRay`), which
			//! is the ONLY place the coherent frame is built.  That branch
			//! keys on `bShadingTangentFromGeometry`; `bHasShadingTangent`
			//! is merely the SUB-CASE inside it (a geometry that also
			//! supplies a real fibre/UV tangent -- HairGeometry, the
			//! analytic primitives, UV-mapped meshes -- rather than taking
			//! the legacy world-X projection).  Gating a modifier's frame
			//! rebuild on `bHasShadingTangent` ALONE therefore misses
			//! SDFGeometry's heightfield mode, which sets
			//! `bShadingTangentFromGeometry` WITHOUT `bHasShadingTangent`
			//! (SDFGeometry.cpp, the `m_isHeightfield` branch of
			//! IntersectRay; the field comment at
			//! RayIntersectionGeometric.h:391-394 spells the pairing out)
			//! -- such a hit would fall to `CreateFromW`, which for the
			//! canonical heightfield case (u = +X, v = +Y, w = +Z) rotates
			//! the frame by 180 degrees (CreateFromW gives u = -X, v = -Y)
			//! and additionally discards the mirrored-instance `FlipV`.
			//!
			//! WHY THE `OR` AND NOT JUST THE FIRST FLAG.  No in-tree
			//! geometry sets `bHasShadingTangent` without also setting
			//! `bShadingTangentFromGeometry`, so today the second disjunct
			//! is unreachable -- but if a future geometry did, its ONB
			//! would NOT have been built from that tangent, and projecting
			//! the incoming u into the new tangent plane is harmless there
			//! (it preserves whatever frame the hit arrived with, which is
			//! precisely GlintModifier's continuity argument).  The OR is
			//! the conservative direction: it can only ever ADD frame
			//! preservation, never remove it.
			//!
			//! A hit with NEITHER flag keeps the legacy `CreateFromW`
			//! rebuild -- pinned byte-for-byte by
			//! HairTangentPlumbingTest tests 10 and 12.
			inline bool HasCoherentTangent(
				const RayIntersectionGeometric& ri		///< [in] The hit being modified
				)
			{
				return ri.bShadingTangentFromGeometry || ri.bHasShadingTangent;
			}

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
