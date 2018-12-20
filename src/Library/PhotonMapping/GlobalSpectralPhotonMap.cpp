//////////////////////////////////////////////////////////////////////
//
//  GlobalSpectralPhotonMap.cpp - Implements the global photon map 
//                                 made from spectra
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GlobalSpectralPhotonMap.h"
#include <algorithm>
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

GlobalSpectralPhotonMap::GlobalSpectralPhotonMap( 
	const unsigned int max_photons,
	const IPhotonTracer* tracer
	) : 
  PhotonMapDirectionalHelper<SpectralPhoton>( max_photons, tracer ), 
  nm_range( 1.0 )
{
}

GlobalSpectralPhotonMap::~GlobalSpectralPhotonMap()
{
}

// Computes the radiance estimate at a given surface position
void GlobalSpectralPhotonMap::RadianceEstimate( 
		RISEPel&				rad,							// returned radiance
		const RayIntersectionGeometric&	ri,						// ray-surface intersection information
		const IBSDF&			brdf							// BRDF of the surface to estimate irradiance from
		) const
{
	rad = RISEPel( 0, 0, 0 );

	// locate the nearest photons
	PhotonDistListType heap;
	LocatePhotons( ri.ptIntersection, dGatherRadius, nMaxPhotonsOnGather, heap, 0, vphotons.size()-1 );

	if( heap.size() > nMinPhotonsOnGather )
	{
		XYZPel	sumPel( 0, 0, 0 );

		// They haven't been sorted yet, since the list isn't full
		if( heap.size() < nMaxPhotonsOnGather ) {
			std::make_heap( heap.begin(), heap.end() );
		}

		const Scalar farthest_away = (heap.size()<nMaxPhotonsOnGather ? dGatherRadius : heap[0].distance);
		const Scalar maxNDist = farthest_away * dEllipseRatio;

		// Sum irradiance from all photons
		PhotonDistListType::const_iterator i, e;

		for( i=heap.begin(), e=heap.end(); i!=e; i++ )
		{
			const SpectralPhoton& p = (*i).element;

			// The photon dir call and following if can be omitted (for speed)
			// if the scene does not have any thin surfaces
			const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, ri.ptIntersection );
			const Scalar pcos = Vector3Ops::Dot( vec, ri.vNormal );

			if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
				const Vector3 vPhotonDir = PhotonDir(p.theta,p.phi);
				if( Vector3Ops::Dot(vPhotonDir,ri.vNormal) > 0 ) {
					// Compute XYZ valye from spectra
					XYZPel thisNM( 0, 0, 0 );
					if( ColorUtils::XYZFromNM( thisNM, p.nm ) ) {
						sumPel = sumPel + (thisNM * p.power * XYZPel(brdf.value( vPhotonDir, ri  )));
					}
				}
			}
		}

		rad = RISEPel( sumPel ) * (1.0/(PI*farthest_away));
	}
}

// Computes the radiance estimate at a given surface position for the given wavelength
void GlobalSpectralPhotonMap::RadianceEstimateNM( 
			const Scalar			nm,								// wavelength for the estimate
			Scalar&					rad,							// returned radiance for the particular wavelength
			const RayIntersectionGeometric&	ri,						// ray-surface intersection information
			const IBSDF&			brdf							// BRDF of the surface to estimate irradiance from
			) const
{
	rad = 0;

	// locate the nearest photons
	PhotonDistListType heap;
	LocatePhotons( ri.ptIntersection, dGatherRadius, nMaxPhotonsOnGather, heap, 0, vphotons.size()-1 );

	if( heap.size() > nMinPhotonsOnGather )
	{
		// They haven't been sorted yet, since the list isn't full
		if( heap.size() < nMaxPhotonsOnGather ) {
			std::make_heap( heap.begin(), heap.end() );
		}

		const Scalar farthest_away = (heap.size()<nMaxPhotonsOnGather ? dGatherRadius : heap[0].distance);
		const Scalar maxNDist = farthest_away * dEllipseRatio;

		// Sum irradiance from all photons
		PhotonDistListType::const_iterator i, e;

		for( i=heap.begin(), e=heap.end(); i!=e; i++ )
		{
			const SpectralPhoton& p = (*i).element;

			// The photon dir call and following if can be omitted (for speed)
			// if the scene does not have any thin surfaces
			const Vector3 vec = Vector3Ops::mkVector3( p.ptPosition, ri.ptIntersection );
			const Scalar pcos = Vector3Ops::Dot( vec, ri.vNormal );

			if( (pcos < maxNDist) && (pcos > -maxNDist) ) {
				const Vector3 vPhotonDir = PhotonDir(p.theta,p.phi);
				if( Vector3Ops::Dot(vPhotonDir,ri.vNormal) > 0 ) {
					// Only take samples that are within the range we want
					if( fabs(p.nm-nm) <= nm_range ) {
						rad += p.power * brdf.valueNM(vPhotonDir, ri, nm);
					}
				}
			}
		}

		rad /= PI*farthest_away;
	}
}

