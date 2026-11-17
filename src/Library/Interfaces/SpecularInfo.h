//////////////////////////////////////////////////////////////////////
//
//  SpecularInfo.h - Describes a material's specular (delta) behavior.
//
//  Used by both ISPF and IMaterial so that specular info can be
//  queried at either level of the interface hierarchy.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 25, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECULAR_INFO_
#define SPECULAR_INFO_

#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/RGBSpectra.h"

namespace RISE
{
	//! Information about a material's specular (delta distribution) behavior.
	//! Used by the specular manifold sampling solver to determine the constraint
	//! type (reflection vs refraction) and IOR at each specular vertex.
	struct SpecularInfo
	{
		bool    isSpecular;		///< True if this material has delta (specular) interactions
		bool    canRefract;		///< True if refraction is possible (not a pure mirror)
		Scalar  ior;			///< Index of refraction at this point (for refraction; 1.0 if reflection-only)
		RISEPel attenuation;	///< Color attenuation for transmitted/reflected light (e.g., colored glass refractance)
		Scalar attenuationNM; ///< Wavelength-resolved multiplier from GetSpecularInfoNM; never an RGB channel.
		bool attenuationAppliesToReflection; ///< False for transmission-only refractance (PerfectRefractorSPF).
		bool hasCustomSpecularFresnel; ///< SPF supplies a coating/interface law; query it at the solved angle.
		bool attenuationIsInteriorTransmittance; ///< Dielectric tau per world-unit distance, paid only on an exiting transmission (same contract as DielectricSPF).
		bool    valid;			///< True if this info was successfully computed
		bool    clearTransmission;	///< True ONLY for a CLEAR transmissive dielectric boundary (Dielectric / PerfectRefractor): light passes into a NON-scattering medium and out the far side.  Distinguishes a clear glass shell from a subsurface-scattering surface or a coat-over-substrate (Polished), which share isSpecular && canRefract but must BLOCK a transparent shadow ray.  Default false.
		bool    hasInterior;	///< True if this material tracks its own IOR-stack membership (Scatter/ScatterNM classify entry vs. exit from ior_stack.containsCurrent(), exactly like a refractor) even though it is NOT specular and carries no distinct index of its own (DL-46).  Example: TranslucentSPF's diffuse lampshade model -- its interior segments re-push the ENCLOSING medium's IOR unchanged, they never introduce a new numeric IOR.  `IORStackSeeding::SeedFromPoint` uses `canRefract || hasInterior` to decide whether an object needs probe-based containment tracking, and re-pushes the enclosing IOR (rather than reading `ior`) for a `hasInterior`-only entry.  Setting `canRefract` (or `isSpecular`) on such a material merely to be picked up by the seeding probe would misrepresent it to every OTHER `GetSpecularInfo` consumer (SMS chain building, dielectric-shadow-ray gating, etc.) -- use this flag instead.  Default false.

		// Compatibility for RGB-only material/SPF extensions. Authored spectra
		// override the NM query; this fallback uses the existing unbounded
		// reflectance uplift, preserving values above one and clamping negatives.
		void SetSpectralAttenuationFromRGB( Scalar nm )
		{
			RISEPel nonnegative = attenuation;
			for( unsigned int c = 0; c < 3; ++c )
				if( nonnegative[c] < 0 ) nonnegative[c] = 0;
			attenuationNM = nonnegative.r == nonnegative.g && nonnegative.r == nonnegative.b
				? nonnegative.r : RGBUnboundedSpectrum::FromRGB( nonnegative ).Eval( nm );
		}

		SpecularInfo() :
		isSpecular( false ),
		canRefract( false ),
		ior( 1.0 ),
		attenuation( 1.0, 1.0, 1.0 ),
		attenuationNM( 1.0 ),
		attenuationAppliesToReflection( true ),
		hasCustomSpecularFresnel( false ),
		attenuationIsInteriorTransmittance( false ),
		valid( false ),
		clearTransmission( false ),
		hasInterior( false )
		{
		}
	};
}

#endif
