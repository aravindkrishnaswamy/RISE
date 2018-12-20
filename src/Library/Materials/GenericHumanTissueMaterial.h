//////////////////////////////////////////////////////////////////////
//
//  GenericHumanTissueMaterial.h - The BioSpec skin material implementation
//    as described by Krishnaswamy and Baranoski
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GENERIC_HUMAN_TISSUE_MATERIAL_
#define GENERIC_HUMAN_TISSUE_MATERIAL_

#include "GenericHumanTissueSPF.h"

namespace RISE
{
	//
	// The material
	//
	namespace Implementation
	{
		class GenericHumanTissueMaterial : public virtual IMaterial, public virtual Implementation::Reference
		{
		protected:
			GenericHumanTissueSPF*	pSPF;

			virtual ~GenericHumanTissueMaterial()
			{
				safe_release( pSPF );
			}

		public:
			GenericHumanTissueMaterial(
				const IPainter& sca,										///< Scattering co-efficient
				const IPainter& g,											///< g factor in the HG phase function
				const Scalar hb_ratio_,										///< Ratio of oxyhemoglobin to deoxyhemoglobin in blood
				const Scalar whole_blood,									///< Percentage of the tissue made up of whole blood
				const Scalar bilirubin_concentration,						///< Concentration of Bilirubin in whole blood
				const Scalar betacarotene_concentration,					///< Concentration of Beta-Carotene in whole blood
				const bool diffuse											///< Is the tissue just diffuse?
				) : 
			pSPF( 0 )
			{
				pSPF = new GenericHumanTissueSPF(
					sca,
					g, 
					hb_ratio_,
					whole_blood,
					bilirubin_concentration,
					betacarotene_concentration,
					diffuse
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


