//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinSPF.cpp - Implementation of the BioSpecSkinSPF
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "BioSpecSkinSPF.h"
#include "BioSpecSkinData.h"
#include "../Utilities/Optics.h"
#include "../Utilities/GeometricUtilities.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

BioSpecSkinSPF::BioSpecSkinSPF(
	const IPainter& thickness_SC_,								///< Thickness of the stratum corneum (in cm)
	const IPainter& thickness_epidermis_,						///< Thickness of the epidermis (in cm)
	const IPainter& thickness_papillary_dermis_,				///< Thickness of the papillary dermis (in cm)
	const IPainter& thickness_reticular_dermis_,				///< Thickness of the reticular dermis (in cm)
	const IPainter& ior_SC_,									///< Index of refraction of the stratum corneum
	const IPainter& ior_epidermis_,								///< Index of refraction of the epidermis
	const IPainter& ior_papillary_dermis_,						///< Index of refraction of the papillary dermis
	const IPainter& ior_reticular_dermis_,						///< Index of refraction of the reticular dermis
	const IPainter& concentration_eumelanin_,					///< Average Concentration of eumelanin in the melanosomes
	const IPainter& concentration_pheomelanin_,					///< Average Concentration of pheomelanin in the melanosomes
	const IPainter& melanosomes_in_epidermis_,					///< Percentage of the epidermis made up of melanosomes
	const IPainter& hb_ratio_,									///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
	const IPainter& whole_blood_in_papillary_dermis_,			///< Percentage of the papillary dermis made up of whole blood
	const IPainter& whole_blood_in_reticular_dermis_,			///< Percentage of the reticular dermis made up of whole blood
	const IPainter& bilirubin_concentration_,					///< Concentration of Bilirubin in whole blood
	const IPainter& betacarotene_concentration_SC_,				///< Concentration of Beta-Carotene in the stratum corneum
	const IPainter& betacarotene_concentration_epidermis_,		///< Concentration of Beta-Carotene in the epidermis
	const IPainter& betacarotene_concentration_dermis_,			///< Concentration of Beta-Carotene in the dermis
	const IPainter& folds_aspect_ratio_,						///< Aspect ratio of the little folds and wrinkles on the skin surface
	const bool bSubdermalLayer_									///< Should the model simulate a perfectly reflecting subdermal layer?
	) : 
  pnt_thickness_SC( thickness_SC_ ),
  pnt_thickness_epidermis( thickness_epidermis_ ),
  pnt_thickness_papillary_dermis( thickness_papillary_dermis_ ),
  pnt_thickness_reticular_dermis( thickness_reticular_dermis_ ),
  pnt_ior_SC( ior_SC_ ),
  pnt_ior_epidermis( ior_epidermis_ ),
  pnt_ior_papillary_dermis( ior_papillary_dermis_ ),
  pnt_ior_reticular_dermis( ior_reticular_dermis_ ),
  pnt_concentration_eumelanin( concentration_eumelanin_ ),
  pnt_concentration_pheomelanin( concentration_pheomelanin_ ),
  pnt_melanosomes_in_epidermis( melanosomes_in_epidermis_ ),
  pnt_hb_ratio( hb_ratio_ ),
  pnt_whole_blood_in_papillary_dermis( whole_blood_in_papillary_dermis_ ),
  pnt_whole_blood_in_reticular_dermis( whole_blood_in_reticular_dermis_ ),
  pnt_betacarotene_concentration_SC( betacarotene_concentration_SC_ ),
  pnt_betacarotene_concentration_epidermis( betacarotene_concentration_epidermis_ ),
  pnt_betacarotene_concentration_dermis( betacarotene_concentration_dermis_ ),
  pnt_bilirubin_concentration( bilirubin_concentration_ ),
  pnt_folds_aspect_ratio( folds_aspect_ratio_ ),
  subdermal_layer( bSubdermalLayer_ ),
  pSCscattering( 0 ), 
  pEpidermisScat( 0 ),
  pEumelaninExt( 0 ),
  pPheomelaninExt( 0 ),
  pOxyHemoglobinExt( 0 ),
  pDeoxyHemoglobinExt( 0 )
  {

	pnt_thickness_SC.addref();
	pnt_thickness_epidermis.addref();
	pnt_thickness_papillary_dermis.addref();
	pnt_thickness_reticular_dermis.addref();

	pnt_ior_SC.addref();
	pnt_ior_epidermis.addref();
	pnt_ior_papillary_dermis.addref();
	pnt_ior_reticular_dermis.addref();

	pnt_concentration_eumelanin.addref();
	pnt_concentration_pheomelanin.addref();

	pnt_melanosomes_in_epidermis.addref();

	pnt_hb_ratio.addref();
	pnt_whole_blood_in_papillary_dermis.addref();
	pnt_whole_blood_in_reticular_dermis.addref();

	pnt_betacarotene_concentration_SC.addref();
	pnt_betacarotene_concentration_epidermis.addref();
	pnt_betacarotene_concentration_dermis.addref();

	pnt_bilirubin_concentration.addref();

	pnt_folds_aspect_ratio.addref();	

	// Stratum corneum and epidermis scattering functions
	{
		const int count = sizeof( SkinData::exitant_angles ) / sizeof( Scalar );

		IPiecewiseFunction1D* pSCFunc302 = 0, *pSCFunc365 = 0, *pSCFunc436 = 0, *pSCFunc546 = 0;
		IPiecewiseFunction1D* pEpFunc302 = 0, *pEpFunc365 = 0, *pEpFunc436 = 0, *pEpFunc546 = 0;

		RISE_API_CreatePiecewiseLinearFunction1D( &pSCFunc302 );
		RISE_API_CreatePiecewiseLinearFunction1D( &pSCFunc365 );
		RISE_API_CreatePiecewiseLinearFunction1D( &pSCFunc436 );
		RISE_API_CreatePiecewiseLinearFunction1D( &pSCFunc546 );

		RISE_API_CreatePiecewiseLinearFunction1D( &pEpFunc302 );
		RISE_API_CreatePiecewiseLinearFunction1D( &pEpFunc365 );
		RISE_API_CreatePiecewiseLinearFunction1D( &pEpFunc436 );
		RISE_API_CreatePiecewiseLinearFunction1D( &pEpFunc546 );

		pSCFunc302->addControlPoints( count, SkinData::stratum_corneum_302, SkinData::exitant_angles );
		pEpFunc302->addControlPoints( count, SkinData::epidermis_302, SkinData::exitant_angles );

		pSCFunc365->addControlPoints( count, SkinData::stratum_corneum_365, SkinData::exitant_angles );
		pEpFunc365->addControlPoints( count, SkinData::epidermis_365, SkinData::exitant_angles );

		pSCFunc436->addControlPoints( count, SkinData::stratum_corneum_436, SkinData::exitant_angles );
		pEpFunc436->addControlPoints( count, SkinData::epidermis_436, SkinData::exitant_angles );

		pSCFunc546->addControlPoints( count, SkinData::stratum_corneum_546, SkinData::exitant_angles );
		pEpFunc546->addControlPoints( count, SkinData::epidermis_546, SkinData::exitant_angles );

		IPiecewiseFunction2D* pSC = 0;
		IPiecewiseFunction2D* pEp = 0;

		{
			pSCFunc302->setUseLUT( true );
			pSCFunc302->GenerateLUT( 1000 );

			pEpFunc302->setUseLUT( true );
			pEpFunc302->GenerateLUT( 1000 );

			pSCFunc365->setUseLUT( true );
			pSCFunc365->GenerateLUT( 1000 );

			pEpFunc365->setUseLUT( true );
			pEpFunc365->GenerateLUT( 1000 );

			pSCFunc436->setUseLUT( true );
			pSCFunc436->GenerateLUT( 1000 );

			pEpFunc436->setUseLUT( true );
			pEpFunc436->GenerateLUT( 1000 );

			pSCFunc546->setUseLUT( true );
			pSCFunc546->GenerateLUT( 1000 );

			pEpFunc546->setUseLUT( true );
			pEpFunc546->GenerateLUT( 1000 );
		}

		RISE_API_CreatePiecewiseLinearFunction2D( &pSC );
		RISE_API_CreatePiecewiseLinearFunction2D( &pEp );

		pSC->addControlPoint( 302.0, pSCFunc302 );
		pSC->addControlPoint( 365.0, pSCFunc365 );
		pSC->addControlPoint( 436.0, pSCFunc436 );
		pSC->addControlPoint( 546.0, pSCFunc546 );

		pEp->addControlPoint( 302.0, pEpFunc302 );
		pEp->addControlPoint( 365.0, pEpFunc365 );
		pEp->addControlPoint( 436.0, pEpFunc436 );
		pEp->addControlPoint( 546.0, pEpFunc546 );

		pSCscattering = pSC;
		pEpidermisScat = pEp;
	}

	// Eumelanin absorption function
	{
		const int count = sizeof( SkinData::omlc_eumelanin_wavelengths ) / sizeof( Scalar );

		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );

		pFunc->addControlPoints( count, SkinData::omlc_eumelanin_wavelengths, SkinData::omlc_eumelanin_ext_mgml );

		pEumelaninExt = pFunc;
	}

	// Pheomelanin absorption function
	{
		const int count = sizeof( SkinData::omlc_pheomelanin_wavelengths ) / sizeof( Scalar );

		IPiecewiseFunction1D* pFunc = 0;
		RISE_API_CreatePiecewiseLinearFunction1D( &pFunc );

		pFunc->addControlPoints( count, SkinData::omlc_pheomelanin_wavelengths, SkinData::omlc_pheomelanin_ext_mgml );

		pPheomelaninExt = pFunc;
	}

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

	hb_concentration = SkinData::hb_concen_whole_blood;											// blood is 2-12% of dermis
}

