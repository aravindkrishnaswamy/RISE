//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringShaderOp.h - BSSRDF evaluation via point
//  sampling and hierarchical octree summation
//
//  Implements the two-pass approach from Jensen & Buhler,
//  "A Rapid Hierarchical Rendering Technique for Translucent
//  Materials" (SIGGRAPH 2002):
//
//  Pass 1 (sample generation):
//    Distribute N sample points uniformly on the object surface.
//    At each point, evaluate incident irradiance using a separate
//    shader (typically direct lighting only).  Points with zero
//    illumination are discarded.  The samples are stored in a
//    PointSetOctree for hierarchical evaluation.
//
//  Pass 2 (BSSRDF evaluation):
//    For each shading point, query the octree to sum contributions
//    from all sample points weighted by the extinction function
//    Rd(r).  Distant clusters are approximated using the node's
//    average irradiance (the hierarchical acceleration).
//
//  The irrad_scale parameter absorbs the area-to-point-count ratio
//  and the unit conversion between the extinction function's
//  internal units (mm for the dipole) and scene-space meters.
//  It is effectively dA * unit_conversion and must be tuned
//  per-scene when changing numpoints or geometric_scale.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 18, 2005
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBSURFACE_SCATTERING_SHADER_OP_
#define SUBSURFACE_SCATTERING_SHADER_OP_

#include "../../Interfaces/IShaderOp.h"
#include "../../Interfaces/IMaterial.h"
#include "../../Interfaces/ISubSurfaceExtinctionFunction.h"
#include "../../Utilities/Reference.h"
#include "../../Utilities/Threads/Threads.h"
#include "PointSetOctree.h"
#include <vector>
#include <map>

namespace RISE
{
	namespace Implementation
	{
		class SubSurfaceScatteringShaderOp : 
			public virtual IShaderOp, 
			public virtual Reference
		{
		protected:
			virtual ~SubSurfaceScatteringShaderOp();

			const unsigned int numPoints;
			const Scalar error;
			const unsigned int maxPointsPerNode;
			const unsigned char maxDepth;
			const Scalar irrad_scale;
			const bool multiplyBSDF;
			const bool regenerate;
			const IShader& shader;
			const ISubSurfaceExtinctionFunction& extinction;
			const bool cache;
			const bool low_discrepancy;

			typedef std::map<const IObject*,PointSetOctree*> PointSetMap;
			// Known scene-immutability exception: pointsets are lazily built on first
			// access because construction requires ray tracing the scene. Access is
			// serialized by create_mutex (double-checked locking), so this is thread-safe.
			mutable PointSetMap	pointsets;

			const RMutex create_mutex;

		public:
			SubSurfaceScatteringShaderOp( 
				const unsigned int numPoints_,
				const Scalar error_,
				const unsigned int maxPointsPerNode_,
				const unsigned char maxDepth_,
				const Scalar irrad_scale_,
				const bool multiplyBSDF_,
				const bool regenerate_,
				const IShader& shader_,
				const ISubSurfaceExtinctionFunction& extinction_,
				const bool cache_,
				const bool low_discrepancy_
				);

			//! Tells the shader to apply shade to the given intersection point
			void PerformOperation( 
				const RuntimeContext& rc,					///< [in] Runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				RISEPel& c,									///< [in/out] Resultant color from op
				const IORStack& ior_stack,			///< [in/out] Index of refraction stack
				const ScatteredRayContainer* pScat			///< [in] Scattering information
				) const;

			//! Tells the shader to apply shade to the given intersection point for the given wavelength
			/// \return Amplitude of spectral function 
			Scalar PerformOperationNM( 
				const RuntimeContext& rc,					///< [in] Runtime context
				const RayIntersection& ri,					///< [in] Intersection information 
				const IRayCaster& caster,					///< [in] The Ray Caster to use for all ray casting needs
				const IRayCaster::RAY_STATE& rs,			///< [in] Current ray state
				const Scalar caccum,						///< [in] Current value for wavelength
				const Scalar nm,							///< [in] Wavelength to shade
				const IORStack& ior_stack,			///< [in/out] Index of refraction stack
				const ScatteredRayContainer* pScat			///< [in] Scattering information
				) const;

			//! Tells the ShaderOp to reset itself
			void ResetRuntimeData() const;

			//! Asks if the shader op needs SPF data
			bool RequireSPF() const { return false; };
		};
	}
}

#endif
