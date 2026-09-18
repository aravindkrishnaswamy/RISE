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
#include "HenyeyGreensteinPhaseFunction.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Optics.h"
#include "../RISE_API.h"

using namespace RISE;
using namespace RISE::Implementation;

// DL-183 nit: this ctor's parameter ORDER (whole_blood_, betacarotene_
// concentration_, bilirubin_concentration_, hb_ratio_) does NOT match
// GenericHumanTissueMaterial's ctor order (hb_ratio_, whole_blood,
// bilirubin_concentration, betacarotene_concentration) in
// GenericHumanTissueMaterial.h, which passes its own same-named members
// straight through positionally. Traced end to end (debt-dl183 review
// round 1): that mismatch is the INVERSE of a second one at
// RISE_API_CreateGenericHumanTissueMaterial's call into
// GenericHumanTissueMaterial, so the two cancel and each scalar lands in
// the correctly-NAMED member here despite two layers of positionally-
// scrambled passthroughs. Confusing-but-currently-correct, NOT a defect
// -- do NOT reorder this parameter list to "match" GenericHumanTissue-
// Material without re-deriving the full chain; see docs/DEBT_LEDGER.md's
// DL-183 row for the full trace.
GenericHumanTissueSPF::GenericHumanTissueSPF(
	const IScalarPainter& sca_,									///< Scattering co-efficient (how much scattering happens)
	const IScalarPainter& g_,										///< Anisotropy factor for the HG phase function
	const Scalar whole_blood_,								///< Amount of tissue composed of whole blood
	const Scalar betacarotene_concentration_,				///< Concentration of beta-carotene in the dermis
	const Scalar bilirubin_concentration_,					///< Concentration of bilirubin in whole blood
	const Scalar hb_ratio_,									///< Oxy/deoxy hemoglobin ratio
	const bool diffuse_										///< Is the scattering just diffuse ?
	) :
  pSca( &sca_ ),
  pG( &g_ ),
  whole_blood( whole_blood_ ),
  betacarotene_concentration( betacarotene_concentration_ ),
  bilirubin_concentration( bilirubin_concentration_ ),
  hb_ratio( hb_ratio_ ),
  diffuse( diffuse_ )
{
	pSca->addref();
	pG->addref();

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
	// Pre-refactor this released only `g` — `sca` leaked.  The
	// pointer rewrite makes the pair symmetric.
	safe_release( pSca );
	safe_release( pG );

	safe_release( pOxyHemoglobinExt );
	safe_release( pDeoxyHemoglobinExt );
	safe_release( pBilirubinExt );
	safe_release( pBetaCaroteneExt );
}

void GenericHumanTissueSPF::SetSca( const IScalarPainter& v ) { v.addref(); safe_release( pSca ); pSca = &v; }
void GenericHumanTissueSPF::SetG( const IScalarPainter& v )   { v.addref(); safe_release( pG );   pG   = &v; }


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
	ISampler& sampler,				///< [in] Sampler
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay trans;
	// DL-183: this used to be `ri.ray.origin` -- the INCOMING ray's
	// origin (the camera, or the previous bounce point), not the point
	// on THIS surface the ray actually hit.  Every continuation ray
	// started on the wrong line.  BioSpecSkinSPF's sibling sites
	// (`remmitted.ray.origin = ri.ptIntersection`) always did this
	// correctly.
	trans.ray.origin = ri.ptIntersection;
	trans.type = ScatteredRay::eRayTranslucent;
	trans.kray = RISEPel(1.0,1.0,1.0);
	trans.isDelta = false;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available (see DielectricSPF for detailed rationale)
	if( ior_stack.containsCurrent() ) {
		// We are coming from the inside of the object
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );

		// Check if it gets absorbed
		const Scalar absorption = ComputeTissueAbsorptionCoefficient( sampler.Get1D()*400.0+380.0 );
		const Scalar x = sampler.Get1D();
		const Scalar pa = (1.0-exp(-(absorption*distance)));

		if( x < pa ) {
			// It got absorbed... oh well
			return;
		}

		const Scalar ps = (1.0-exp(-(pSca->GetValuesAt(ri).v[0] * distance)));

		if( x < (pa + ps) ) {
			// Scattering
			if( diffuse ) {
				// Just diffusely scatter the ray and send it on its way
				trans.ray.SetDir(GeometricUtilities::Perturb( ri.ray.Dir(),
					acos( sqrt(sampler.Get1D()) ),
					sampler.Get1D() * TWO_PI
				));
			} else {
				// Apply the henyey-greenstein phase function for the scattering
				trans.ray.SetDir(HenyeyGreensteinPhaseFunction::SampleWithG( ri.ray.Dir(), sampler, pG->GetValuesAt(ri).v[0] ));
			}
		} else {
			// DL-184: this branch used to be unconditional (outside the
			// `if` above), so it ran AFTER the scattering branch too and
			// silently overwrote whatever direction it had just sampled
			// -- every interior interaction was straight-through
			// regardless of the scattering roll.  It belongs here only:
			// otherwise its just transmitted!
			trans.ray.SetDir(ri.ray.Dir());
		}
	} else{
		if( diffuse ) {
			// Just diffusely scatter the ray and send it on its way
			trans.ray.SetDir(GeometricUtilities::Perturb( ri.ray.Dir(),
				acos( sqrt(sampler.Get1D()) ),
				sampler.Get1D() * TWO_PI
			));
		} else {
			// Apply the henyey-greenstein phase function for the scattering
			trans.ray.SetDir(HenyeyGreensteinPhaseFunction::SampleWithG( ri.ray.Dir(), sampler, pG->GetValuesAt(ri).v[0] ));
		}
	}

	scattered.AddScatteredRay( trans );
}

