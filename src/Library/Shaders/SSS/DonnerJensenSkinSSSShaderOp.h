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

			// --- Optional offset painters (null = uniform, no spatial variation) ---
			// Each offset adds to its base scalar at the shading point:
			//   effective_value = base_scalar + offset_painter.GetColor(ri)[0]
			IPainter*			pOffsetMelanin;			///< Adds to melanin_fraction
			IPainter*			pOffsetHbEpidermis;		///< Adds to hemoglobin_epidermis
			IPainter*			pOffsetHbDermis;		///< Adds to hemoglobin_dermis
			bool				m_has_offset_painters;	///< true if any offset painter is non-null

			// --- Chromophore LUTs ---
			IFunction1D*		pEumelaninExt;
			IFunction1D*		pPheomelaninExt;
			IFunction1D*		pOxyHemoglobinExt;
			IFunction1D*		pDeoxyHemoglobinExt;
			IFunction1D*		pBetaCaroteneExt;
			Scalar				hb_concentration;

		public:
			// --- Tabulated profile (base, used when no offset painters) ---
			static const int TABLE_SIZE = 1024;
		protected:
			Scalar				m_table_r2_max;
			Scalar				m_table_r2_step;
			RISEPel				m_Rd_table[TABLE_SIZE];
			Scalar				m_max_distance;

			// --- Profile LUT for spatially-varying parameters ---
			// A 3D grid over (melanin_fraction, hemoglobin_epidermis, hemoglobin_dermis).
			// Each grid point stores a full TABLE_SIZE Rd(r) table.
			// At render time, trilinear interpolation selects the per-pixel profile.
			// Only allocated when offset painters are present.
			static const int N_MEL = 10;		///< Grid points for melanin axis
			static const int N_HBE = 6;			///< Grid points for hemoglobin_epidermis axis
			static const int N_HBD = 8;			///< Grid points for hemoglobin_dermis axis
			static const int LUT_TOTAL = N_MEL * N_HBE * N_HBD;	///< Total grid points (480)

			RISEPel*			m_lut_tables;	///< LUT_TOTAL × TABLE_SIZE, heap-allocated (null if uniform)
			Scalar				m_mel_grid[N_MEL];
			Scalar				m_hbe_grid[N_HBE];
			Scalar				m_hbd_grid[N_HBD];
			Scalar				m_max_distance_lut;		///< Conservative max distance across all grid points

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

			/// Overload for LUT: tabulate into a caller-provided table.
			void TabulateProfileAtWavelengthInto(
				const Scalar nm,
				const int channel,
				const Scalar mel_frac,
				const Scalar hb_epi,
				const Scalar hb_derm,
				RISEPel* table_out,
				Scalar table_r2_max,
				Scalar table_r2_step
				) const;

			/// Precompute the 3D profile LUT (called only when offset painters exist).
			void PrecomputeLUT();

			/// Trilinear interpolation of the LUT into caller's stack buffer.
			/// Thread-safe: reads only immutable LUT data, writes to caller's buffer.
			void InterpolateProfile(
				Scalar mel, Scalar hbe, Scalar hbd,
				RISEPel* table_out
				) const;

			/// Flattened index into m_lut_tables.
			int LUTIndex( int i_mel, int i_hbe, int i_hbd ) const
			{
				return (i_mel * N_HBE * N_HBD + i_hbe * N_HBD + i_hbd) * TABLE_SIZE;
			}

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
				const Scalar blood_oxygenation_,
				IPainter* pOffsetMelanin_ = 0,
				IPainter* pOffsetHbEpidermis_ = 0,
				IPainter* pOffsetHbDermis_ = 0
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