BioSpecSkinSPF::~BioSpecSkinSPF()
{
	safe_release( pSCscattering );
	safe_release( pEpidermisScat );
	safe_release( pEumelaninExt );
	safe_release( pPheomelaninExt );
	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBilirubinExt );
	safe_release( pBetaCaroteneExt );

	pnt_thickness_SC.release();
	pnt_thickness_epidermis.release();
	pnt_thickness_papillary_dermis.release();
	pnt_thickness_reticular_dermis.release();

	pnt_ior_SC.release();
	pnt_ior_epidermis.release();
	pnt_ior_papillary_dermis.release();
	pnt_ior_reticular_dermis.release();

	pnt_concentration_eumelanin.release();
	pnt_concentration_pheomelanin.release();

	pnt_melanosomes_in_epidermis.release();

	pnt_hb_ratio.release();
	pnt_whole_blood_in_papillary_dermis.release();
	pnt_whole_blood_in_reticular_dermis.release();

	pnt_betacarotene_concentration_SC.release();
	pnt_betacarotene_concentration_epidermis.release();
	pnt_betacarotene_concentration_dermis.release();

	pnt_bilirubin_concentration.release();

	pnt_folds_aspect_ratio.release();
}

bool BioSpecSkinSPF::ProcessSCInteraction( 
	const Scalar nm,											///< The wavelength we are working in
	const Vector3& photon_in,									///< The photon we are starting with
	Vector3& photon_out,										///< The photon coming out of the layer
	const OrthonormalBasis3D& onb,								///< The orthonormal basis
	const RandomNumberGenerator& random,				///< Random number generator for the MC process
	const bool bAtOutsideBoundary,								///< The photon is at the boundary with the outside of the skin,
																///< if false, it is at the epidermal boundary
	const bool bDoCellScattering,								///< Should we apply the Trowbridge cell scattering
	const bool bDoFresnel										///< Should we do a Fresnel check?
	) const
{
	Vector3				photon_temp = photon_in;

	if( bAtOutsideBoundary ) {
		if( bDoCellScattering ) {
			// Compute the Outside / stratum corneum boundary interaction
			const Scalar ref = Outside_SC_Boundary_Refraction( 
				photon_in,
				photon_temp, 
				onb, 
				1.0 );

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Take the reflectance and use the Trowbridge scattering
				photon_out = StratumCorneum_Cell_Scattering( random, 
								Optics::CalculateReflectedRay( photon_in, onb.w() ) );

				if( Vector3Ops::Dot(onb.w(), photon_out) > 0.0 ) {
					return false;
				}
				// Otherwise it was scattered back into the SC, so we just say it is absorbed
				return true;
			}
		}

		// The ray continues into the stratum corneum, perturb it
		photon_temp = StratumCorneum_Scattering( nm, random, photon_temp );

		// Continue the ray until the SC--Epidermis boundary
		// We need this distance

		// Use simple geometry
		const Scalar cos_alpha = Vector3Ops::Dot(-onb.w(), photon_temp);
		if( cos_alpha <= 0.0 ) {
			// perturbed right out of the stratum corneum, so just absorb it
//			return true;
			photon_out = photon_temp;
			return false;
		}

		const Scalar absorption = ComputeBetaCaroteneAbsorptionCoefficientStratumCorneum( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * cos_alpha;

		if( path_length <=  thickness_papillary_dermis ) {
			return true;
		}

		// No absorption

		return ProcessEpidermisInteraction(
			nm,
			photon_temp, 
			photon_out,
			onb,
			random,
			true, 
			true
			);
	} else {
		// Facing up, which means we process the epidermis to SC boundary
		// and move the ray up

		if( bDoFresnel ) {
			// Compute the epidermis / SC boundary interaction
			const Scalar ref = Epidermis_SC_Boundary_Refraction(
				photon_in,
				photon_temp,
				onb );

			if( ref > 0.0 ) {
				if( random.CanonicalRandom() < ref ) {

					photon_temp = Optics::CalculateReflectedRay( photon_in, -onb.w() );

					if( Vector3Ops::Dot(onb.w(), photon_temp) < 0.0 ) {
						// Ray is reflected back to the epidermis
						return ProcessEpidermisInteraction( 
							nm,
							photon_temp, 
							photon_out,
							onb, 
							random, 
							true, 
							false );
					}
				}
			}
		}

		// Otherwise, the ray continues into the stratum corneum, perturb it
		photon_temp = StratumCorneum_Scattering( nm, random, photon_temp );

		// Continue the ray until the Outside--SC boundary
		// We need this distance
		// Use simple geometry
		const Scalar cos_alpha = Vector3Ops::Dot(onb.w(), photon_temp);
		if( cos_alpha < 0.0 ) {
			// Bounced back into the epidermis
			return ProcessEpidermisInteraction(
				nm, 
				photon_temp,
				photon_out,
				onb, 
				random,
				true, 
				true );
		}

		const Scalar absorption = ComputeBetaCaroteneAbsorptionCoefficientStratumCorneum( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * cos_alpha;

		if( path_length <=  thickness_papillary_dermis ) {
			return true;
		}

		// No absorption

		// Check to see if it leaves the skin
		{
			Vector3 leaving_skin;

			// Compute the SC / outside boundary interaction
			const Scalar ref = SC_Outside_Boundary_Refraction(
				photon_temp,
				leaving_skin,
				onb,
				1.0
				);

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Ray is reflected back into the stratum corneum
				leaving_skin = Optics::CalculateReflectedRay( photon_temp, -onb.w() );

				// Ray is reflected back to the stratum corneum
				return ProcessSCInteraction( 
					nm,
					leaving_skin, 
					photon_out,
					onb, 
					random, 
					true,
					false,
					false );
			}

			// Otherwise, the photon escapes the skin and we return the result
			photon_out = photon_temp;
			return false;
		}
	}
}

