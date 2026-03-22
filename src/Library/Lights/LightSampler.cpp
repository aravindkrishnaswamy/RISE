//////////////////////////////////////////////////////////////////////
//
//  LightSampler.cpp - Implementation of the LightSampler utility
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "LightSampler.h"
#include "../Interfaces/ILightPriv.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IEmitter.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Color/ColorMath.h"
#include "../Utilities/Math3D/Constants.h"
#include "../Intersection/RayIntersectionGeometric.h"

using namespace RISE;
using namespace RISE::Implementation;

LightSampler::LightSampler()
{
}

LightSampler::~LightSampler()
{
}

Scalar LightSampler::ComputeTotalExitance(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries
	) const
{
	Scalar total = 0;

	// Accumulate exitance from non-mesh lights
	const ILightManager* pLightMgr = scene.GetLights();
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			if( l->CanGeneratePhotons() )
			{
				total += ColorMath::MaxValue( l->radiantExitance() );
			}
		}
	}

	// Accumulate exitance from mesh luminaries
	LuminaryManager::LuminariesList::const_iterator i, e;
	for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ )
	{
		const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
		if( pEmitter )
		{
			const Scalar area = i->pLum->GetArea();
			const RISEPel power = pEmitter->averageRadiantExitance() * area;
			total += ColorMath::MaxValue( power );
		}
	}

	return total;
}

bool LightSampler::SampleLight(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const RandomNumberGenerator& random,
	LightSample& sample
	) const
{
	// Initialize the sample
	sample.pLight = 0;
	sample.pLuminary = 0;
	sample.isDelta = false;
	sample.Le = RISEPel( 0, 0, 0 );
	sample.pdfPosition = 0;
	sample.pdfDirection = 0;
	sample.pdfSelect = 0;

	// Compute total exitance across all light sources
	const Scalar total_exitance = ComputeTotalExitance( scene, luminaries );

	if( total_exitance <= 0 )
	{
		return false;
	}

	// Select a light proportional to its exitance
	const Scalar xi = random.CanonicalRandom() * total_exitance;
	Scalar cumulative = 0;

	// Try non-mesh lights first
	const ILightManager* pLightMgr = scene.GetLights();
	if( pLightMgr )
	{
		const ILightManager::LightsList& lights = pLightMgr->getLights();
		ILightManager::LightsList::const_iterator m, n;
		for( m=lights.begin(), n=lights.end(); m!=n; m++ )
		{
			const ILightPriv* l = *m;
			if( l->CanGeneratePhotons() )
			{
				const Scalar exitance = ColorMath::MaxValue( l->radiantExitance() );
				cumulative += exitance;

				if( cumulative >= xi )
				{
					// Selected this non-mesh light
					sample.pLight = l;
					sample.pdfSelect = exitance / total_exitance;
					sample.isDelta = true;

					// Generate a random photon using the light's own method
					const Point3 ptrand(
						random.CanonicalRandom(),
						random.CanonicalRandom(),
						random.CanonicalRandom()
						);
					const Ray photonRay = l->generateRandomPhoton( ptrand );

					sample.position = photonRay.origin;
					sample.direction = photonRay.Dir();
					sample.normal = photonRay.Dir();

					// Emitted radiance in the sampled direction
					sample.Le = l->emittedRadiance( photonRay.Dir() );

					// For delta-position lights, pdfPosition = 1
					sample.pdfPosition = 1.0;

					// Query the light's own directional PDF
					sample.pdfDirection = l->pdfDirection( photonRay.Dir() );

					return true;
				}
			}
		}
	}

	// Try mesh luminaries
	LuminaryManager::LuminariesList::const_iterator i, e;
	for( i=luminaries.begin(), e=luminaries.end(); i!=e; i++ )
	{
		const IEmitter* pEmitter = i->pLum->GetMaterial()->GetEmitter();
		if( !pEmitter )
		{
			continue;
		}

		const Scalar area = i->pLum->GetArea();
		const RISEPel power = pEmitter->averageRadiantExitance() * area;
		const Scalar exitance = ColorMath::MaxValue( power );
		cumulative += exitance;

		if( cumulative >= xi )
		{
			// Selected this mesh luminary
			sample.pLuminary = i->pLum;
			sample.pdfSelect = exitance / total_exitance;
			sample.isDelta = false;

			// Sample a uniform random point on the luminary surface
			const Point3 prand(
				random.CanonicalRandom(),
				random.CanonicalRandom(),
				random.CanonicalRandom()
				);
			Point2 coord;
			i->pLum->UniformRandomPoint(
				&sample.position,
				&sample.normal,
				&coord,
				prand
				);

			// pdfPosition = 1 / area (uniform sampling on surface)
			sample.pdfPosition = (area > 0) ? (Scalar(1.0) / area) : 0;

			// Build an orthonormal basis around the surface normal
			// and sample a cosine-weighted hemisphere direction
			OrthonormalBasis3D onb;
			onb.CreateFromW( sample.normal );

			const Point2 dirRand(
				random.CanonicalRandom(),
				random.CanonicalRandom()
				);
			sample.direction = GeometricUtilities::CreateDiffuseVector( onb, dirRand );

			// pdfDirection = cos(theta) / pi for cosine-weighted hemisphere
			const Scalar cosTheta = Vector3Ops::Dot( sample.direction, sample.normal );
			sample.pdfDirection = (cosTheta > 0) ? (cosTheta * INV_PI) : 0;

			// Compute emitted radiance at this point in this direction
			RayIntersectionGeometric rig( Ray( sample.position, sample.direction ), nullRasterizerState );
			rig.vNormal = sample.normal;
			rig.ptCoord = coord;
			rig.onb = onb;

			sample.Le = pEmitter->emittedRadiance( rig, sample.direction, sample.normal );

			return true;
		}
	}

	// Should not reach here if total_exitance > 0, but handle edge case
	return false;
}

Scalar LightSampler::PdfSelectLight(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const ILight& light
	) const
{
	const Scalar exitance = ColorMath::MaxValue( light.radiantExitance() );
	if( exitance <= 0 )
	{
		return 0;
	}

	const Scalar total_exitance = ComputeTotalExitance( scene, luminaries );
	if( total_exitance <= 0 )
	{
		return 0;
	}

	return exitance / total_exitance;
}

Scalar LightSampler::PdfSelectLuminary(
	const IScene& scene,
	const LuminaryManager::LuminariesList& luminaries,
	const IObject& luminary
	) const
{
	const IMaterial* pMat = luminary.GetMaterial();
	if( !pMat )
	{
		return 0;
	}

	const IEmitter* pEmitter = pMat->GetEmitter();
	if( !pEmitter )
	{
		return 0;
	}

	const Scalar area = luminary.GetArea();
	const Scalar exitance = ColorMath::MaxValue( pEmitter->averageRadiantExitance() * area );
	if( exitance <= 0 )
	{
		return 0;
	}

	const Scalar total_exitance = ComputeTotalExitance( scene, luminaries );
	if( total_exitance <= 0 )
	{
		return 0;
	}

	return exitance / total_exitance;
}
