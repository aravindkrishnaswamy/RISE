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

	const Vector3	r = ri.ray.dir;
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

	const Vector3	r = ri.ray.dir;
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
			scattered.AddScatteredRay( front );
		}

		trans.krayNM = pTrans.GetColorNM(ri,nm);
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.krayNM > 0 ) {
			myonb.FlipW();

			rv = GeometricUtilities::Perturb( myonb.w(),
				acos( pow(random.CanonicalRandom(), 1.0 / (N.GetColorNM(ri,nm) + 1.0)) ),
				TWO_PI * random.CanonicalRandom() );

			trans.ray.Set( ri.ptIntersection, rv );
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
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(random.CanonicalRandom(), 1.0 / (N.GetColorNM(ri,nm) + 1.0)) ),
					TWO_PI * random.CanonicalRandom() );

				trans.type = ScatteredRay::eRayTranslucent;
				trans.krayNM *= scat;
				trans.ray.Set( ri.ptIntersection, rv );
				scattered.AddScatteredRay( trans );

				front.krayNM *= (1.0-scat);
			}
		}

		rv = GeometricUtilities::Perturb( n, 
			acos( pow(random.CanonicalRandom(), 1.0 / (N.GetColorNM(ri,nm) + 1.0)) ),
			TWO_PI * random.CanonicalRandom() );

		front.ray.Set( ri.ptIntersection, rv );

		scattered.AddScatteredRay( front );
	}
}

