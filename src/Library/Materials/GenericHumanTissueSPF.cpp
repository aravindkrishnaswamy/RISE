//////////////////////////////////////////////////////////////////////
//
//  GenericHumanTissueSPF.cpp - Implementation of dielectric SPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "GenericHumanTissueSPF.h"
#include "BioSpecSkinData.h"
#include "BioSpecSkinSPF.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

GenericHumanTissueSPF::GenericHumanTissueSPF( 
	const IPainter& sca_,									///< Scattering co-efficient (how much scattering happens)
	const IPainter& g_,										///< Anisotropy factor for the HG phase function
	const Scalar whole_blood_,								///< Amount of tissue composed of whole blood
	const Scalar betacarotene_concentration_,				///< Concentration of beta-carotene in the dermis
	const Scalar bilirubin_concentration_,					///< Concentration of bilirubin in whole blood
	const Scalar hb_ratio_,									///< Oxy/deoxy hemoglobin ratio
	const bool diffuse_										///< Is the scattering just diffuse ?
	) : 
  sca( sca_ ),
  g( g_ ),
  whole_blood( whole_blood_ ),
  betacarotene_concentration( betacarotene_concentration_ ),
  bilirubin_concentration( bilirubin_concentration_ ),
  hb_ratio( hb_ratio_ ),
  diffuse( diffuse_ )
{
	sca.addref();
	g.addref();

	// Hemoglobin absorption function
	{
		const int count = sizeof( SkinData::omlc_prahl_hemoglobin_wavelengths ) / sizeof( Scalar );

		IPiecewiseFunction1D* pOxyFunc = 0;
		IPiecewiseFunction1D* pDeOxyFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pOxyFunc );
		RISE_API_CreatePiecewiseLinearFunction1D( &pDeOxyFunc );

		pOxyFunc->addControlPoints( count, SkinData::omlc_prahl_hemoglobin_wavelengths, SkinData::omlc_prahl_oxyhemoglobin );
		pDeOxyFunc->addControlPoints( count, SkinData::omlc_prahl_hemoglobin_wavelengths, SkinData::omlc_prahl_deoxyhemoglobin );

        pOxyHemoglobinExt = pOxyFunc;
		pDeoxyHemoglobinExt = pDeOxyFunc;
	}

	// Bilirubin absorption function
	{
		const int count = sizeof( SkinData::omlc_prahl_bilirubin_wavelengths ) / sizeof( Scalar );

		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );

		pFunc->addControlPoints( count, SkinData::omlc_prahl_bilirubin_wavelengths, SkinData::omlc_prahl_bilirubin  );

        pBilirubinExt = pFunc;
	}

	// Beta-carotene absorption function
	{
		const int count = sizeof( SkinData::omlc_prahl_betacarotene_wavelengths ) / sizeof( Scalar );

		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );

		pFunc->addControlPoints( count, SkinData::omlc_prahl_betacarotene_wavelengths, SkinData::omlc_prahl_betacarotene  );

        pBetaCaroteneExt = pFunc;
	}

	hb_concentration = SkinData::hb_concen_whole_blood;				// Hemoglobin concentration
}

GenericHumanTissueSPF::~GenericHumanTissueSPF( )
{
	g.release();

	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBilirubinExt );
	safe_release( pBetaCaroteneExt );
}


Scalar GenericHumanTissueSPF::ComputeTissueAbsorptionCoefficient(
			const Scalar nm											///< [in] Wavelength of light to consider
			) const 
{
	const Scalar abs_hbo2 = BioSpecSkinSPF::ComputeHemoglobinAbsorptionCoefficient( nm, pOxyHemoglobinExt, hb_concentration ) * hb_ratio;
	const Scalar abs_hb = BioSpecSkinSPF::ComputeHemoglobinAbsorptionCoefficient( nm, pDeoxyHemoglobinExt, hb_concentration ) * (1.0-hb_ratio);
	const Scalar abs_bilirubin = pBilirubinExt->Evaluate( nm ) * bilirubin_concentration / 585.0;
	const Scalar abs_carotene = pBetaCaroteneExt->Evaluate( nm ) * betacarotene_concentration / 537.0;

	return (abs_hbo2+abs_hb+abs_bilirubin+abs_carotene)*whole_blood;
}

