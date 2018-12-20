//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinMaterial.h - The BioSpec skin material implementation
//    as described by Krishnaswamy and Baranoski
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIOSPEC_SKIN_MATERIAL_
#define BIOSPEC_SKIN_MATERIAL_

#include "BioSpecSkinSPF.h"

//
// The material
//
namespace RISE
{
	namespace Implementation
	{
		class BioSpecSkinMaterial : public virtual IMaterial, public virtual Implementation::Reference
		{
		protected:
			BioSpecSkinSPF*	pSPF;

			virtual ~BioSpecSkinMaterial()
			{
				safe_release( pSPF );
			}

		public:
			BioSpecSkinMaterial(
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
				const bool bSubdermalLayer									///< Should the model simulate a perfectly reflecting subdermal layer?
				) : 
			pSPF( 0 )
			{
				pSPF = new BioSpecSkinSPF(
					thickness_SC_,
					thickness_epidermis_,
					thickness_papillary_dermis_,
					thickness_reticular_dermis_,
					ior_SC_,
					ior_epidermis_,
					ior_papillary_dermis_,
					ior_reticular_dermis_,
					concentration_eumelanin_,
					concentration_pheomelanin_,
					melanosomes_in_epidermis_,
					hb_ratio_,
					whole_blood_in_papillary_dermis_,
					whole_blood_in_reticular_dermis_,
					bilirubin_concentration_,
					betacarotene_concentration_SC_,
					betacarotene_concentration_epidermis_,
					betacarotene_concentration_dermis_,
					folds_aspect_ratio_,
					bSubdermalLayer
					);
			}

			IBSDF* GetBSDF() const
			{
				return 0;
			}

			ISPF* GetSPF() const
			{
				return pSPF;
			}

			IEmitter* GetEmitter() const
			{
				return 0;
			}
		};
	}
}

#endif


