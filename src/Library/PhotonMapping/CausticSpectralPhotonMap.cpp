//////////////////////////////////////////////////////////////////////
//
//  CausticSpectralPhotonMap.cpp - Implements the caustic photon map 
//                                 made from spectra
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 8, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CausticSpectralPhotonMap.h"
#include <algorithm>
#include "../Interfaces/ILog.h"
#include "../Utilities/Color/ColorUtils.h"

using namespace RISE;
using namespace RISE::Implementation;

CausticSpectralPhotonMap::CausticSpectralPhotonMap( 
	const unsigned int max_photons,
	const IPhotonTracer* tracer
	) : 
  PhotonMapDirectionalSpectralHelper( max_photons, tracer ),
  nm_range( 1.0 )
{
}

CausticSpectralPhotonMap::~CausticSpectralPhotonMap()
{
}

// Computes the radiance estimate at a given surface position
void CausticSpectralPhotonMap::RadianceEstimate( 
		RISEPel&						rad,					// returned radiance
		const RayIntersectionGeometric&	ri,						// ray-surface intersection information
		const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
		) const
{
	rad = RISEPel( 0, 0, 0 );

	// locate the nearest photons
	PhotonDistListType heap;
	LocatePhotons( ri.ptIntersection, dGatherRadius, nMaxPhotonsOnGather, heap, 0, static_cast<int>(vphotons.size())-1 );

	if( heap.size() > nMinPhotonsOnGather )
	{
		XYZPel	sumPel( 0, 0, 0 );

		// They haven't been sorted yet, since the list isn't full
		if( heap.size() < nMaxPhotonsOnGather ) {
			std::make_heap( heap.begin(), heap.end() );
		}

		const Scalar farthest_away = heap[0].distance;
		// A zero-radius neighborhood has no finite-area density estimate.
		if( farthest_away <= 0 ) return;
		const Scalar maxNDist = farthest_away * dEllipseRatio;

		// Sum irradiance from all photons
		PhotonDistListType::const_iterator i, e;

		for( i=heap.begin(), e=heap.end(); i!=e; i++ )
		{
			const SpectralPhoton& p = i->element;

			// The photon dir call and following if can be omitted (for speed)
			// if the scene does not have any thin surfaces.
			// Thin-surface clamp uses the GEOMETRIC normal — same-
			// physical-surface test (Jensen 2001 §6.1).
			const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, ri.ptIntersection );
			const Scalar pcos = Vector3Ops::Dot( vec, ri.vGeomNormal );

			if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
				const Vector3 vPhotonDir = p.incomingDirection;
				const Scalar response = PathVertexEval::RadianceShadingNormalFactor( ri.vNormal, ri.vGeomNormal, vPhotonDir );
				if( response > 0 ) {
					// Compute XYZ valye from spectra
					XYZPel thisNM( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( thisNM, p.nm ) ) {
						sumPel = sumPel + (thisNM * (p.power * response) * brdf.valueNM(vPhotonDir,ri,p.nm));
					}
				}
			}
		}

		// I chose not to filter this, because some blurring with the spectral photonmap is good, perhaps
		// I will change my mind later.
		rad = RISEPel( sumPel ) * (samplingLaw.IntegralScale()/(PI*farthest_away));
	}
}

// Computes the radiance estimate at a given surface position for the given wavelength
void CausticSpectralPhotonMap::RadianceEstimateNM( 
			const Scalar					nm,						// wavelength for the estimate
			Scalar&							rad,					// returned radiance for the particular wavelength
			const RayIntersectionGeometric&	ri,						// ray-surface intersection information
			const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
			) const
{
	rad = 0;
	const Scalar mass=samplingLaw.WindowMass(nm,nm_range);
	if(mass<=0)return;

	// locate the nearest photons
	PhotonDistListType heap;
	LocatePhotons( ri.ptIntersection, dGatherRadius, nMaxPhotonsOnGather, heap, 0, static_cast<int>(vphotons.size())-1 );

	if( heap.size() > nMinPhotonsOnGather )
	{
		// They haven't been sorted yet, since the list isn't full
		if( heap.size() < nMaxPhotonsOnGather ) {
			std::make_heap( heap.begin(), heap.end() );
		}

		const Scalar farthest_away = (heap.size()<nMaxPhotonsOnGather ? dGatherRadius : heap[0].distance);
		// A zero-radius neighborhood has no finite-area density estimate.
		if( farthest_away <= 0 ) return;
		const Scalar maxNDist = farthest_away * dEllipseRatio;

		// Sum irradiance from all photons
		PhotonDistListType::const_iterator i, e;

		for( i=heap.begin(), e=heap.end(); i!=e; i++ )
		{
			const SpectralPhoton& p = (*i).element;

			// The photon dir call and following if can be omitted (for speed)
			// if the scene does not have any thin surfaces.
			// Thin-surface clamp uses the GEOMETRIC normal — same-
			// physical-surface test (Jensen 2001 §6.1).
			const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, ri.ptIntersection );
			const Scalar pcos = Vector3Ops::Dot( vec, ri.vGeomNormal );

			if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
				const Vector3 vPhotonDir = p.incomingDirection;
				const Scalar response = PathVertexEval::RadianceShadingNormalFactor( ri.vNormal, ri.vGeomNormal, vPhotonDir );
				if( response > 0 ) {
					// Only take samples that are within the range we want
					if( fabs(p.nm-nm) <= nm_range ) {
						rad += (p.power * response) * brdf.valueNM( vPhotonDir, ri, nm );
					}
				}
			}
		}

		// I chose not to filter this, because some blurring with the spectral photonmap is good, perhaps
		// I will change my mind later.
		rad /= (PI*farthest_away)*mass;
	}
}

bool CausticSpectralPhotonMap::Store(Scalar power,Scalar nm,const Point3& position,const Vector3& direction)
{
 return StoreSpectral(power,nm,position,direction);
}
void CausticSpectralPhotonMap::Serialize(IWriteBuffer& buffer)const
{
 WriteSpectralMap(buffer,3,nm_range);
}
bool CausticSpectralPhotonMap::DeserializeChecked(IReadBuffer& buffer)
{
 return ReadSpectralMap(buffer,3,nm_range);
}
void CausticSpectralPhotonMap::Deserialize(IReadBuffer& buffer)
{
 DeserializeChecked(buffer);
}