//! Does epidermal interactions
//! Can pass the photon off to the papillary dermis or to the stratum corneum
//! \return TRUE if the photon was absorbed, FALSE otherwise
bool BioSpecSkinSPF::ProcessEpidermisInteraction(
	const Scalar nm,											///< The wavelength we are working in
	const Vector3& photon_in,									///< The photon we are starting with
	Vector3& photon_out,										///< The photon coming out of the layer
	const OrthonormalBasis3D& onb,								///< The orthonormal basis
	const RandomNumberGenerator& random,				///< Random number generator for the MC process
	const bool bAtSCBoundary,									///< The photon is at the boundary with the stratum corneum,
																///< if false, it is at the dermal boundary
	const bool bDoFresnel										///< Should we do a Fresnel check?
	) const
{
	Vector3				photon_temp = photon_in;

	if( bAtSCBoundary ) {
		// Facing down, at the SC boundary

		if( bDoFresnel ) {
			// Do the SC--Epidermis interaction
			const Scalar ref = SC_Epidermis_Boundary_Refraction( 
				photon_in,
				photon_temp, 
				onb );

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Reflects back to the stratum corneum
				photon_temp = Optics::CalculateReflectedRay( photon_in, onb.w() );

				if( Vector3Ops::Dot(-onb.w(), photon_temp) < 0.0 ) {
					return ProcessSCInteraction(
						nm,
						photon_temp, 
						photon_out,
						onb,
						random,
						false,
						false, 
						false);
				}
			}
		}

		
		Vector3 photon_scattered;
		do {
			photon_scattered = Epidermis_Scattering( nm, random, photon_temp );
		} while( Vector3Ops::Dot(-onb.w(), photon_scattered) < 0 );
		photon_temp = photon_scattered;
		const Scalar cos_alpha = Vector3Ops::Dot(-onb.w(), photon_temp);
		
		const Scalar absorption = ComputeEpidermisAbsorptionCoefficient( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * cos_alpha;

		if( path_length <=  thickness_epidermis ) {
			return true;
		}

		// Otherwise it makes it to the papillary dermis
		return ProcessPapillaryDermisInteraction(
			nm, 
			photon_temp,
			photon_out,
			onb,
			random,
			true, 
			true );
	} 
	else
	{
		// Facing up, at the dermis boundary

		if( bDoFresnel ) {
			// Do the Dermis--Epidermis interaction
			const Scalar ref = PapillaryDermis_Epidermis_Boundary_Refraction(
				photon_in,
				photon_temp,
				onb );

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Reflects back to the dermis
				photon_temp = Optics::CalculateReflectedRay( photon_in, -onb.w() );

				if( Vector3Ops::Dot(onb.w(), photon_temp) < 0.0 ) {
					return ProcessPapillaryDermisInteraction(
						nm,
						photon_temp,
						photon_out,
						onb, 
						random,
						true, 
						false );
				}
			}
		}

		
		Vector3 photon_scattered;
		do {
			photon_scattered = Epidermis_Scattering( nm, random, photon_temp );
		} while( Vector3Ops::Dot(onb.w(), photon_scattered) < 0 );
		photon_temp = photon_scattered;
		const Scalar cos_alpha = Vector3Ops::Dot(onb.w(), photon_scattered);
		
		const Scalar absorption = ComputeEpidermisAbsorptionCoefficient( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * cos_alpha;

		if( path_length <=  thickness_epidermis ) {
			return true;
		}

		return ProcessSCInteraction(
			nm,
			photon_temp,
			photon_out,
			onb, 
			random,
			false, 
			false, 
			true );
	}
}

