//////////////////////////////////////////////////////////////////////
//
//  TranslucentPelPhotonMap.cpp - Implements the caustic photon map of type PEL
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
#include "TranslucentPelPhotonMap.h"
#include <algorithm>
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

TranslucentPelPhotonMap::TranslucentPelPhotonMap( 
	const unsigned int max_photons,
	const IPhotonTracer* tracer
	) : 
  PhotonMapCore<TranslucentPhoton>( max_photons, tracer )
{
}

TranslucentPelPhotonMap::~TranslucentPelPhotonMap()
{
}

bool TranslucentPelPhotonMap::Store( const RISEPel& power, const Point3& pos )
{
	if( vphotons.size() >= nMaxPhotons ) {
		return false;
	}

	if( ColorMath::MaxValue(power) <= 0 ) {
		return false;
	}

	TranslucentPhoton p;

	p.ptPosition = pos;
	p.power = power;

	bbox.Include( p.ptPosition );
	vphotons.push_back( p );
	maxPower = r_max( maxPower, ColorMath::MaxValue(power) );

	return true;
}

// Computes the irradiance estimate at a given surface position
void TranslucentPelPhotonMap::RadianceEstimate( 
		RISEPel&						rad,					// returned radiance
		const RayIntersectionGeometric&	ri,						// ray-surface intersection information
		const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
		) const
{
	rad = RISEPel( 0, 0, 0 );

	// locate the nearest photons
	PhotonDistListType heap;
	LocatePhotons( ri.ptIntersection, dGatherRadius, nMaxPhotonsOnGather, heap, 0, vphotons.size()-1 );

	if( heap.size() > nMinPhotonsOnGather )
	{
		// They haven't been sorted yet, since the list isn't full
		if( heap.size() < nMaxPhotonsOnGather ) {
			std::make_heap( heap.begin(), heap.end() );
		}

		const Scalar farthest_away = heap[0].distance;

		// Sum irradiance from all photons
		PhotonDistListType::const_iterator i, e;
		for( i=heap.begin(), e=heap.end(); i!=e; i++ ) {
			rad = rad + i->element.power;
		}

		// Broken, should fix this
		rad = rad * brdf.value( ri.vNormal, ri ) * (1.0/(PI*farthest_away));
	}
}

void TranslucentPelPhotonMap::Serialize( 
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
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( TranslucentPhoton ) * vphotons.size() );
	buffer.setUInt( vphotons.size() );

	for( unsigned int i=0; i<vphotons.size(); i++ ) {
		const TranslucentPhoton& p = vphotons[i];
		Point3Ops::Serialize( p.ptPosition, buffer );
		buffer.setUChar( p.plane );
		ColorUtils::SerializeRGBPel( p.power, buffer );
	}
}

void TranslucentPelPhotonMap::Deserialize(
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
		TranslucentPhoton p;
		Point3Ops::Deserialize( p.ptPosition, buffer );
		p.plane = buffer.getUChar();
		ColorUtils::DeserializeRGBPel( p.power, buffer );
		vphotons.push_back( p );
	}
}

void TranslucentPelPhotonMap::ScalePhotonPower( const Scalar scale )
{
	for( unsigned int i=nPrevScale; i<vphotons.size(); i++ ) {
		vphotons[i].power = vphotons[i].power * scale;
	}

	nPrevScale = vphotons.size();

	PhotonMapCore<TranslucentPhoton>::ScalePhotonPower( scale );
}

