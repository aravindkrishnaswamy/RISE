//////////////////////////////////////////////////////////////////////
//
//  ShadowPhotonMap.cpp - Implements the shadow photon map
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 14, 2004
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ShadowPhotonMap.h"
#include <algorithm>
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

ShadowPhotonMap::ShadowPhotonMap( 
	const unsigned int max_photons,
	const IPhotonTracer* tracer
	) : 
  PhotonMapCore<ShadowPhoton>( max_photons, tracer )
{
}

ShadowPhotonMap::~ShadowPhotonMap()
{
}

bool ShadowPhotonMap::Store( 
	const Point3& pos,
	const bool shadow
	)
{
	if( vphotons.size() >= nMaxPhotons ) {
		return false;
	}

	ShadowPhoton p;

	p.ptPosition = pos;
	p.shadow = shadow;

	bbox.Include( p.ptPosition );
	vphotons.push_back( p );

	maxPower = nMaxPhotons*nMaxPhotons;

	return true;
}

// Computes the irradiance estimate at a given surface position
void ShadowPhotonMap::RadianceEstimate( 
		RISEPel&						rad,					// returned radiance
		const RayIntersectionGeometric&	ri,						// ray-surface intersection information
		const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
		) const
{
	rad = RISEPel( 0, 0, 0 );
}

void ShadowPhotonMap::LocateShadowPhotons( 
			const Point3&			loc,								// the location from which to search for photons
			const Scalar			maxDist,							// the maximum radius to look for photons
			const int				from,								// index to search from
			const int				to,									// index to search to
			bool&					lit_found,							// has a fully lit photon been found ?
			bool&					shadow_found						// has a fully shadowed photon been found ?
		) const
{
	// sanity check
	if( to-from < 0 ) {
		return;
	}
	
	// Compute a new median
	int median = 1;

	while( (4*median) <= (to-from+1) ) {
		median += median;
	}

	if( (3*median) <= (to-from+1) ) {
		median += median;
		median += from - 1;
	} else {
		median = to-median + 1;
	}

	// Compute the distance to the photon
	Vector3 v = Vector3Ops::mkVector3( loc, vphotons[ median ].ptPosition );
	Scalar distanceToPhoton = Vector3Ops::SquaredModulus(v);

	if( distanceToPhoton < maxDist )
	{
		// Check this shadow photon
		if( vphotons[median].shadow ) {
			shadow_found = true;
		} else {
			lit_found = true;
		}
	}

	if( shadow_found && lit_found ) {
		return;
	}

	int axis = vphotons[median].plane;

	Scalar distance2 = loc[axis] - vphotons[median].ptPosition[axis];
	Scalar sqrD2 = distance2*distance2;

	if( sqrD2 > maxDist ) {
		if( distance2 <= 0 ) {
			LocateShadowPhotons( loc, maxDist, from, median-1, lit_found, shadow_found );
		} else {
			LocateShadowPhotons( loc, maxDist, median+1, to, lit_found, shadow_found );
		}
	}

	// Search both sides of the tree
	if( sqrD2 < maxDist ) {
		LocateShadowPhotons( loc, maxDist, from, median-1, lit_found, shadow_found );
		LocateShadowPhotons( loc, maxDist, median+1, to, lit_found, shadow_found );
	}
}

//! Estimates the shadowing amount
void ShadowPhotonMap::ShadowEstimate( 
	char&					shadow,					///< [in] 0 = totally lit, 1 = totally shadowed, 2 = pieces of both
	const Point3&			pos						///< [in] Surface position
	) const
{
	// locate the nearest photons
	bool shadow_found = false;
	bool lit_found = false;

	LocateShadowPhotons( pos, dGatherRadius, 0, vphotons.size()-1, lit_found, shadow_found );

	shadow = 1;

	if( shadow_found && !lit_found ) {
		shadow = 1;
	} else if( !shadow_found && lit_found ) {
		shadow = 0;
	} else if( shadow_found && lit_found ) {
		shadow = 2;
	}
}


void ShadowPhotonMap::Serialize( 
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
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( ShadowPhoton ) * vphotons.size() );
	buffer.setUInt( vphotons.size() );

	for( unsigned int i=0; i<vphotons.size(); i++ ) {
		const ShadowPhoton& p = vphotons[i];
		Point3Ops::Serialize( p.ptPosition, buffer );
		buffer.setUChar( p.plane );
		buffer.setChar( p.shadow );
	}
}

void ShadowPhotonMap::Deserialize(
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
		ShadowPhoton p;
		Point3Ops::Deserialize( p.ptPosition, buffer );
		p.plane = buffer.getUChar();
		p.shadow = !!buffer.getChar();
		vphotons.push_back( p );
	}
}