//! Does papillary dermal interactions
//! Can pass the photon off to the epidermis or reticular dermis
//! \return TRUE if the photon was absorbed, FALSE otherwise
bool BioSpecSkinSPF::ProcessPapillaryDermisInteraction(
	const Scalar nm,											///< The wavelength we are working in
	const Vector3& photon_in,									///< The photon we are starting with
	Vector3& photon_out,										///< The photon coming out of the layer
	const OrthonormalBasis3D& onb,								///< The orthonormal basis
	const RandomNumberGenerator& random,				///< Random number generator for the MC process
	const bool bAtEpidermisBoundary,							///< The photon is at the boundary with the epidermis,
																///< if false, it is at the reticular dermis boundary
	const bool bDoFresnel										///< Should we do a Fresnel check?
	) const
{
	Vector3 photon_temp = photon_in;

	if( bAtEpidermisBoundary ) {
		// Facing down, at the epidermis boundary

		if( bDoFresnel ) {
			// Do the Epidermis--Dermis interaction
			const Scalar ref = Epidermis_PapillaryDermis_Boundary_Refraction( 
				photon_in,
				photon_temp, 
				onb );

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Reflects back to the epidermis
				photon_temp = Optics::CalculateReflectedRay( photon_in, onb.w() );

				if( Vector3Ops::Dot(-onb.w(), photon_temp ) < 0 ) {
					return ProcessEpidermisInteraction(
						nm,
						photon_temp, 
						photon_out,
						onb,
						random,
						false, 
						false );
				}
			}
		}

		// Do Rayleigh check
		const Scalar distance_to_reticular = thickness_papillary_dermis / Vector3Ops::Dot(-onb.w(), photon_temp );
		const Scalar prob_rayleigh = 1.0-ComputeRayleighScatteringProbability( nm, ior_papillary_dermis, distance_to_reticular );

		// Check to see if the ray is scattered
		if( random.CanonicalRandom() < prob_rayleigh ) {
			// The ray is Rayleigh scattered, get the direction
			photon_temp = Rayleigh_Phase_Function_Scattering( random, photon_temp );
		} else {
			// The ray continues into the dermis, perturb it
			photon_temp = Dermis_Scattering( random, -onb.w() );
		}

		// Make sure the scattering didn't result in us throwing the ray back up to the epidermis
		if( Vector3Ops::Dot(-onb.w(), photon_temp ) < 0.0 ) {
			// bounced back to the epidermis
			return ProcessEpidermisInteraction(
				nm,
				photon_temp,
				photon_out,
				onb,
				random,
				false, 
				true );
		}

		const Scalar absorption = ComputePapillaryDermisAbsorptionCoefficient( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * Vector3Ops::Dot(-onb.w(), photon_temp );

		if( path_length <=  thickness_papillary_dermis ) {
			return true;
		}

		// No scattering or absorption
		return ProcessReticularDermisInteraction(
			nm,
			photon_temp,
			photon_out,
			onb, 
			random, 
			true, 
			true );
	}
	else 
	{
		// Facing up, at the dermis boundary

		if( bDoFresnel ) {
			// Do the Papillary--Reticular interaction
			// Ray coming up from the reticular dermis
			const Scalar ref = ReticularLayer_PapillaryDermis_Boundary_Refraction(
				photon_in,
				photon_temp,
				onb );

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Reflects back to the reticular dermis
				photon_temp = Optics::CalculateReflectedRay( photon_in, -onb.w() );

				if( Vector3Ops::Dot(onb.w(), photon_temp) < 0.0 ) {
					return ProcessReticularDermisInteraction(
						nm,
						photon_temp,
						photon_out,
						onb, 
						random,
						true, 
						false );
				}
			}
		}

		// We don't bother with the Rayleigh check, since it was already done on the 
		// way down

		photon_temp = Dermis_Scattering( random, onb.w() );
		
		const Scalar absorption = ComputePapillaryDermisAbsorptionCoefficient( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * Vector3Ops::Dot(onb.w(), photon_temp );

		if( path_length <=  thickness_papillary_dermis ) {
			return true;
		}

		// No scattering or absorption
		return ProcessEpidermisInteraction(
			nm,
			photon_temp,
			photon_out,
			onb, 
			random,
			false,
			true );
	}
}

