//////////////////////////////////////////////////////////////////////
//
//  WardIsotropicGaussianBRDF.cpp
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
#include "WardIsotropicGaussianBRDF.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

WardIsotropicGaussianBRDF::WardIsotropicGaussianBRDF(
	const IPainter& diffuse_,
	const IPainter& specular_,
	const IScalarPainter& alpha_
	) :
  pDiffuse( &diffuse_ ),
  pSpecular( &specular_ ),
  pAlpha( &alpha_ )
{
	pDiffuse->addref();
	pSpecular->addref();
	pAlpha->addref();
}

WardIsotropicGaussianBRDF::~WardIsotropicGaussianBRDF( )
{
	safe_release( pDiffuse );
	safe_release( pSpecular );
	safe_release( pAlpha );
}

void WardIsotropicGaussianBRDF::SetDiffuse( const IPainter& v )      { v.addref(); safe_release( pDiffuse );  pDiffuse  = &v; }
void WardIsotropicGaussianBRDF::SetSpecular( const IPainter& v )     { v.addref(); safe_release( pSpecular ); pSpecular = &v; }
void WardIsotropicGaussianBRDF::SetAlpha( const IScalarPainter& v )  { v.addref(); safe_release( pAlpha );    pAlpha    = &v; }

template< class T >
static void ComputeFactors( 
    T& diffuse, 
	T& specular,
	const Vector3& vLightIn, 
	const RayIntersectionGeometric& ri, 
	const Vector3& n,
	const T& alpha
	)
{
	Vector3 v = Vector3Ops::Normalize(vLightIn); // light vector
	Vector3 r = Vector3Ops::Normalize(-ri.ray.Dir()); // outgoing ray vector

	const Scalar nr = Vector3Ops::Dot(n,r);
	const Scalar nv = Vector3Ops::Dot(n,v);

	if( (nr >= NEARZERO) && (nv >= NEARZERO) ) {
		diffuse = INV_PI;

		const Vector3 h = Vector3Ops::Normalize(v+r);
		const Scalar hn = Vector3Ops::Dot(n,h);

		const Scalar first = 1.0 / (sqrt(nr*nv));
		const Scalar tanh = tan(acos(hn));
		const T sqralpha = alpha*alpha;
		const T second = exp( -(tanh*tanh)/sqralpha ) / (FOUR_PI*sqralpha);

		specular = first*second;
	}
}

RISEPel WardIsotropicGaussianBRDF::value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const
{
	RISEPel d, s;
	const ScalarTriple at = pAlpha->GetValuesAt(ri);
	const RISEPel a( at.v[0], at.v[1], at.v[2] );
	ComputeFactors<RISEPel>( d, s, vLightIn, ri, ri.onb.w(), a );

	return d*pDiffuse->GetColor(ri) + s*pSpecular->GetColor(ri);
}

Scalar WardIsotropicGaussianBRDF::valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	Scalar d=0, s=0;
	ComputeFactors<Scalar>( d, s, vLightIn, ri, ri.onb.w(), pAlpha->GetValueAtNM(ri,nm) );

	return d*pDiffuse->GetColorNM(ri,nm) + s*pSpecular->GetColorNM(ri,nm);
}

RISEPel WardIsotropicGaussianBRDF::albedo( const RayIntersectionGeometric& ri ) const
{
	// Ward's Gaussian normalization makes the spec lobe integrate to
	// ≈ Rs over the hemisphere, so total reflectance ≈ Rd + Rs.
	return pDiffuse->GetColor( ri ) + pSpecular->GetColor( ri );
}
