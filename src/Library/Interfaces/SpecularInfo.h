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
		bool    valid;			///< True if this info was successfully computed

		SpecularInfo() :
		isSpecular( false ),
		canRefract( false ),
		ior( 1.0 ),
		valid( false )
		{
		}
	};
}

#endif
