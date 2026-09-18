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
			// DL-183 nit: this ctor's parameter ORDER (hb_ratio_, whole_blood,
			// bilirubin_concentration, betacarotene_concentration) does NOT match
			// GenericHumanTissueSPF's ctor order (whole_blood_,
			// betacarotene_concentration_, bilirubin_concentration_, hb_ratio_)
			// below, and RISE_API_CreateGenericHumanTissueMaterial's call INTO
			// this ctor (RISE_API.cpp) does not match THIS ctor's declared names
			// either -- it passes (sca, g, whole_blood_, betacarotene_
			// concentration_, bilirubin_concentration_, hb_ratio_, diffuse)
			// positionally. Traced end to end (debt-dl183 review round 1): the
			// two mismatches are INVERSE PERMUTATIONS of each other, so each of
			// the four scalars ends up in the correctly-NAMED member at the SPF,
			// despite two layers of positionally-scrambled passthroughs. This is
			// confusing-but-currently-correct, NOT a defect -- do NOT "fix" only
			// ONE of the two call sites (this ctor's body, or the RISE_API.cpp
			// call site) without re-deriving the full chain; a one-sided fix
			// breaks the cancellation and silently swaps whole_blood/hb_ratio/
			// betacarotene_concentration at the SPF. See docs/DEBT_LEDGER.md's
			// DL-183 row for the full parameter-by-parameter trace.
			GenericHumanTissueMaterial(
				const IScalarPainter& sca,										///< Scattering co-efficient
				const IScalarPainter& g,											///< g factor in the HG phase function
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

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to the SPF (no BRDF for this material).  The four
			//! Scalar parameters (whole_blood, *_concentration, hb_ratio)
			//! are baked into the SPF at construction and not editable
			//! through this interface.
			inline const IScalarPainter& GetSca() const { return pSPF->GetSca(); }
			inline const IScalarPainter& GetG()   const { return pSPF->GetG(); }
			inline void SetSca( const IScalarPainter& v ) { pSPF->SetSca( v ); }
			inline void SetG( const IScalarPainter& v )   { pSPF->SetG( v ); }
		};
	}
}

#endif


