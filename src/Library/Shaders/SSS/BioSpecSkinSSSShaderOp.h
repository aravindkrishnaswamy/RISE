//////////////////////////////////////////////////////////////////////
//
//  BioSpecSkinSSSShaderOp.h - Octree-based BSSRDF evaluation for
//  the BioSpec biophysical skin model with 4-layer multipole
//  diffusion.
//
//  This is the octree rendering counterpart to BioSpecDiffusionProfile
//  (which uses BDPT importance sampling).  It provides:
//    - Noise-free deterministic BSSRDF evaluation via hierarchical
//      irradiance integration (Jensen & Buhler 2002)
//    - Tabulated 4-layer multipole profiles with hybrid Hankel+dipole
//      correction for numerical robustness
//    - Spatially-varying skin parameters via optional offset painters
//      and a precomputed 3D profile LUT
//
//  The 4-layer model (SC, epidermis, papillary dermis, reticular
//  dermis) uses BioSpec's original scattering formulas:
//    - SC/epidermis: Bashkatov 2005 (73.7*(nm/500)^-2.33)
//    - Dermis: Rayleigh scattering from collagen (ComputeBeta)
//
//  References:
//    Krishnaswamy & Baranoski 2004 — BioSpec skin model
//    Donner & Jensen 2005 — Multipole diffusion
//    Jensen & Buhler 2002 — Hierarchical irradiance integration
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BIOSPEC_SKIN_SSS_SHADER_OP_H
#define BIOSPEC_SKIN_SSS_SHADER_OP_H

#include "../../Interfaces/IShaderOp.h"
#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Interfaces/IPainter.h"
#include "../../Interfaces/IFunction1D.h"
#include "../../Utilities/Reference.h"
#include "../../Utilities/Threads/Threads.h"
#include "../../Materials/MultipoleDiffusion.h"
#include "PointSetOctree.h"
#include <vector>
#include <map>

