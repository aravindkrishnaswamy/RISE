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
	const IScalarPainter& ext,
	const IScalarPainter& N_,
	const IScalarPainter& scat
	) :
  pRefFront( &rF ),
  pTrans( &T ),
  pExtinction( &ext ),
  pN( &N_ ),
  pScat( &scat )
{
	pRefFront->addref();
	pTrans->addref();
	pExtinction->addref();
	pN->addref();
	pScat->addref();
}

TranslucentSPF::~TranslucentSPF( )
{
	safe_release( pRefFront );
	safe_release( pTrans );
	safe_release( pExtinction );
	safe_release( pN );
	safe_release( pScat );
}

void TranslucentSPF::SetRefFront( const IPainter& v )         { v.addref(); safe_release( pRefFront );   pRefFront   = &v; }
void TranslucentSPF::SetTrans( const IPainter& v )            { v.addref(); safe_release( pTrans );      pTrans      = &v; }
void TranslucentSPF::SetExtinction( const IScalarPainter& v ) { v.addref(); safe_release( pExtinction ); pExtinction = &v; }
void TranslucentSPF::SetN( const IScalarPainter& v )          { v.addref(); safe_release( pN );          pN          = &v; }
void TranslucentSPF::SetScat( const IScalarPainter& v )       { v.addref(); safe_release( pScat );       pScat       = &v; }

void TranslucentSPF::Scatter( 
			const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
			ISampler& sampler,				///< [in] Sampler
			ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
			const IORStack& ior_stack								///< [in/out] Index of refraction stack
			) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();
	OrthonormalBasis3D	myonb = ri.onb;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available, matching DielectricSPF.  The normal-
	// based dot-product test is unreliable for nested translucent objects
	// because a back-scattered ray hitting an enclosing surface from
	// inside the cavity is misclassified as "exiting."
	const bool bEntering = !ior_stack.containsCurrent();

	if( bEntering )
	{
		// Going in
		// Front face
		front.kray = pRefFront->GetColor(ri);
		front.type = ScatteredRay::eRayDiffuse;

		if( front.kray[0] > 0 ) {
			rv = GeometricUtilities::Perturb( n,
				acos( sqrt( sampler.Get1D() ) ),
				TWO_PI * sampler.Get1D() );

			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
			front.isDelta = false;
			scattered.AddScatteredRay( front );
		}

		trans.kray = pTrans->GetColor(ri);
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.kray[0] > 0 ) {
			myonb.FlipW();

			const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
			if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(sampler.Get1D(), 1.0 / (Nfactor[0] + 1.0)) ),
					TWO_PI * sampler.Get1D() );

				trans.ray.Set( ri.ptIntersection, rv );
				// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
				const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
				trans.pdf = (Nfactor[0] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[0] );
				trans.isDelta = false;
				trans.ior_stack = new IORStack( ior_stack );
				trans.ior_stack->push( 1.0 );
				GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
				scattered.AddScatteredRay( trans );
			} else {
				// Add a new ray for each color component
				RISEPel p = trans.kray;
				trans.kray = 0;
				Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
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
					trans.ior_stack = new IORStack( ior_stack );
					trans.ior_stack->push( 1.0 );
					GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
					scattered.AddScatteredRay( trans );
				}
			}
		}
	}
	else
	{
		// Coming out the other side
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		const ScalarTriple abt = pExtinction->GetValuesAt(ri);
		const RISEPel ab( abt.v[0], abt.v[1], abt.v[2] );
		front.kray = ColorMath::exponential( -distance*ab );

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( ColorMath::MaxValue(front.kray) > 0 ) {
			// Check the scattering parameter
			const ScalarTriple scat_t = pScat->GetValuesAt(ri);
			const RISEPel scat( scat_t.v[0], scat_t.v[1], scat_t.v[2] );

			if( ColorMath::MaxValue(scat) > 0 ) {
				// Multiple scatter back
				myonb.FlipW();

				trans.type = ScatteredRay::eRayTranslucent;
				trans.kray = front.kray * scat;

				const ScalarTriple Nfactor_t = pN->GetValuesAt(ri); const RISEPel Nfactor( Nfactor_t.v[0], Nfactor_t.v[1], Nfactor_t.v[2] );
				if( (Nfactor[0] == Nfactor[1]) && (Nfactor[1] == Nfactor[2]) ) {
					rv = GeometricUtilities::Perturb( myonb.w(),
						acos( pow(sampler.Get1D(), 1.0 / (Nfactor[0] + 1.0)) ),
						TWO_PI * sampler.Get1D() );

					trans.ray.Set( ri.ptIntersection, rv );
					// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
					{
						const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
						trans.pdf = (Nfactor[0] + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nfactor[0] );
						trans.isDelta = false;
					}
					front.kray = front.kray * (RISEPel(1.0,1.0,1.0)-scat);
					// Back-scattered ray stays inside this object, no stack change
					scattered.AddScatteredRay( trans );
				} else {
					// Add a new ray for each color component
					RISEPel p = trans.kray;
					RISEPel f = front.kray;
					trans.kray = 0;
					Point2 ptrand( sampler.Get1D(), sampler.Get1D() );
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
						// Back-scattered ray stays inside this object, no stack change
						scattered.AddScatteredRay( trans );
					}
				}
			}
		}

		// Exit diffuse ray leaves the object — pop from IOR stack
		rv = GeometricUtilities::Perturb( n,
				acos( sqrt(sampler.Get1D() ) ),
				TWO_PI * sampler.Get1D() );

		front.ray.Set( ri.ptIntersection, rv );
		front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
		front.isDelta = false;
		front.ior_stack = new IORStack( ior_stack );
		front.ior_stack->pop();
		GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
		scattered.AddScatteredRay( front );
	}
}

void TranslucentSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay	front;
	ScatteredRay	trans;

	const Vector3& n = ri.onb.w();
	OrthonormalBasis3D	myonb = ri.onb;

	const Vector3	r = ri.ray.Dir();
	Vector3		rv;

	const bool bEnteringNM = !ior_stack.containsCurrent();

	if( bEnteringNM )
	{
		// Extinction check
		front.krayNM = pRefFront->GetColorNM(ri,nm);
		front.type = ScatteredRay::eRayDiffuse;

		if( front.krayNM > 0 ) {
			rv = GeometricUtilities::Perturb( n,
				acos( sqrt(sampler.Get1D()) ),
				TWO_PI * sampler.Get1D() );

			front.ray.Set( ri.ptIntersection, rv );
			front.pdf = fabs( Vector3Ops::Dot( front.ray.Dir(), ri.onb.w() ) ) * INV_PI;
			front.isDelta = false;
			scattered.AddScatteredRay( front );
		}

		trans.krayNM = pTrans->GetColorNM(ri,nm);
		trans.type = ScatteredRay::eRayTranslucent;

		if( trans.krayNM > 0 ) {
			myonb.FlipW();

			const Scalar Nval = pN->GetValueAtNM(ri,nm);
			rv = GeometricUtilities::Perturb( myonb.w(),
				acos( pow(sampler.Get1D(), 1.0 / (Nval + 1.0)) ),
				TWO_PI * sampler.Get1D() );

			trans.ray.Set( ri.ptIntersection, rv );
			// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
			const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
			trans.pdf = (Nval + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval );
			trans.isDelta = false;
			trans.ior_stack = new IORStack( ior_stack );
			trans.ior_stack->push( 1.0 );
			GlobalLog()->PrintNew( trans.ior_stack, __FILE__, __LINE__, "ior stack" );
			scattered.AddScatteredRay( trans );
		}
	}
	else
	{
		// Coming out the other side
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );
		front.krayNM = pTrans->GetColorNM(ri,nm) * exp(-(pExtinction->GetValueAtNM(ri,nm)*distance));

		front.type = ScatteredRay::eRayDiffuse;

		// Don't bother checking scattering if the ray is totally extinguished
		if( front.krayNM > 0 ) {
			// Check the scattering parameter
			const Scalar scat = pScat->GetValueAtNM(ri,nm);

			if( scat > 0 ) {
				// Multiple scatter back
				myonb.FlipW();
				const Scalar Nval_scat = pN->GetValueAtNM(ri,nm);
				rv = GeometricUtilities::Perturb( myonb.w(),
					acos( pow(sampler.Get1D(), 1.0 / (Nval_scat + 1.0)) ),
					TWO_PI * sampler.Get1D() );

				trans.type = ScatteredRay::eRayTranslucent;
				trans.krayNM *= scat;
				trans.ray.Set( ri.ptIntersection, rv );
				// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
				{
					const Scalar cosAlpha = fabs( Vector3Ops::Dot( trans.ray.Dir(), myonb.w() ) );
					trans.pdf = (Nval_scat + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval_scat );
					trans.isDelta = false;
				}
				// Back-scattered ray stays inside this object, no stack change
				scattered.AddScatteredRay( trans );

				front.krayNM *= (1.0-scat);
			}
		}

		// Exit ray leaves the object — pop from IOR stack
		{
			const Scalar Nval_front = pN->GetValueAtNM(ri,nm);
			rv = GeometricUtilities::Perturb( n,
				acos( pow(sampler.Get1D(), 1.0 / (Nval_front + 1.0)) ),
				TWO_PI * sampler.Get1D() );

			front.ray.Set( ri.ptIntersection, rv );
			// Phong-lobe PDF: (N+1)/(2*pi) * cos^N(alpha)
			const Scalar cosAlpha = fabs( Vector3Ops::Dot( front.ray.Dir(), n ) );
			front.pdf = (Nval_front + 1.0) * 0.5 * INV_PI * pow( cosAlpha, Nval_front );
			front.isDelta = false;
		}

		front.ior_stack = new IORStack( ior_stack );
		front.ior_stack->pop();
		GlobalLog()->PrintNew( front.ior_stack, __FILE__, __LINE__, "ior stack" );
		scattered.AddScatteredRay( front );
	}
}

Scalar TranslucentSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
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
	const IORStack& ior_stack
	) const
{
	return Pdf( ri, wo, ior_stack );
}

