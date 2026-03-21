//////////////////////////////////////////////////////////////////////
//
//  TranslucentSPF.cpp - Implementation of the translucent
//  SPF
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
#include "TranslucentSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Optics.h"
#include "../Utilities/RandomNumbers.h"

using namespace RISE;
using namespace RISE::Implementation;

TranslucentSPF::TranslucentSPF( 
	const IPainter& rF,
	const IPainter& T, 
	const IPainter& ext,
	const IPainter& N_, 
	const IPainter& scat 
	) : 
  pRefFront( rF ),
  pTrans( T ), 
  pExtinction( ext ), 
  N( N_ ), 
  pScat( scat )
{
	pRefFront.addref();
	pTrans.addref();
	pExtinction.addref();
	N.addref();
	pScat.addref();
}

TranslucentSPF::~TranslucentSPF( )
{
	pRefFront.release();
	pTrans.release();
	pExtinction.release();
	N.release();
	pScat.release();
}

void TranslucentSPF::Scatter( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			const RandomNumberGenerator& random,				///< [in] Random number generator
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack* const ior_stack								///< [in/out] Index of refraction stack
			) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();
	OrthonormalBasis3D	myonb = ri.onb;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	if( Vector3Ops::Dot(n,r) < 0.0 )
	{
		// Going in 
		// Front face
		front.kray = pRefFront.GetColor(ri);
		front.type = ScatteredRay::eRayDiffuse;

		if( front.kray[0] > 0 ) {
			rv = GeometricUtilities::Perturb( n,
				acos( sqrt( random.CanonicalRandom() ) ),
				TWO_PI * random.CanonicalRandom() );

			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
			front.isDelta = false;
			scattered.AddScatteredRay( front );
		}

		trans.kray = pTrans.GetColor(ri);
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.kray[0] > 0 ) {
			myonb.FlipW();

			const RISEPel Nfactor = N.GetColor(ri);
			if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(random.CanonicalRandom(), 1.0 / (Nfactor[0] + 1.0)) ),
					TWO_PI * random.CanonicalRandom() );

				trans.ray.Set( ri.ptIntersection, rv );
				// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
				const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
				trans.pdf = (Nfactor[0] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[0] );
				trans.isDelta = false;
				scattered.AddScatteredRay( trans );
			} else {
				// Add a new ray for each color component
				RISEPel p = trans.kray;
				trans.kray = 0;
				Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );
				for( int i=0; i<3; i++ ) {
					rv = GeometricUtilities::Perturb( myonb.w(),
						acos( pow(ptrand.x, 1.0 / (Nfactor[i] + 1.0)) ),
						TWO_PI * ptrand.y );

					trans.kray = 0;
					trans.kray[0] = p[0];
					trans.ray.Set( ri.ptIntersection, rv );
					// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
					const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
					trans.pdf = (Nfactor[i] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[i] );
					trans.isDelta = false;
					scattered.AddScatteredRay( trans );
				}
			}
		}
	}
	else
	{
		// Coming out the other side
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		const RISEPel ab = pExtinction.GetColor(ri);
		front.kray = ColorMath::exponential( -distance*ab );

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( ColorMath::MaxValue(front.kray) > 0 ) {
			// Check the scattering parameter
			const RISEPel scat = pScat.GetColor(ri);

			if( ColorMath::MaxValue(scat) > 0 ) {
				// Multiple scatter back
				myonb.FlipW();

				trans.type = ScatteredRay::eRayTranslucent;
				trans.kray = front.kray * scat;

				const RISEPel Nfactor = N.GetColor(ri);
				if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
					rv = GeometricUtilities::Perturb( myonb.w(),
						acos( pow(random.CanonicalRandom(), 1.0 / (Nfactor[0] + 1.0)) ),
						TWO_PI * random.CanonicalRandom() );

					trans.ray.Set( ri.ptIntersection, rv );
					// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
					{
						const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
						trans.pdf = (Nfactor[0] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[0] );
						trans.isDelta = false;
					}
					front.kray = front.kray * (RISEPel(1.0,1.0,1.0)-scat);
					scattered.AddScatteredRay( trans );
				} else {
					// Add a new ray for each color component
					RISEPel p = trans.kray;
					RISEPel f = front.kray;
					trans.kray = 0;
					Point2 ptrand( random.CanonicalRandom(), random.CanonicalRandom() );
					for( int i=0; i<3; i++ ) {
						rv = GeometricUtilities::Perturb( myonb.w(),
							acos( pow(ptrand.x, 1.0 / (Nfactor[i] + 1.0)) ),
							TWO_PI * ptrand.y );

						trans.kray = 0;
						trans.kray[i] = p[i];
						trans.ray.Set( ri.ptIntersection, rv );
						// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
						{
							const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
							trans.pdf = (Nfactor[i] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[i] );
							trans.isDelta = false;
						}
						front.kray = 0;
						front.kray[i] = f[i] * (1.0-scat[i]);
						scattered.AddScatteredRay( trans );
					}
				}
			}
		}

		rv = GeometricUtilities::Perturb( n,
				acos( sqrt(random.CanonicalRandom() ) ),
				TWO_PI * random.CanonicalRandom() );

		front.ray.Set( ri.ptIntersection, rv );
		front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
		front.isDelta = false;
		scattered.AddScatteredRay( front );
	}
}

void TranslucentSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();
	OrthonormalBasis3D	myonb = ri.onb;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	if( Vector3Ops::Dot(n,r) < 0.0 )
	{
		// Extinction check
		front.krayNM = pRefFront.GetColorNM(ri,nm);
		front.type = ScatteredRay::eRayDiffuse;
		
		if( front.krayNM > 0 ) {
			rv = GeometricUtilities::Perturb( n,
				acos( sqrt(random.CanonicalRandom()) ),
				TWO_PI * random.CanonicalRandom() );

			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
			front.isDelta = false;
			scattered.AddScatteredRay( front );
		}

		trans.krayNM = pTrans.GetColorNM(ri,nm);
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.krayNM > 0 ) {
			myonb.FlipW();

			const Scalar Nval = N.GetColorNM(ri,nm);
			rv = GeometricUtilities::Perturb( myonb.w(),
				acos( pow(random.CanonicalRandom(), 1.0 / (Nval + 1.0)) ),
				TWO_PI * random.CanonicalRandom() );

			trans.ray.Set( ri.ptIntersection, rv );
			// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
			const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
			trans.pdf = (Nval + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval );
			trans.isDelta = false;
			scattered.AddScatteredRay( trans );
		}
	}
	else
	{
		// Coming out the other side
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		front.krayNM = pTrans.GetColorNM(ri,nm) * exp(-(pExtinction.GetColorNM(ri,nm)*distance));
		
		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( front.krayNM > 0 ) {
			// Check the scattering parameter
			const Scalar scat = pScat.GetColorNM(ri,nm);

			if( scat > 0 ) {
				// Multiple scatter back
				myonb.FlipW();
				const Scalar Nval_scat = N.GetColorNM(ri,nm);
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(random.CanonicalRandom(), 1.0 / (Nval_scat + 1.0)) ),
					TWO_PI * random.CanonicalRandom() );

				trans.type = ScatteredRay::eRayTranslucent;
				trans.krayNM *= scat;
				trans.ray.Set( ri.ptIntersection, rv );
				// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
				{
					const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
					trans.pdf = (Nval_scat + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval_scat );
					trans.isDelta = false;
				}
				scattered.AddScatteredRay( trans );

				front.krayNM *= (1.0-scat);
			}
		}

		{
			const Scalar Nval_front = N.GetColorNM(ri,nm);
			rv = GeometricUtilities::Perturb( n,
				acos( pow(random.CanonicalRandom(), 1.0 / (Nval_front + 1.0)) ),
				TWO_PI * random.CanonicalRandom() );

			front.ray.Set( ri.ptIntersection, rv );
			// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
			const Scalar cosAlpha = fabs( Vector3Ops::Dot( front.ray.Dir(), n ) );
			front.pdf = (Nval_front + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval_front );
			front.isDelta = false;
		}

		scattered.AddScatteredRay( front );
	}
}

Scalar TranslucentSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack* const ior_stack
	) const
{
	// For the front hemisphere diffuse component, return cosine-weighted PDF
	// For the translucent (back hemisphere) component, return 0
	// (translucent paths have a complex mixed PDF that we approximate as 0)
	const bool bFrontFace = Vector3Ops::Dot(ri.ray.Dir(), ri.onb.w()) <= NEARZERO;
	const Scalar cosTheta = bFrontFace ?
		Vector3Ops::Dot( wo, ri.onb.w() ) :
		-Vector3Ops::Dot( wo, ri.onb.w() );
	return (cosTheta > 0) ? cosTheta * INV_PI : 0;
}

Scalar TranslucentSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack* const ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}

