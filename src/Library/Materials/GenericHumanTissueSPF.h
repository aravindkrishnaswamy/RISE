//////////////////////////////////////////////////////////////////////
//
//  GenericHumanTissueSPF.h - Defines generic human tissue
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GENERIC_HUMAN_TISSUE_SPF_
#define GENERIC_HUMAN_TISSUE_SPF_

#include "../Interfaces/ISPF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class GenericHumanTissueSPF : public virtual ISPF, public virtual Reference
		{
		protected:
			virtual ~GenericHumanTissueSPF( );

			const IPainter&				sca;							///< Scattering co-efficient (says how much scattering happens)
			const IPainter&				g;								///< Henyey-Greenstein phase function g factor

			const Scalar				whole_blood;					///< Amount of tissue composed of whole blood
			const Scalar				betacarotene_concentration;		///< Concentration of beta-carotene in the dermis
			const Scalar				bilirubin_concentration;		///< Concentration of bilirubin in whole blood
			const Scalar				hb_ratio;						///< Oxy/deoxy hemoglobin ratio

			const bool					diffuse;						///< Is this just diffuse tissue ?

			// Internal stuff
			IFunction1D*				pOxyHemoglobinExt;				///< Lookup table for oxyhemoglobin extinction (from absorption)
			IFunction1D*				pDeoxyHemoglobinExt;			///< Lookup table for deoxyhemoglobin extinction (from absorption)

			IFunction1D*				pBilirubinExt;					///< Lookup table for bilirubin extinction (from absorption)
			IFunction1D*				pBetaCaroteneExt;				///< Lookup table for beta-carotene extinction (from absorption)

			Scalar						hb_concentration;				///< Concentration of hemoglobin in whole blood	


			Scalar ComputeTissueAbsorptionCoefficient( 
				const Scalar nm											///< [in] Wavelength of light to consider
				) const;

		public:
			GenericHumanTissueSPF( 
				const IPainter& sca_,									///< Scattering co-efficient (how much scattering happens)
				const IPainter& g_,										///< Anisotropy factor for the HG phase function
				const Scalar whole_blood_,								///< Amount of tissue composed of whole blood
				const Scalar betacarotene_concentration,				///< Concentration of beta-carotene in the dermis
				const Scalar bilirubin_concentration,					///< Concentration of bilirubin in whole blood
				const Scalar hb_ratio_,									///< Oxy/deoxy hemoglobin ratio
				const bool diffuse_										///< Is the scattering just diffuse ?
				);

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors.  
			void	Scatter( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;

			//! Given parameters describing the intersection of a ray with a surface, this will return
			//! the reflected and transmitted rays along with attenuation factors which taking into 
			//! account spectral affects.  
			void	ScatterNM( 
				const RayIntersectionGeometric& ri,							///< [in] Geometric intersection details for point of intersection
				const RandomNumberGenerator& random,				///< [in] Random number generator
				const Scalar nm,											///< [in] Wavelength the material is to consider (only used for spectral processing)
				ScatteredRayContainer& scattered,							///< [out] The list of scattered rays from the surface
				const IORStack* const ior_stack								///< [in/out] Index of refraction stack
				) const;
		};
	}
}

#endif