//! Does reticular dermal interactions
//! Can pass the photon off to the epidermis or reticular dermis
//! \return TRUE if the photon was absorbed, FALSE otherwise
bool BioSpecSkinSPF::ProcessReticularDermisInteraction(
	const Scalar nm,											///< The wavelength we are working in
	const Vector3& photon_in,									///< The photon we are starting with
	Vector3& photon_out,										///< The photon coming out of the layer
	const OrthonormalBasis3D& onb,								///< The orthonormal basis
	const RandomNumberGenerator& random,				///< Random number generator for the MC process
	const bool bAtPapillaryDermisBoundary,						///< The photon is at the boundary with the papillary dermis,
																///< if false, it is at the subdermal boundary
	const bool bDoFresnel										///< Should we do a Fresnel check?
	) const
{
	Vector3 photon_temp = photon_in;

	if( bAtPapillaryDermisBoundary ) {
		// Facing down, at the papillary dermis boundary
		if( bDoFresnel ) {
			// Do the Epidermis--Dermis interaction
			const Scalar ref = PapillaryDermis_ReticularLayer_Boundary_Refraction( 
				photon_in,
				photon_temp, 
				onb );

			if( ref > 0.0 && random.CanonicalRandom() < ref ) {
				// Reflects back to the papillary dermis
				photon_temp = Optics::CalculateReflectedRay( photon_in, onb.w() );

				if( Vector3Ops::Dot(-onb.w(), photon_temp ) < 0 ) {
					return ProcessPapillaryDermisInteraction(
						nm,
						photon_temp, 
						photon_out,
						onb,
						random,
						false, 
						false );
				}
			}
		}

		// Do Rayleigh check
		const Scalar distance_to_subdermis = thickness_reticular_dermis / Vector3Ops::Dot(-onb.w(), photon_temp );
		const Scalar prob_rayleigh = 1.0-ComputeRayleighScatteringProbability( nm, ior_reticular_dermis, distance_to_subdermis );

		// Check to see if the ray is scattered
		if( random.CanonicalRandom() < prob_rayleigh ) {
			// The ray is Rayleigh scattered, get the direction
			photon_temp = Rayleigh_Phase_Function_Scattering( random, photon_temp );
		} else {
			// The ray continues into the dermis, perturb it
			photon_temp = Dermis_Scattering( random, -onb.w() );
		}

		if( Vector3Ops::Dot(-onb.w(), photon_temp ) < 0.0 ) {
			// bounced back to the papillary derms
			return ProcessPapillaryDermisInteraction(
				nm,
				photon_temp,
				photon_out,
				onb,
				random,
				false, 
				true );
		}

		const Scalar absorption = ComputeReticularDermisAbsorptionCoefficient( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * Vector3Ops::Dot(-onb.w(), photon_temp );

		if( path_length <=  thickness_reticular_dermis ) {
			return true;
		}

		// No scattering or absorption
		return ProcessSubdermisInteraction(
			nm,
			photon_temp,
			photon_out,
			onb, 
			random );
	}
	else 
	{
		// Ray coming up from the subdermis
		// Continue into the dermis towards the papillary dermis

		// We don't bother with the Rayleigh check, since it was already done on the 
		// way down

		photon_temp = Dermis_Scattering( random, onb.w() );

		const Scalar absorption = ComputeReticularDermisAbsorptionCoefficient( nm );
		const Scalar path_length = (-1.0 / absorption) * log(random.CanonicalRandom()) * Vector3Ops::Dot(onb.w(), photon_temp);

		if( path_length <=  thickness_reticular_dermis ) {
			return true;
		}

		// No scattering or absorption
		return ProcessPapillaryDermisInteraction(
			nm,
			photon_temp,
			photon_out,
			onb, 
			random,
			false, 
			true );
	}
}

//! Does subdermal interactions
//! Can pass the photon off to the dermis
//! \return TRUE if the photon was absorbed, FALSE otherwise
bool BioSpecSkinSPF::ProcessSubdermisInteraction(
	const Scalar nm,											///< The wavelength we are working in
	const Vector3& photon_in,									///< The photon we are starting with
	Vector3& photon_out,										///< The photon coming out of the layer
	const OrthonormalBasis3D& onb,								///< The orthonormal basis
	const RandomNumberGenerator& random				///< Random number generator for the MC process
	) const
{
	if( subdermal_layer ) {
		// Simply diffuses the photon back to the dermis
		Vector3 photon_temp;
		photon_temp = photon_in;
		const Vector3 vReflected = Optics::CalculateReflectedRay( photon_temp, onb.w() );

		return ProcessReticularDermisInteraction(
			nm, 
			vReflected,
			photon_out,
			onb,
			random,
			false, 
			true );
	}

	// Otherwise let it through
	photon_out = photon_in;
	return false;
}

void BioSpecSkinSPF::Scatter( 
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RandomNumberGenerator& random,				///< [in] Random number generator
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{

	// Setup the instance specific variables
	{
		thickness_SC = pnt_thickness_SC.GetColor(ri)[0];
		thickness_epidermis = pnt_thickness_epidermis.GetColor(ri)[0];
		thickness_papillary_dermis = pnt_thickness_papillary_dermis.GetColor(ri)[0];
		thickness_reticular_dermis = pnt_thickness_reticular_dermis.GetColor(ri)[0];

		ior_SC = pnt_ior_SC.GetColor(ri)[0];
		ior_epidermis = pnt_ior_epidermis.GetColor(ri)[0];
		ior_papillary_dermis = pnt_ior_papillary_dermis.GetColor(ri)[0];
		ior_reticular_dermis = pnt_ior_reticular_dermis.GetColor(ri)[0];

		concentration_eumelanin = pnt_concentration_eumelanin.GetColor(ri)[0];
		concentration_pheomelanin = pnt_concentration_pheomelanin.GetColor(ri)[0];

		melanosomes_in_epidermis = pnt_melanosomes_in_epidermis.GetColor(ri)[0];

		hb_ratio = pnt_hb_ratio.GetColor(ri)[0];
		whole_blood_in_papillary_dermis = pnt_whole_blood_in_papillary_dermis.GetColor(ri)[0];
		whole_blood_in_reticular_dermis = pnt_whole_blood_in_reticular_dermis.GetColor(ri)[0];

		betacarotene_concentration_SC = pnt_betacarotene_concentration_SC.GetColor(ri)[0];
		betacarotene_concentration_epidermis = pnt_betacarotene_concentration_epidermis.GetColor(ri)[0];
		betacarotene_concentration_dermis = pnt_betacarotene_concentration_dermis.GetColor(ri)[0];

		bilirubin_concentration = pnt_bilirubin_concentration.GetColor(ri)[0];

		folds_aspect_ratio = pnt_folds_aspect_ratio.GetColor(ri)[0];
	}

	Vector3 photon_out;

	// Figure out whether the ray is coming in from the top or
	// the bottom of the skin
	bool bAbsorbed = false;

	if( Vector3Ops::Dot( ri.ray.dir, ri.onb.w() ) < 0 ) {
		// From top, so send it to the stratum corneum
		bAbsorbed = ProcessSCInteraction(
			random.CanonicalRandom()*400.0+380.0,
			ri.ray.dir,
			photon_out,
			ri.onb,
			random,
			true, 
			true, 
			true );
	} else {
		// From bottom, so send it to the reticular dermis
		bAbsorbed = ProcessReticularDermisInteraction(
			random.CanonicalRandom()*400.0+380.0,
			ri.ray.dir,
			photon_out,
			ri.onb,
			random,
			false, 
			true );
	}
	
	if( !bAbsorbed ) {
		ScatteredRay remmitted;
		remmitted.ray.origin = ri.ptIntersection;
		remmitted.ray.dir = photon_out;
		remmitted.kray = RISEPel(1.0,1.0,1.0);
		scattered.AddScatteredRay( remmitted );
	}
}

void BioSpecSkinSPF::ScatterNM( 
		const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
		const RandomNumberGenerator& random,				///< [in] Random number generator
		const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
		ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
		const IORStack* const ior_stack								///< [in/out] Index of refraction stack
		) const
{
	// Setup the instance specific variables
	{
		thickness_SC = pnt_thickness_SC.GetColorNM(ri,nm);
		thickness_epidermis = pnt_thickness_epidermis.GetColorNM(ri,nm);
		thickness_papillary_dermis = pnt_thickness_papillary_dermis.GetColorNM(ri,nm);
		thickness_reticular_dermis = pnt_thickness_reticular_dermis.GetColorNM(ri,nm);

		ior_SC = pnt_ior_SC.GetColorNM(ri,nm);
		ior_epidermis = pnt_ior_epidermis.GetColorNM(ri,nm);
		ior_papillary_dermis = pnt_ior_papillary_dermis.GetColorNM(ri,nm);
		ior_reticular_dermis = pnt_ior_reticular_dermis.GetColorNM(ri,nm);

		concentration_eumelanin = pnt_concentration_eumelanin.GetColorNM(ri,nm);
		concentration_pheomelanin = pnt_concentration_pheomelanin.GetColorNM(ri,nm);

		melanosomes_in_epidermis = pnt_melanosomes_in_epidermis.GetColorNM(ri,nm);

		hb_ratio = pnt_hb_ratio.GetColorNM(ri,nm);
		whole_blood_in_papillary_dermis = pnt_whole_blood_in_papillary_dermis.GetColorNM(ri,nm);
		whole_blood_in_reticular_dermis = pnt_whole_blood_in_reticular_dermis.GetColorNM(ri,nm);

		betacarotene_concentration_SC = pnt_betacarotene_concentration_SC.GetColorNM(ri,nm);
		betacarotene_concentration_epidermis = pnt_betacarotene_concentration_epidermis.GetColorNM(ri,nm);
		betacarotene_concentration_dermis = pnt_betacarotene_concentration_dermis.GetColorNM(ri,nm);

		bilirubin_concentration = pnt_bilirubin_concentration.GetColorNM(ri,nm);

		folds_aspect_ratio = pnt_folds_aspect_ratio.GetColorNM(ri,nm);
	}

	Vector3 photon_out;

	// Figure out whether the ray is coming in from the top or
	// the bottom of the skin
	bool bAbsorbed = false;

	if( Vector3Ops::Dot( ri.ray.dir, ri.onb.w() ) < 0 ) {
		// From top, so send it to the stratum corneum
		bAbsorbed = ProcessSCInteraction(
			nm,
			ri.ray.dir,
			photon_out,
			ri.onb,
			random,
			true, 
			true, 
			true );
	} else {
		// From bottom, so send it to the reticular dermis
		bAbsorbed = ProcessReticularDermisInteraction(
			nm, 
			ri.ray.dir, 
			photon_out,
			ri.onb, 
			random, 
			false,
			true );
	}
	
	if( !bAbsorbed ) {
		ScatteredRay remmitted;
		remmitted.ray.origin = ri.ptIntersection;
		remmitted.ray.dir = photon_out;
		remmitted.krayNM = 1.0;
		scattered.AddScatteredRay( remmitted );	
	}
}

