//////////////////////////////////////////////////////////////////////
//
//  VCMLightVertex.h - Compact persistent light-subpath vertex used
//    as the spatial query target of VCM's photon merging pass.
//
//    SmallVCM's "light vertex" is a PathVertex restricted to the
//    minimum state needed to:
//      (a) be located by a fixed-radius KD-tree query at an eye
//          vertex, and
//      (b) re-evaluate the BSDF and its forward/reverse solid-angle
//          PDFs at merge time using the stored incoming direction.
//
//    The full BDPTVertex carries orthonormal bases, path-guiding
//    training metadata, medium pointers, texture coordinates, and
//    BSSRDF flags that merging never reads; at 1920x1080 x ~10
//    bounces those dominate the memory budget.  LightVertex is a
//    lean copy that reconstructs OrthonormalBasis3D on demand from
//    the normal at merge time.
//
//    The first two fields (ptPosition and plane) must match the
//    layout expected by the VCMLightVertexKDTree<T> template so the
//    balance/query algorithms can be instantiated on this type.
//
//    Step 0 ships the declarations only; Step 3 populates the store
//    and Step 4 begins writing to LightVertex during the light-pass
//    post-walk.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 14, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VCM_LIGHT_VERTEX_
#define VCM_LIGHT_VERTEX_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "VCMRecurrence.h"

namespace RISE
{
	class IMaterial;
	class IObject;

	namespace Implementation
	{
		/// Flags packed into LightVertex::flags.
		enum LightVertexFlags
		{
			kLVF_IsDelta		= 1 << 0,	///< Sampled interaction at this vertex is a delta (specular) BSDF lobe
			kLVF_IsConnectible	= 1 << 1,	///< Material has at least one non-delta BxDF component
			kLVF_IsBSSRDFEntry	= 1 << 2,	///< Skip: non-analytic PDF, recurrence terminates here
			kLVF_HasVertexColor	= 1 << 3	///< vColor was populated from a colored mesh hit; consumers should mirror it into the reconstructed ri.bHasVertexColor
		};

		/// Compact per-vertex record stored in the VCM light vertex
		/// KD-tree.  The first two fields must match the layout the
		/// cloned KD-tree template expects.
		struct LightVertex
		{
			Point3				ptPosition;		///< REQUIRED FIRST: KD-tree builder reads [axis]
			unsigned char		plane;			///< REQUIRED SECOND: KD-tree builder writes this
			unsigned char		flags;			///< LightVertexFlags bitmask
			unsigned short		pathLength;		///< Light-subpath bounces to reach this vertex

			Vector3				normal;			///< Surface normal at the vertex
			Vector3				wi;				///< Direction from previous vertex to this one
			const IMaterial*	pMaterial;		///< For merge-time BSDF evaluation
			const IObject*		pObject;		///< For merge-time BSDF evaluation

			RISEPel				throughput;		///< Cumulative alpha_i from light origin
			VCMMisQuantities	mis;			///< dVCM/dVC/dVM at this vertex after the geometric update

			//! Per-vertex color interpolated by the geometry at hit time
			//! (linear ROMM RGB; see RISEPel).  Replayed into the
			//! reconstructed RayIntersectionGeometric on merge so the
			//! vertex-color painter sees the same color it would on a
			//! direct PT path.  bHasVertexColor mirrors the intersection
			//! field of the same name; encoded as a flag bit on `flags`
			//! to avoid a per-vertex padding hole.
			RISEPel				vColor;

			LightVertex() :
				ptPosition( 0, 0, 0 ),
				plane( 0 ),
				flags( 0 ),
				pathLength( 0 ),
				normal( 0, 0, 1 ),
				wi( 0, 0, 0 ),
				pMaterial( 0 ),
				pObject( 0 ),
				throughput( 0, 0, 0 ),
				vColor( 0, 0, 0 )
			{}
		};

		/// Spectral (NM) variant stores single-wavelength throughput
		/// and the wavelength itself so Step 10 can re-evaluate
		/// companion wavelengths against the hero-traced path.
		struct LightVertexNM : public LightVertex
		{
			Scalar				throughputNM;
			Scalar				nm;

			LightVertexNM() :
				LightVertex(),
				throughputNM( 0 ),
				nm( 0 )
			{}
		};
	}
}

#endif