bool GlobalSpectralPhotonMap::Store( const Scalar power, const Scalar nm, const Point3& pos, const Vector3& dir )
{
	if( vphotons.size() > nMaxPhotons ) {
		return false;
	}

	SpectralPhoton p;

	p.ptPosition = pos;
	p.power = power;
	p.nm = nm;

	int theta = int( acos( dir.z ) * (256.0 / PI) );
	theta = theta > 255 ? 255 : theta;
	p.theta = (unsigned char)(theta);

	int phi = int( atan2( dir.y, dir.x ) * (256.0/TWO_PI) );
	phi = phi > 255 ? 255 : phi;
	phi = phi < 0 ? phi+256 : phi;

	p.phi = (unsigned char)(phi);

	bbox.Include( p.ptPosition );
	vphotons.push_back( p );
	maxPower = r_max( maxPower, power );

	return true;
}

void GlobalSpectralPhotonMap::Serialize( 
	IWriteBuffer&			buffer					///< [in] Buffer to serialize to
	) const
{
	buffer.ResizeForMore( sizeof( unsigned int ) * 4 + sizeof( Scalar ) * 3 );

	buffer.setUInt( nMaxPhotons );
	buffer.setUInt( nPrevScale );
	buffer.setDouble( dGatherRadius );
	buffer.setDouble( dEllipseRatio );
	buffer.setUInt( nMinPhotonsOnGather );
	buffer.setUInt( nMaxPhotonsOnGather );
	buffer.setDouble( maxPower );

	// Serialize the bounding box
	bbox.Serialize( buffer );

	// Serialize number of stored photons
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( SpectralPhoton ) * vphotons.size() );
	buffer.setUInt( vphotons.size() );

	for( unsigned int i=0; i<vphotons.size(); i++ ) {
		const SpectralPhoton& p = vphotons[i];
		Point3Ops::Serialize( p.ptPosition, buffer );
		buffer.setUChar( p.plane );
		buffer.setDouble( p.power );
		buffer.setUChar( p.theta );
		buffer.setUChar( p.phi );
		buffer.setDouble( p.nm );
	}
}

void GlobalSpectralPhotonMap::Deserialize(
	IReadBuffer&			buffer					///< [in] Buffer to deserialize from
	)
{
	nMaxPhotons = buffer.getUInt();
	nPrevScale = buffer.getUInt();
	dGatherRadius = buffer.getDouble();
	dEllipseRatio = buffer.getDouble();
	nMinPhotonsOnGather = buffer.getUInt();
	nMaxPhotonsOnGather = buffer.getUInt();
	maxPower = buffer.getDouble();

	bbox.Deserialize( buffer );

	const unsigned int numphot = buffer.getUInt();
	vphotons.reserve( numphot );

	for( unsigned int i=0; i<numphot; i++ ) {
		SpectralPhoton p;
		Point3Ops::Deserialize( p.ptPosition, buffer );
		p.plane = buffer.getUChar();
		p.power = buffer.getDouble();
		p.theta = buffer.getUChar();
		p.phi = buffer.getUChar();
		p.nm = buffer.getDouble();
		vphotons.push_back( p );
	}
}

