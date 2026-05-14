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
				const IScalarPainter& thickness_SC_,								///< Thickness of the stratum corneum (in cm)
				const IScalarPainter& thickness_epidermis_,						///< Thickness of the epidermis (in cm)
				const IScalarPainter& thickness_papillary_dermis_,				///< Thickness of the papillary dermis (in cm)
				const IScalarPainter& thickness_reticular_dermis_,				///< Thickness of the reticular dermis (in cm)
				const IScalarPainter& ior_SC_,									///< Index of refraction of the stratum corneum
				const IScalarPainter& ior_epidermis_,								///< Index of refraction of the epidermis
				const IScalarPainter& ior_papillary_dermis_,						///< Index of refraction of the papillary dermis
				const IScalarPainter& ior_reticular_dermis_,						///< Index of refraction of the reticular dermis
				const IScalarPainter& concentration_eumelanin_,					///< Average Concentration of eumelanin in the melanosomes
				const IScalarPainter& concentration_pheomelanin_,					///< Average Concentration of pheomelanin in the melanosomes
				const IScalarPainter& melanosomes_in_epidermis_,					///< Percentage of the epidermis made up of melanosomes
				const IScalarPainter& hb_ratio_,									///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
				const IScalarPainter& whole_blood_in_papillary_dermis_,			///< Percentage of the papillary dermis made up of whole blood
				const IScalarPainter& whole_blood_in_reticular_dermis_,			///< Percentage of the reticular dermis made up of whole blood
				const IScalarPainter& bilirubin_concentration_,					///< Concentration of Bilirubin in whole blood
				const IScalarPainter& betacarotene_concentration_SC_,				///< Concentration of Beta-Carotene in the stratum corneum
				const IScalarPainter& betacarotene_concentration_epidermis_,		///< Concentration of Beta-Carotene in the epidermis
				const IScalarPainter& betacarotene_concentration_dermis_,			///< Concentration of Beta-Carotene in the dermis
				const IScalarPainter& folds_aspect_ratio_,						///< Aspect ratio of the little folds and wrinkles on the skin surface
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


