//////////////////////////////////////////////////////////////////////
//
//  WardAnisotropicEllipticalGaussianBRDF.cpp
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "WardAnisotropicEllipticalGaussianBRDF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

WardAnisotropicEllipticalGaussianBRDF::WardAnisotropicEllipticalGaussianBRDF(
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IScalarPainter& alphax_,
	const IScalarPainter& alphay_
	) :
  pDiffuse( &diffuse_ ),
  pSpecular( &specular_ ),
  pAlphaX( &alphax_ ),
  pAlphaY( &alphay_ )
{
	pDiffuse->addref();
	pSpecular->addref();
	pAlphaX->addref();
	pAlphaY->addref();
}

WardAnisotropicEllipticalGaussianBRDF::~WardAnisotropicEllipticalGaussianBRDF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pAlphaX );
	safe_release( pAlphaY );
}

void WardAnisotropicEllipticalGaussianBRDF::SetDiffuse( const IPainter& v )      { v.addref(); safe_release( pDiffuse );  pDiffuse  = &v; }
void WardAnisotropicEllipticalGaussianBRDF::SetSpecular( const IPainter& v )     { v.addref(); safe_release( pSpecular ); pSpecular = &v; }
void WardAnisotropicEllipticalGaussianBRDF::SetAlphaX( const IScalarPainter& v ) { v.addref(); safe_release( pAlphaX );   pAlphaX   = &v; }
void WardAnisotropicEllipticalGaussianBRDF::SetAlphaY( const IScalarPainter& v ) { v.addref(); safe_release( pAlphaY );   pAlphaY   = &v; }

template< class T >
static void ComputeFactors( 
    T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const T& alphax,
	const T& alphay
	)
{
	Vector3 l = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Vector3& n = ri.onb.w();
	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nl = Vector3Ops::Dot(n,l);	

	if( (nr >= 0) && (nl >= 0) ) {
		diffuse = INV_PI;

		const Vector3 h = Vector3Ops::Normalize(l+r);
		const Scalar nh = Vector3Ops::Dot(n,h);

		const Scalar phi = acos(Vector3Ops::Dot(ri.onb.u(),Vector3Ops::Normalize(h-(nh*n))));

		const Scalar first = 1.0 / (sqrt(nr*nl));
		const Scalar tanh = tan(acos(nh));

		const T inside = (cos(phi)*cos(phi))/(alphax*alphax) + (sin(phi)*sin(phi))/(alphay*alphay);
		const T second = exp( -(tanh*tanh)*inside );
		const T third = 1.0 / (FOUR_PI*alphax*alphay);

		specular = first*second*third;
	}
}

RISEPel WardAnisotropicEllipticalGaussianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel d, s;
	const ScalarTriple axt = pAlphaX->GetValuesAt(ri);
	const ScalarTriple ayt = pAlphaY->GetValuesAt(ri);
	const RISEPel ax( axt.v[0], axt.v[1], axt.v[2] );
	const RISEPel ay( ayt.v[0], ayt.v[1], ayt.v[2] );
	ComputeFactors<RISEPel>( d, s, vLightIn, ri, ax, ay );

	return d*pDiffuse->GetColor(ri) + s*pSpecular->GetColor(ri);
}

Scalar WardAnisotropicEllipticalGaussianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar d=0, s=0;
	ComputeFactors<Scalar>( d, s, vLightIn, ri, pAlphaX->GetValueAtNM(ri,nm), pAlphaY->GetValueAtNM(ri,nm) );

	return d*pDiffuse->GetColorNM(ri,nm) + s*pSpecular->GetColorNM(ri,nm);
}

RISEPel WardAnisotropicEllipticalGaussianBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Same energy argument as the isotropic variant: Ward's anisotropic
	// elliptical Gaussian normalization makes the spec lobe integrate
	// to ≈ Rs, so total reflectance ≈ Rd + Rs.
	return pDiffuse->GetColor( ri ) + pSpecular->GetColor( ri );
}
