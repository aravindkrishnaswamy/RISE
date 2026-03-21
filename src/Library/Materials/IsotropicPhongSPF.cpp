//////////////////////////////////////////////////////////////////////
//
//  IsotropicPhongSPF.cpp - Implementation of the phong SPF
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
#include "IsotropicPhongSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"

using namespace RISE;
using namespace RISE::Implementation;

IsotropicPhongSPF::IsotropicPhongSPF( const IPainter& Rd_, const IPainter& Rs_, const IPainter& exp ) :
  Rd( Rd_ ), Rs( Rs_ ), exponent( exp )
{
	Rd.addref();
	Rs.addref();
	exponent.addref();
}

IsotropicPhongSPF::~IsotropicPhongSPF( )
{
	Rd.release();
	Rs.release();
	exponent.release();
}

static void GenerateDiffuseRay(
	ScatteredRay& diffuse,
	const Scalar rdotn,											///< [in] Angle between view ray and normal
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& ptrand										///< [in] Random numbers
	)
{
	diffuse.type = ScatteredRay::eRayDiffuse;

	// Generate a reflected ray randomly with a cosine distribution
	if( rdotn > NEARZERO )
	{
		OrthonormalBasis3D	myonb = ri.onb;
		myonb.FlipW();
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	} else {
		diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( ri.onb, ptrand ) );
	}
}

static void GenerateSpecularRay(
	ScatteredRay& specular,
	const Vector3& normal,										///< [in] Adjusted normal at surface
	const Vector3& reflected,									///< [in] Reflected ray at surface
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const Point2& ptrand,										///< [in] Random numbers
	const Scalar exponent
	)
{
	specular.type = ScatteredRay::eRayReflection;

	Vector3	rv = reflected;

	// Use the warping function to perturb the reflected ray using phong
	rv = GeometricUtilities::Perturb(rv,
        acos( pow(ptrand.x, 1.0 / (exponent+1.0)) ),
                 TWO_PI * ptrand.y);

	specular.ray.Set( ri.ptIntersection, rv );
}

void IsotropicPhongSPF::Scatter(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );

	const RISEPel N = exponent.GetColor(ri);

	ScatteredRay diffuse, specular;
	GenerateDiffuseRay( diffuse, rdotn, ri,  Point2( random.CanonicalRandom(), random.CanonicalRandom() ) );

	// Set PDF for diffuse ray: cosine-weighted hemisphere sampling
	{
		const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), n );
		diffuse.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		diffuse.isDelta = false;
	}

	if( N[0] == N[1] && N[1] == N[2] ) {
		GenerateSpecularRay( specular, n, reflected, ri,  Point2( random.CanonicalRandom(), random.CanonicalRandom() ), N[0] );
		specular.kray = Rs.GetColor(ri);

		// PDF for phong lobe: (N+1)/(2*pi) * cos^N(alpha), alpha = angle from reflection direction
		const Scalar cosAlpha = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), Vector3Ops::Normalize(reflected) );
		if( cosAlpha > 0 ) {
			specular.pdf = (N[0] + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N[0] );
		} else {
			specular.pdf = 0;
		}
		specular.isDelta = false;

		scattered.AddScatteredRay( specular );
	} else {
		const RISEPel spec = Rs.GetColor(ri);
		const Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );
		for( int i=0; i<3; i++ ) {
			GenerateSpecularRay( specular, n, reflected, ri,  ptrand, N[i] );
			specular.kray = 0.0;
			specular.kray[i] = spec[i];

			// PDF for phong lobe per channel
			const Scalar cosAlpha = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), Vector3Ops::Normalize(reflected) );
			if( cosAlpha > 0 ) {
				specular.pdf = (N[i] + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N[i] );
			} else {
				specular.pdf = 0;
			}
			specular.isDelta = false;

			scattered.AddScatteredRay( specular );
		}
	}

	diffuse.kray = Rd.GetColor(ri);
	scattered.AddScatteredRay( diffuse );
}


void IsotropicPhongSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );

	ScatteredRay diffuse, specular;
	const Scalar N = exponent.GetColorNM(ri,nm);
	GenerateDiffuseRay( diffuse, rdotn, ri,  Point2( random.CanonicalRandom(), random.CanonicalRandom() ) );
	GenerateSpecularRay( specular, n, reflected, ri,  Point2( random.CanonicalRandom(), random.CanonicalRandom() ),  N );

	diffuse.krayNM = Rd.GetColorNM(ri,nm);
	specular.krayNM = Rs.GetColorNM(ri,nm);

	// Set PDF for diffuse ray
	{
		const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), n );
		diffuse.pdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;
		diffuse.isDelta = false;
	}

	// Set PDF for specular ray
	{
		const Scalar cosAlpha = Vector3Ops::Dot( Vector3Ops::Normalize(specular.ray.Dir()), Vector3Ops::Normalize(reflected) );
		if( cosAlpha > 0 ) {
			specular.pdf = (N + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N );
		} else {
			specular.pdf = 0;
		}
		specular.isDelta = false;
	}

	scattered.AddScatteredRay( diffuse );
	scattered.AddScatteredRay( specular );
}

Scalar IsotropicPhongSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );
	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	// Diffuse component: cosine-weighted hemisphere
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	const Scalar diffusePdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;

	// Specular component: phong lobe around reflection direction
	// Use average exponent across channels
	const RISEPel N = exponent.GetColor(ri);
	const Scalar Navg = (N[0] + N[1] + N[2]) / 3.0;
	const Scalar cosAlpha = Vector3Ops::Dot( woNorm, Vector3Ops::Normalize(reflected) );
	const Scalar specPdf = (cosAlpha > 0) ? (Navg + 1.0) * INV_PI * 0.5 * pow( cosAlpha, Navg ) : 0;

	// Weight by relative importance of diffuse vs specular reflectance
	const RISEPel rd = Rd.GetColor(ri);
	const RISEPel rs = Rs.GetColor(ri);
	const Scalar dWeight = ColorMath::MaxValue(rd);
	const Scalar sWeight = ColorMath::MaxValue(rs);
	const Scalar totalWeight = dWeight + sWeight;

	if( totalWeight < NEARZERO ) {
		return 0;
	}

	return (dWeight * diffusePdf + sWeight * specPdf) / totalWeight;
}

Scalar IsotropicPhongSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	const Scalar rdotn = Vector3Ops::Dot(ri.ray.Dir(), ri.vNormal);
	const Vector3 n = rdotn > 0 ? -ri.onb.w() : ri.onb.w();
	const Vector3 reflected = Optics::CalculateReflectedRay( ri.ray.Dir(), n );
	const Vector3 woNorm = Vector3Ops::Normalize( wo );

	// Diffuse component
	const Scalar cosTheta = Vector3Ops::Dot( woNorm, n );
	const Scalar diffusePdf = (cosTheta > 0) ? cosTheta * INV_PI : 0;

	// Specular component
	const Scalar N = exponent.GetColorNM(ri,nm);
	const Scalar cosAlpha = Vector3Ops::Dot( woNorm, Vector3Ops::Normalize(reflected) );
	const Scalar specPdf = (cosAlpha > 0) ? (N + 1.0) * INV_PI * 0.5 * pow( cosAlpha, N ) : 0;

	// Weight by relative importance
	const Scalar rd = Rd.GetColorNM(ri,nm);
	const Scalar rs = Rs.GetColorNM(ri,nm);
	const Scalar totalWeight = rd + rs;

	if( totalWeight < NEARZERO ) {
		return 0;
	}

	return (rd * diffusePdf + rs * specPdf) / totalWeight;
}
