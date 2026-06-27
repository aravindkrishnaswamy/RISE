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
  pRadEx( &radEx_ ),
  scale( scale_ ),
  pPhongN( &N )
{
	pRadEx->addref();
	pPhongN->addref();
	RefreshAverages();
}

PhongEmitter::~PhongEmitter( )
{
	safe_release( pRadEx );
	safe_release( pPhongN );
}

void PhongEmitter::RefreshAverages()
{
	averageRadEx = RISEPel( 0, 0, 0 );
	averageSpectrum = VisibleSpectralPacket();

	// Sample the texture space of the radiance exitance to compute the average radiant exitance
	RayIntersectionGeometric rig( Ray(), nullRasterizerState );
	// Deterministic 10x10 stratified UV grid (cell centres), NOT 100 GlobalRNG samples: reproducible and
	// consumes no render-RNG at parse (this runs at emitter construction). It feeds light-importance weights
	// + photon flux (never the emitted radiance itself), so the converged render is unchanged (uniform
	// emissive: bit-identical; varying: importance-invariant / more-accurate flux). A regular grid can alias
	// a painter whose period resonates with the 0.1-UV pitch -- acceptable for an importance/flux weight.
	for( int gy=0; gy<10; gy++ ) for( int gx=0; gx<10; gx++ ) {
		rig.ptCoord = Point2( (Scalar(gx)+Scalar(0.5))/Scalar(10), (Scalar(gy)+Scalar(0.5))/Scalar(10) );
		averageRadEx = averageRadEx + pRadEx->GetColor(rig);
		averageSpectrum = averageSpectrum + pRadEx->GetSpectrum(rig);
	}

	averageRadEx = averageRadEx * (scale/Scalar(100.0));
	averageSpectrum = averageSpectrum * (scale/Scalar(100.0));
}

void PhongEmitter::SetRadEx( const IPainter& v )
{
	v.addref();
	safe_release( pRadEx );
	pRadEx = &v;
	RefreshAverages();
}

void PhongEmitter::SetN( const IScalarPainter& v )
{
	v.addref();
	safe_release( pPhongN );
	pPhongN = &v;
	// No average refresh needed — phongN affects the per-direction
	// emission shape but not the per-area average colour.
}

RISEPel PhongEmitter::emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const
{
	const Scalar	co = Vector3Ops::Dot(out,N);
	if( co < 0 ) {
		return RISEPel(0,0,0);
	}

	// According to the PDF for phong PDF(theta_i) = (n+1)/(2*PI) * cos^n(alpha)
	//   where alpha = angle between outgoing and direction of perfect specular (in this case the normal)
	const ScalarTriple pN_t = pPhongN->GetValuesAt( ri );
	const RISEPel	pN( pN_t.v[0], pN_t.v[1], pN_t.v[2] );
	const RISEPel	k = (pN + 1) * pow(co,pN) * (1.0 / TWO_PI);
	return (pRadEx->GetColor(ri) * k * scale);
}

Scalar PhongEmitter::emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm) const
{
	const Scalar	co = Vector3Ops::Dot(out,N);
	if( co < 0 ) {
		return 0;
	}

	// According to the PDF for phong PDF(theta_i) = (n+1)/(2*PI) * cos^n(alpha)
	//   where alpha = angle between outgoing and direction of perfect specular (in this case the normal)
	const Scalar	pN = pPhongN->GetValueAtNM( ri, nm );
	const Scalar	k = (pN + 1) * pow(co,pN) * (1.0 / TWO_PI);
	return (pRadEx->GetColorNM( ri, nm ) * k * scale);
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
	const ScalarTriple N_t = pPhongN->GetValuesAt(ri);
	if( N_t.IsUniform() ) {
		return GeometricUtilities::CreatePhongVector( ri.onb, random, N_t.v[0] );
	} else {
		return GeometricUtilities::CreatePhongVector( ri.onb, random, N_t.v[int(floor(random.x*3))] );
	}
}