void GenericHumanTissueSPF::Scatter( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay trans;
	trans.ray.origin = ri.ray.origin;
	trans.type = ScatteredRay::eRayTranslucent;
	trans.kray = RISEPel(1.0,1.0,1.0);

	const Scalar cos_alpha = Vector3Ops::Dot(-ri.onb.w(), ri.ray.dir );

	if( cos_alpha < NEARZERO ) {
		// We are coming from the inside of the object
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );

		// Check if it gets absorbed
		const Scalar absorption = ComputeTissueAbsorptionCoefficient( random.CanonicalRandom()*400.0+380.0 );
		const Scalar x = random.CanonicalRandom();
		const Scalar pa = (1.0-exp(-(absorption*distance)));

		if( x < pa ) {
			// It got absorbed... oh well
			return;
		}

		const Scalar ps = (1.0-exp(-(sca.GetColor(ri)[0] * distance)));

		if( x < (pa + ps) ) {
			// Scattering
			if( diffuse ) {
				// Just diffusely scatter the ray and send it on its way
				trans.ray.dir = GeometricUtilities::Perturb( ri.ray.dir, 
					acos( sqrt(random.CanonicalRandom()) ),
					random.CanonicalRandom() * TWO_PI 
				);
			} else {
				// Apply the henyey-greenstein phase function for the scattering
				trans.ray.dir = BioSpecSkinSPF::Scattering_From_HenyeyGreenstein( random, ri.ray.dir, g.GetColor(ri)[0] );
			}
		}
		
		// Otherwise its just transmitted!
		trans.ray.dir = ri.ray.dir;
	} else{
		if( diffuse ) {
			// Just diffusely scatter the ray and send it on its way
			trans.ray.dir = GeometricUtilities::Perturb( ri.ray.dir, 
				acos( sqrt(random.CanonicalRandom()) ),
				random.CanonicalRandom() * TWO_PI 
			);
		} else {
			// Apply the henyey-greenstein phase function for the scattering
			trans.ray.dir = BioSpecSkinSPF::Scattering_From_HenyeyGreenstein( random, ri.ray.dir, g.GetColor(ri)[0] );
		}
	}

	scattered.AddScatteredRay( trans );
}

void GenericHumanTissueSPF::ScatterNM( 
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	const RandomNumberGenerator& random,				///< [in] Random number generator
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack* const ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay trans;
	trans.ray.origin = ri.ray.origin;
	trans.type = ScatteredRay::eRayTranslucent;
	trans.krayNM = 1.0;

	const Scalar cos_alpha = Vector3Ops::Dot(ri.onb.w(), ri.ray.dir );

	if( cos_alpha > NEARZERO ) {
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );

		// Check if it gets absorbed
		const Scalar absorption = ComputeTissueAbsorptionCoefficient( nm );
		const Scalar x = random.CanonicalRandom();
		const Scalar pa = (1.0-exp(-(absorption*distance)));
		
		if( x < pa ) {
			// It got absorbed... oh well
			return;
		}

		const Scalar ps = (1.0-exp(-(sca.GetColorNM(ri,nm) * distance)));

		if( x < (pa + ps) ) {
			// Scattering
			if( diffuse ) {
				// Just diffusely scatter the ray and send it on its way
				trans.ray.dir = GeometricUtilities::Perturb( ri.ray.dir, 
					acos( sqrt(random.CanonicalRandom()) ),
					random.CanonicalRandom() * TWO_PI 
				);
			} else {
				// Apply the henyey-greenstein phase function for the scattering
				trans.ray.dir = BioSpecSkinSPF::Scattering_From_HenyeyGreenstein( random, ri.ray.dir, g.GetColor(ri)[0] );
			}
		}
		
		// Otherwise its just transmitted!
		trans.ray.dir = ri.ray.dir;
	} else{
		if( diffuse ) {
			// Just diffusely scatter the ray and send it on its way
			trans.ray.dir = GeometricUtilities::Perturb( ri.ray.dir, 
				acos( sqrt(random.CanonicalRandom()) ),
				random.CanonicalRandom() * TWO_PI 
			);
		} else {
			// Apply the henyey-greenstein phase function for the scattering
			trans.ray.dir = BioSpecSkinSPF::Scattering_From_HenyeyGreenstein( random, ri.ray.dir, g.GetColorNM(ri,nm) );
		}
	}

	scattered.AddScatteredRay( trans );
}
