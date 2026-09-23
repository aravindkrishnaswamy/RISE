//////////////////////////////////////////////////////////////////////
//
//  CausticPelPhotonMap.cpp - Implements the caustic photon map of type PEL
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: August 23, 2002
//  Tabs: 4
//  Comments:  The code here is an implementation from Henrik Wann
//             Jensen's book Realistic Image Synthesis Using 
//             Photon Mapping.  Much of the code is influeced or
//             taken from the sample code in the back of his book.
//			   I have however used STD data structures rather than
//			   reinventing the wheel as he does.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CausticPelPhotonMap.h"
#include <algorithm>
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

CausticPelPhotonMap::CausticPelPhotonMap( 
	const unsigned int max_photons,
	const IPhotonTracer* tracer
	) : 
  PhotonMapDirectionalPelHelper<Photon>( max_photons, tracer )
{
}

CausticPelPhotonMap::~CausticPelPhotonMap()
{
}

// Computes the radiance estimate at a given surface position
void CausticPelPhotonMap::RadianceEstimate( 
		RISEPel&						rad,					// returned radiance
		const RayIntersectionGeometric&	ri,						// ray-surface intersection information
		const IBSDF& brdf, const IORStack* pIorStack // BRDF of the surface to estimate irradiance from
 ) const
{
	RadianceEstimateFromSearch( rad, ri, brdf );
}

void CausticPelPhotonMap::Serialize(IWriteBuffer& buffer)const
{
 WriteExactPrefix(buffer,1);WriteExactBody(buffer);
}
bool CausticPelPhotonMap::DeserializeChecked(IReadBuffer& buffer)
{
 if(!ReadExactPrefix(buffer,1))return false;
 ExactPacketState state;if(!ReadExactBody(buffer,state))return false;
 CommitExactBody(state);return true;
}
void CausticPelPhotonMap::Deserialize(IReadBuffer& buffer)
{
 DeserializeChecked(buffer);
}
