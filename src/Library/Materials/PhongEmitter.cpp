//////////////////////////////////////////////////////////////////////
//
//  PhongEmitter.cpp - Implements a Phong emitter
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PhongEmitter.h"
#include "../Utilities/GeometricUtilities.h"

using namespace RISE;
using namespace RISE::Implementation;

PhongEmitter::PhongEmitter( const IPainter& radEx_, const Scalar scale_, const IScalarPainter& N ) :
  radEx( radEx_ ),
  scale( scale_ ),
  phongN( N )
{
	radEx.addref();
	phongN.addref();

	// Sample the texture space of the radiance exitance to compute the average radiant exitance
	RayIntersectionGeometric rig( Ray(), nullRasterizerState );
	for( int i=0; i<100; i++ ) {
		rig.ptCoord = Point2( GlobalRNG().CanonicalRandom(), GlobalRNG().CanonicalRandom() );
		averageRadEx = averageRadEx + radEx.GetColor(rig);
		averageSpectrum = averageSpectrum + radEx.GetSpectrum(rig);
	}

	averageRadEx = averageRadEx * (scale/Scalar(100.0));
	averageSpectrum = averageSpectrum * (scale/Scalar(100.0));
}

PhongEmitter::~PhongEmitter( )
{
	radEx.release();
	phongN.release();
}

RISEPel PhongEmitter::emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const
{
	const Scalar	co = Vector3Ops::Dot(out,N);
	if( co < 0 ) {
		return RISEPel(0,0,0);
	}

	// According to the PDF for phong PDF(theta_i) = (n+1)/(2*PI) * cos^n(alpha)
	//   where alpha = angle between outgoing and direction of perfect specular (in this case the normal)
	const ScalarTriple pN_t = phongN.GetValuesAt( ri );
	const RISEPel	pN( pN_t.v[0], pN_t.v[1], pN_t.v[2] );
	const RISEPel	k = (pN + 1) * pow(co,pN) * (1.0 / TWO_PI);
	return (radEx.GetColor(ri) * k * scale);
}

Scalar PhongEmitter::emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm) const
{
	const Scalar	co = Vector3Ops::Dot(out,N);
	if( co < 0 ) {
		return 0;
	}

	// According to the PDF for phong PDF(theta_i) = (n+1)/(2*PI) * cos^n(alpha)
	//   where alpha = angle between outgoing and direction of perfect specular (in this case the normal)
	const Scalar	pN = phongN.GetValueAtNM( ri, nm );
	const Scalar	k = (pN + 1) * pow(co,pN) * (1.0 / TWO_PI);
	return (radEx.GetColorNM( ri, nm ) * k * scale);
}

RISEPel PhongEmitter::averageRadiantExitance() const
{
	return averageRadEx;
}

Scalar PhongEmitter::averageRadiantExitanceNM( const Scalar nm ) const
{
	return averageSpectrum.ValueAtNM( int(nm) );
}

Vector3 PhongEmitter::getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random ) const
{
	const ScalarTriple N_t = phongN.GetValuesAt(ri);
	if( N_t.IsUniform() ) {
		return GeometricUtilities::CreatePhongVector( ri.onb, random, N_t.v[0] );
	} else {
		return GeometricUtilities::CreatePhongVector( ri.onb, random, N_t.v[int(floor(random.x*3))] );
	}
}
