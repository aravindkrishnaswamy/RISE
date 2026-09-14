//////////////////////////////////////////////////////////////////////
//
//  SpecularInfo.h - Describes a material's specular (delta) behavior.
//
//  Used by both ISPF and IMaterial so that specular info can be
//  queried at either level of the interface hierarchy.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 25, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECULAR_INFO_
#define SPECULAR_INFO_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	//! Information about a material's specular (delta distribution) behavior.
	//! Used by the specular manifold sampling solver to determine the constraint
	//! type (reflection vs refraction) and IOR at each specular vertex.
	struct SpecularInfo
	{
		bool    isSpecular;		///< True if this material has delta (specular) interactions
		bool    canRefract;		///< True if refraction is possible (not a pure mirror)
		Scalar  ior;			///< Index of refraction at this point (for refraction; 1.0 if reflection-only)
		RISEPel attenuation;	///< Color attenuation for transmitted/reflected light (e.g., colored glass refractance)
		bool    valid;			///< True if this info was successfully computed
		bool    clearTransmission;	///< True ONLY for a CLEAR transmissive dielectric boundary (Dielectric / PerfectRefractor): light passes into a NON-scattering medium and out the far side.  Distinguishes a clear glass shell from a subsurface-scattering surface or a coat-over-substrate (Polished), which share isSpecular && canRefract but must BLOCK a transparent shadow ray.  Default false.
		bool    hasInterior;	///< True if this material tracks its own IOR-stack membership (Scatter/ScatterNM classify entry vs. exit from ior_stack.containsCurrent(), exactly like a refractor) even though it is NOT specular and carries no distinct index of its own (DL-46).  Example: TranslucentSPF's diffuse lampshade model -- its interior segments re-push the ENCLOSING medium's IOR unchanged, they never introduce a new numeric IOR.  `IORStackSeeding::SeedFromPoint` uses `canRefract || hasInterior` to decide whether an object needs probe-based containment tracking, and re-pushes the enclosing IOR (rather than reading `ior`) for a `hasInterior`-only entry.  Setting `canRefract` (or `isSpecular`) on such a material merely to be picked up by the seeding probe would misrepresent it to every OTHER `GetSpecularInfo` consumer (SMS chain building, dielectric-shadow-ray gating, etc.) -- use this flag instead.  Default false.

		SpecularInfo() :
		isSpecular( false ),
		canRefract( false ),
		ior( 1.0 ),
		attenuation( 1.0, 1.0, 1.0 ),
		valid( false ),
		clearTransmission( false ),
		hasInterior( false )
		{
		}
	};
}

#endif