namespace RISE
{
	namespace Implementation
	{
		class BioSpecSkinSSSShaderOp :
			public virtual IShaderOp,
			public virtual ISubSurfaceExtinctionFunction,
			public virtual Reference
		{
		protected:
			virtual ~BioSpecSkinSSSShaderOp();

			// --- Octree configuration ---
			const unsigned int	numPoints;
			const Scalar		error;
			const unsigned int	maxPointsPerNode;
			const unsigned char	maxDepth;
			const Scalar		irrad_scale;
			const IShader&		shader;
			const bool			cache;

			// --- BioSpec skin parameters (scalars) ---
			const Scalar		thickness_SC;
			const Scalar		thickness_epidermis;
			const Scalar		thickness_papillary;
			const Scalar		thickness_reticular;
			const Scalar		ior_SC;
			const Scalar		ior_epidermis;
			const Scalar		ior_papillary;
			const Scalar		ior_reticular;
			const Scalar		concentration_eumelanin;
			const Scalar		concentration_pheomelanin;
			const Scalar		melanosomes_in_epidermis;
			const Scalar		hb_ratio;
			const Scalar		whole_blood_papillary;
			const Scalar		whole_blood_reticular;
			const Scalar		bilirubin_concentration;
			const Scalar		betacarotene_SC;
			const Scalar		betacarotene_epidermis;
			const Scalar		betacarotene_dermis;

			// --- Optional offset painters (null = uniform) ---
			IPainter*			pOffsetMelanosomes;		///< Adds to melanosomes_in_epidermis
			IPainter*			pOffsetBloodPapillary;	///< Adds to whole_blood_papillary
			IPainter*			pOffsetBloodReticular;	///< Adds to whole_blood_reticular
			bool				m_has_offset_painters;

			// --- Chromophore LUTs ---
			IFunction1D*		pEumelaninExt;
			IFunction1D*		pPheomelaninExt;
			IFunction1D*		pOxyHemoglobinExt;
			IFunction1D*		pDeoxyHemoglobinExt;
			IFunction1D*		pBilirubinExt;
			IFunction1D*		pBetaCaroteneExt;
			Scalar				hb_concentration;

		public:
			static const int TABLE_SIZE = 1024;
		protected:

			// --- Tabulated profile (base) ---
			Scalar				m_table_r2_max;
			Scalar				m_table_r2_step;
			RISEPel				m_Rd_table[TABLE_SIZE];
			Scalar				m_max_distance;

			// --- Profile LUT for spatial variation ---
			static const int N_MEL = 10;	///< melanosomes axis
			static const int N_BLP = 8;		///< blood papillary axis
			static const int N_BLR = 6;		///< blood reticular axis
			static const int LUT_TOTAL = N_MEL * N_BLP * N_BLR;	///< 480

			RISEPel*			m_lut_tables;
			Scalar				m_mel_grid[N_MEL];
			Scalar				m_blp_grid[N_BLP];
			Scalar				m_blr_grid[N_BLR];
			Scalar				m_max_distance_lut;

			// --- Per-object octrees ---
			typedef std::map<const IObject*, PointSetOctree*> PointSetMap;
			mutable PointSetMap	pointsets;
			const RMutex		create_mutex;

			// --- Profile computation ---
			static Scalar ComputeSkinBaselineAbsorption( const Scalar nm );

			void ComputePerLayerCoefficients(
				const Scalar nm,
				const Scalar mel_frac,
				const Scalar blood_pap,
				const Scalar blood_ret,
				LayerParams layers_out[4]
				) const;

			void PrecomputeProfile();

			void TabulateProfileAtWavelength(
				const Scalar nm,
				const int channel
				);

			void TabulateProfileAtWavelengthInto(
				const Scalar nm,
				const int channel,
				const Scalar mel_frac,
				const Scalar blood_pap,
				const Scalar blood_ret,
				RISEPel* table_out,
				Scalar table_r2_max,
				Scalar table_r2_step
				) const;

			void PrecomputeLUT();

			void InterpolateProfile(
				Scalar mel, Scalar blp, Scalar blr,
				RISEPel* table_out
				) const;

			int LUTIndex( int i_mel, int i_blp, int i_blr ) const
			{
				return (i_mel * N_BLP * N_BLR + i_blp * N_BLR + i_blr) * TABLE_SIZE;
			}

		public:
			BioSpecSkinSSSShaderOp(
				const unsigned int numPoints_,
				const Scalar error_,
				const unsigned int maxPointsPerNode_,
				const unsigned char maxDepth_,
				const Scalar irrad_scale_,
				const IShader& shader_,
				const bool cache_,
				// BioSpec 18 params
				const Scalar thickness_SC_,
				const Scalar thickness_epidermis_,
				const Scalar thickness_papillary_,
				const Scalar thickness_reticular_,
				const Scalar ior_SC_,
				const Scalar ior_epidermis_,
				const Scalar ior_papillary_,
				const Scalar ior_reticular_,
				const Scalar concentration_eumelanin_,
				const Scalar concentration_pheomelanin_,
				const Scalar melanosomes_in_epidermis_,
				const Scalar hb_ratio_,
				const Scalar whole_blood_papillary_,
				const Scalar whole_blood_reticular_,
				const Scalar bilirubin_concentration_,
				const Scalar betacarotene_SC_,
				const Scalar betacarotene_epidermis_,
				const Scalar betacarotene_dermis_,
				// Optional offset painters
				IPainter* pOffsetMelanosomes_ = 0,
				IPainter* pOffsetBloodPapillary_ = 0,
				IPainter* pOffsetBloodReticular_ = 0
				);

			// --- IShaderOp interface ---

			void PerformOperation(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				RISEPel& c,
				const IORStack* const ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			Scalar PerformOperationNM(
				const RuntimeContext& rc,
				const RayIntersection& ri,
				const IRayCaster& caster,
				const IRayCaster::RAY_STATE& rs,
				const Scalar caccum,
				const Scalar nm,
				const IORStack* const ior_stack,
				const ScatteredRayContainer* pScat
				) const;

			void ResetRuntimeData() const;
			bool RequireSPF() const { return false; }

			// --- ISubSurfaceExtinctionFunction interface ---

			RISEPel ComputeTotalExtinction( const Scalar distance ) const;
			Scalar GetMaximumDistanceForError( const Scalar error ) const;
		};
	}
}

#endif
