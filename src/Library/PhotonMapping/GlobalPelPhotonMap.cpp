//////////////////////////////////////////////////////////////////////
//
//  GlobalPelPhotonMap.cpp - Implements the global photon map of type PEL
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 14, 2002
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
#include "GlobalPelPhotonMap.h"

using namespace RISE;
using namespace RISE::Implementation;

GlobalPelPhotonMap::GlobalPelPhotonMap( 
	const unsigned int max_photons,
	const IPhotonTracer* tracer
	) : 
  PhotonMapDirectionalPelHelper<IrradPhoton>( max_photons, tracer ),
  bPrecomputedIrrad( false )
{
}

GlobalPelPhotonMap::~GlobalPelPhotonMap()
{
}

void GlobalPelPhotonMap::PrecomputeIrradiance(
	const unsigned int apart,							// How far apart should precomputed irradiances be?
	IProgressCallback* pFunc							// Progress callback
	)
{
	PhotonListType	irradphotons;
	irradphotons.reserve( vphotons.size() / apart );

	bbox = BoundingBox( Point3(INFINITY,INFINITY,INFINITY), Point3(-INFINITY,-INFINITY,-INFINITY) );

	const unsigned int total = vphotons.size();

	if( pFunc ) {
		pFunc->SetTitle( "Precomputing global photonmap irradiances: " );
	}

	for( unsigned int i=0; i<total; i+=apart ) {
		IrradPhoton p = vphotons[i];
		const Vector3 vNormal = PhotonDir(p.Ntheta,p.Nphi);
		IrradianceEstimateFromSearch( p.irrad, p.ptPosition, vNormal );

		bbox.Include( p.ptPosition );

		irradphotons.push_back( p );

		if( pFunc ) {
			if( i%1000 == 0 ) {
				pFunc->Progress( double(i), double(total) );
			}
		}
	}

	irradphotons.swap( vphotons );

	// Balance again
	Balance();

	if( pFunc ) {
		pFunc->Progress( 1.0, 1.0 );
	}

	bPrecomputedIrrad = true;
}

// Computes the radiance estimate at a given surface position
void GlobalPelPhotonMap::RadianceEstimate( 
		RISEPel&						rad,					// returned radiance
		const RayIntersectionGeometric&	ri,						// ray-surface intersection information
		const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
		) const
{
	if( bPrecomputedIrrad ) {
		// We have precomputed irradiance, so just find the nearest one
		IrradPhoton irrad; // have to do this because of a bug in the GCC compiler
		distance_container<IrradPhoton> nearest( irrad, INFINITY);
		LocateNearestPhoton( ri.ptIntersection, ri.vNormal, dGatherRadius, nearest );

		if( nearest.distance < INFINITY ) {
			rad = nearest.element.irrad * brdf.value( ri.vNormal, ri );
			return;
		} 

		// Otherwise, we failed to find a nearest photon, so do a normal search
	}

	RadianceEstimateFromSearch( rad, ri, brdf );
}

bool GlobalPelPhotonMap::Store( const RISEPel& power, const Point3& pos, const Vector3& N, const Vector3& dir )
{
	if( vphotons.size() >= nMaxPhotons ) {
		return false;
	}

	IrradPhoton p;

	p.ptPosition = pos;
	p.power = power;

	int theta = int( acos( dir.z ) * (256.0 / PI) );
	p.theta = theta > 255 ? 255 : (unsigned char)(theta);

	int phi = int( atan2( dir.y, dir.x ) * (256.0/TWO_PI) );
	phi = phi > 255 ? 255 : phi;
	p.phi = phi < 0 ? (unsigned char)(phi+256) : (unsigned char)(phi);

	int Ntheta = int( acos( N.z ) * (256.0 / PI) );
	p.Ntheta = Ntheta > 255 ? 255 : (unsigned char)(Ntheta);

	int Nphi = int( atan2( N.y, N.x ) * (256.0/TWO_PI) );
	Nphi = Nphi > 255 ? 255 : Nphi;
	p.Nphi = Nphi < 0 ? (unsigned char)(Nphi+256) : (unsigned char)(Nphi);

	bbox.Include( p.ptPosition );
	vphotons.push_back( p );
	maxPower = r_max( maxPower, ColorMath::MaxValue(power) );

	return true;
}

void GlobalPelPhotonMap::Serialize( 
	IWriteBuffer&			buffer					///< [in] Buffer to serialize to
	) const
{
	buffer.ResizeForMore( sizeof( unsigned int ) * 4 + sizeof( Scalar ) * 3 + 1 );

	buffer.setUInt( nMaxPhotons );
	buffer.setUInt( nPrevScale );
	buffer.setDouble( dGatherRadius );
	buffer.setDouble( dEllipseRatio );
	buffer.setUInt( nMinPhotonsOnGather );
	buffer.setUInt( nMaxPhotonsOnGather );
	buffer.setDouble( maxPower );
	buffer.setUChar( bPrecomputedIrrad );

	// Serialize the bounding box
	bbox.Serialize( buffer );

	// Serialize number of stored photons
	buffer.ResizeForMore( sizeof( unsigned int ) + sizeof( IrradPhoton ) * vphotons.size() );
	buffer.setUInt( vphotons.size() );

	for( unsigned int i=0; i<vphotons.size(); i++ ) {
		const IrradPhoton& p = vphotons[i];
		Point3Ops::Serialize( p.ptPosition, buffer );
		buffer.setUChar( p.plane );
		ColorUtils::SerializeRGBPel( p.power, buffer );
		buffer.setUChar( p.theta );
		buffer.setUChar( p.phi );
		ColorUtils::SerializeRGBPel( p.irrad, buffer );
		buffer.setUChar( p.Ntheta );
		buffer.setUChar( p.Nphi );
	}
}

void GlobalPelPhotonMap::Deserialize(
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
	bPrecomputedIrrad = !!buffer.getUChar();

	bbox.Deserialize( buffer );

	const unsigned int numphot = buffer.getUInt();
	vphotons.reserve( numphot );

	for( unsigned int i=0; i<numphot; i++ ) {
		IrradPhoton p;
		Point3Ops::Deserialize( p.ptPosition, buffer );
		p.plane = buffer.getUChar();
		ColorUtils::DeserializeRGBPel( p.power, buffer );
		p.theta = buffer.getUChar();
		p.phi = buffer.getUChar();
		ColorUtils::DeserializeRGBPel( p.irrad, buffer );
		p.Ntheta = buffer.getUChar();
		p.Nphi = buffer.getUChar();
		vphotons.push_back( p );
	}
}

