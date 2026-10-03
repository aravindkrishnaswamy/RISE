//////////////////////////////////////////////////////////////////////
//
//  DataDrivenSPF.cpp - Implementation of datadriven_material's SPF
//    (DL-325).  See DataDrivenSPF.h for the design.
//
//  Author: Aravind Krishnaswamy (RISE debt-cleanup, slice `debt-dl325`)
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "DataDrivenSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"

using namespace RISE;
using namespace RISE::Implementation;

DataDrivenSPF::DataDrivenSPF( const DataDrivenBSDF& bsdf ) :
  pBSDF( &bsdf )
{
	pBSDF->addref();
}

DataDrivenSPF::~DataDrivenSPF( )
{
	safe_release( pBSDF );
}

bool DataDrivenSPF::FrontFrame(
	const RayIntersectionGeometric& ri,
	OrthonormalBasis3D& onbOut,
	Vector3& geomNOut
	) const
{
	// DataDrivenBSDF::value returns 0 unless the view vector is on the
	// shading normal's positive side (nr >= 0).  Where it is 0 there is no
	// function to sample: emit nothing, and report no density.  (A
	// single-sided back face is therefore a termination -- which is exactly
	// what the BSDF already says, and what NEE already sees.)
	if( Vector3Ops::Dot( ri.ray.Dir(), ri.vNormal ) >= -NEARZERO ) {
		return false;
	}

	onbOut = ri.onb;

	// Geometric-horizon gate, identical to LambertianSPF's: a tilted shading
	// normal (bump / normal map / glint) can validate a direction that is
	// below the true surface, which the continuation would tunnel through.
	// Anchored to the ray so the orientation cannot flip with the tilt.
	const Vector3& geomNRaw = ( Vector3Ops::SquaredModulus( ri.vGeomNormal ) > Scalar(1e-12) )
		? ri.vGeomNormal : ri.onb.w();
	geomNOut = ( Vector3Ops::Dot( geomNRaw, ri.ray.Dir() ) < 0 ) ? geomNRaw : -geomNRaw;
	return true;
}

void DataDrivenSPF::Scatter(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	// Always consume the same two canonical numbers, whether or not a ray is
	// emitted (HasFixedDimensionBudget-style stability for QMC samplers).
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );

	OrthonormalBasis3D myonb;
	Vector3 geomN;
	if( !FrontFrame( ri, myonb, geomN ) ) {
		return;
	}

	ScatteredRay diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	diffuse.isDelta = false;

	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) <= 0 ) {
		return;
	}

	const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), myonb.w() );
	diffuse.pdf = cosTheta * INV_PI;
	if( diffuse.pdf <= 0 ) {
		return;
	}

	// kray = f cos / pdf, evaluated from the BSDF itself (= pi f).
	diffuse.kray = pBSDF->value( diffuse.ray.Dir(), ri ) * ( cosTheta / diffuse.pdf );
	scattered.AddScatteredRay( diffuse );
}

void DataDrivenSPF::ScatterNM(
	const RayIntersectionGeometric& ri,
	ISampler& sampler,
	const Scalar nm,
	ScatteredRayContainer& scattered,
	const IORStack& ior_stack
	) const
{
	const Point2 ptrand( sampler.Get1D(), sampler.Get1D() );

	OrthonormalBasis3D myonb;
	Vector3 geomN;
	if( !FrontFrame( ri, myonb, geomN ) ) {
		return;
	}

	ScatteredRay diffuse;
	diffuse.type = ScatteredRay::eRayDiffuse;
	diffuse.ray.Set( ri.ptIntersection, GeometricUtilities::CreateDiffuseVector( myonb, ptrand ) );
	diffuse.isDelta = false;

	if( Vector3Ops::Dot( diffuse.ray.Dir(), geomN ) <= 0 ) {
		return;
	}

	const Scalar cosTheta = Vector3Ops::Dot( diffuse.ray.Dir(), myonb.w() );
	diffuse.pdf = cosTheta * INV_PI;
	if( diffuse.pdf <= 0 ) {
		return;
	}

	diffuse.krayNM = pBSDF->valueNM( diffuse.ray.Dir(), ri, nm ) * ( cosTheta / diffuse.pdf );
	scattered.AddScatteredRay( diffuse );
}

Scalar DataDrivenSPF::PdfImpl( const RayIntersectionGeometric& ri, const Vector3& wo ) const
{
	OrthonormalBasis3D myonb;
	Vector3 geomN;
	if( !FrontFrame( ri, myonb, geomN ) ) {
		return 0;
	}
	const Scalar cosTheta = Vector3Ops::Dot( wo, myonb.w() );
	return ( cosTheta > 0 && Vector3Ops::Dot( wo, geomN ) > 0 ) ? cosTheta * INV_PI : 0;
}

Scalar DataDrivenSPF::Pdf(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const IORStack& ior_stack
	) const
{
	return PdfImpl( ri, wo );
}

Scalar DataDrivenSPF::PdfNM(
	const RayIntersectionGeometric& ri,
	const Vector3& wo,
	const Scalar nm,
	const IORStack& ior_stack
	) const
{
	// The table is RGB-only, so the density has no wavelength dependence.
	return PdfImpl( ri, wo );
}

Scalar DataDrivenSPF::EvaluateLobeFNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	if( rayType != ScatteredRay::eRayDiffuse ) {
		return -1;
	}
	return pBSDF->valueNM( Vector3Ops::Normalize( outDir ), ri, nm );
}

Scalar DataDrivenSPF::EvaluateKrayNM(
	const RayIntersectionGeometric& ri,
	const Vector3& outDir,
	ScatteredRay::ScatRayType rayType,
	Scalar nm,
	const IORStack& ior_stack
	) const
{
	if( rayType != ScatteredRay::eRayDiffuse ) {
		return -1;
	}
	const Vector3 wo = Vector3Ops::Normalize( outDir );
	const Scalar pdf = PdfImpl( ri, wo );
	if( pdf <= 0 ) {
		return 0;
	}
	const Scalar cosTheta = Vector3Ops::Dot( wo, ri.onb.w() );
	return pBSDF->valueNM( wo, ri, nm ) * ( cosTheta / pdf );
}
