//////////////////////////////////////////////////////////////////////
//
//  DonnerJensenSkinSSSShaderOp.h - Octree-based BSSRDF evaluation
//  for the Donner et al. 2008 spectral skin model.
//
//  Implements the Jensen & Buhler 2002 two-pass approach:
//
//  Pass 1 (sample generation):
//    Distribute N sample points on the object surface.
//    At each point, evaluate incident irradiance using a separate
//    shader (typically direct lighting).  Store the illuminated
//    samples in a PointSetOctree.
//
//  Pass 2 (BSSRDF evaluation):
//    For each shading point, query the octree to sum contributions
//    from all sample points weighted by the tabulated diffusion
//    profile Rd(r).  Distant clusters use the node's average
//    irradiance (hierarchical acceleration).
//
//  The diffusion profile is precomputed as a dense lookup table
//  from the Hankel-domain two-layer multipole (Donner & Jensen
//  2005), with hybrid post-processing to eliminate J0 ringing
//  artifacts in the inverse Hankel transform.  This tabulated
//  profile captures the full inter-layer coupling from the
//  adding-doubling without any analytic fitting approximation.
//
//  References:
//    Donner, Weyrich, d'Eon, Ramamoorthi, Rusinkiewicz 2008
//    Donner & Jensen 2005
//    Jensen & Buhler 2002
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DONNER_JENSEN_SKIN_SSS_SHADER_OP_H
#define DONNER_JENSEN_SKIN_SSS_SHADER_OP_H

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
		class DonnerJensenSkinSSSShaderOp :
			public virtual IShaderOp,
			public virtual ISubSurfaceExtinctionFunction,
			public virtual Reference
		{
		protected:
			virtual ~DonnerJensenSkinSSSShaderOp();

			// --- Octree configuration ---
			const unsigned int	numPoints;
			const Scalar		error;
			const unsigned int	maxPointsPerNode;
			const unsigned char	maxDepth;
			const Scalar		irrad_scale;
			const IShader&		shader;
			const bool			cache;

			// --- Donner-Jensen skin parameters ---
			const Scalar		melanin_fraction;
			const Scalar		melanin_blend;
			const Scalar		hemoglobin_epidermis;
			const Scalar		carotene_fraction;
			const Scalar		hemoglobin_dermis;
			const Scalar		epidermis_thickness;
			const Scalar		ior_epidermis;
			const Scalar		ior_dermis;
			const Scalar		blood_oxygenation;

			// --- Chromophore LUTs ---
			IFunction1D*		pEumelaninExt;
			IFunction1D*		pPheomelaninExt;
			IFunction1D*		pOxyHemoglobinExt;
			IFunction1D*		pDeoxyHemoglobinExt;
			IFunction1D*		pBetaCaroteneExt;
			Scalar				hb_concentration;

			// --- Tabulated profile ---
			static const int TABLE_SIZE = 1024;
			Scalar				m_table_r2_max;
			Scalar				m_table_r2_step;
			RISEPel				m_Rd_table[TABLE_SIZE];
			Scalar				m_max_distance;

			// --- Per-object octrees (lazy init) ---
			typedef std::map<const IObject*, PointSetOctree*> PointSetMap;
			mutable PointSetMap	pointsets;
			const RMutex		create_mutex;

			// --- Profile computation helpers ---
			static Scalar ComputeSkinBaselineAbsorption( const Scalar nm );
			static Scalar ComputeEpidermisScattering( const Scalar nm );

			void ComputePerLayerCoefficients(
				const Scalar nm,
				LayerParams layers_out[2]
				) const;

			/// Precompute the tabulated Rd(r) profile for all RGB wavelengths.
			void PrecomputeProfile();

			/// Compute Rd(r) at a single wavelength from the Hankel-domain
			/// multipole composite, with hybrid correction for J0 ringing.
			void TabulateProfileAtWavelength(
				const Scalar nm,
				const int channel
				);

		public:
			DonnerJensenSkinSSSShaderOp(
				const unsigned int numPoints_,
				const Scalar error_,
				const unsigned int maxPointsPerNode_,
				const unsigned char maxDepth_,
				const Scalar irrad_scale_,
				const IShader& shader_,
				const bool cache_,
				const Scalar melanin_fraction_,
				const Scalar melanin_blend_,
				const Scalar hemoglobin_epidermis_,
				const Scalar carotene_fraction_,
				const Scalar hemoglobin_dermis_,
				const Scalar epidermis_thickness_,
				const Scalar ior_epidermis_,
				const Scalar ior_dermis_,
				const Scalar blood_oxygenation_
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
			// Called by PointSetOctree::Evaluate() for each sample

			RISEPel ComputeTotalExtinction( const Scalar distance ) const;
			Scalar GetMaximumDistanceForError( const Scalar error ) const;
		};
	}
}

#endif