void GenericHumanTissueSPF::ScatterNM(
	const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
	ISampler& sampler,				///< [in] Sampler
	const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
	ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
	const IORStack& ior_stack								///< [in/out] Index of refraction stack
	) const
{
	ScatteredRay trans;
	// DL-183: see Scatter()'s twin comment -- this used to be
	// `ri.ray.origin` instead of the actual hit point.
	trans.ray.origin = ri.ptIntersection;
	trans.type = ScatteredRay::eRayTranslucent;
	trans.krayNM = 1.0;
	trans.isDelta = false;

	// Use the IOR stack as the authoritative source for inside/outside
	// determination when available (see DielectricSPF for detailed rationale)
	if( ior_stack.containsCurrent() ) {
		const Scalar distance = Vector3Ops::Magnitude( Vector3Ops::mkVector3(ri.ray.origin, ri.ptIntersection) );

		// Check if it gets absorbed
		const Scalar absorption = ComputeTissueAbsorptionCoefficient( nm );
		const Scalar x = sampler.Get1D();
		const Scalar pa = (1.0-exp(-(absorption*distance)));
		
		if( x < pa ) {
			// It got absorbed... oh well
			return;
		}

		const Scalar ps = (1.0-exp(-(pSca->GetValueAtNM(ri,nm) * distance)));

		if( x < (pa + ps) ) {
			// Scattering
			if( diffuse ) {
				// Just diffusely scatter the ray and send it on its way
				trans.ray.SetDir(GeometricUtilities::Perturb( ri.ray.Dir(),
					acos( sqrt(sampler.Get1D()) ),
					sampler.Get1D() * TWO_PI
				));
			} else {
				// Apply the henyey-greenstein phase function for the scattering
				// DL-126 review round 4 (P2-2): this was `pG->GetValuesAt(ri).v[0]`
				// (the RGB accessor) inside ScatterNM's interior branch, so the
				// in-medium HG lobe's `g` was not actually wavelength-resolved
				// in the NM pipe -- the OUTSIDE branch two lines below already
				// uses `GetValueAtNM(ri,nm)` for this identical parameter.
				trans.ray.SetDir(HenyeyGreensteinPhaseFunction::SampleWithG( ri.ray.Dir(), sampler, pG->GetValueAtNM(ri,nm) ));
			}
		} else {
			// DL-184: see the RGB Scatter()'s twin comment -- this branch
			// used to be unconditional and silently overwrote the
			// scattering branch's own sampled direction.
			trans.ray.SetDir(ri.ray.Dir());
		}
	} else{
		if( diffuse ) {
			// Just diffusely scatter the ray and send it on its way
			trans.ray.SetDir(GeometricUtilities::Perturb( ri.ray.Dir(),
				acos( sqrt(sampler.Get1D()) ),
				sampler.Get1D() * TWO_PI
			));
		} else {
			// Apply the henyey-greenstein phase function for the scattering
			trans.ray.SetDir(HenyeyGreensteinPhaseFunction::SampleWithG( ri.ray.Dir(), sampler, pG->GetValueAtNM(ri,nm) ));
		}
	}

	scattered.AddScatteredRay( trans );
}
