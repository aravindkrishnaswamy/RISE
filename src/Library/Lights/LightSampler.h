//////////////////////////////////////////////////////////////////////
//
//  LightSampler.h - Light source sampling with explicit PDFs for
//  bidirectional path tracing.
//
//  BDPT requires explicit PDF values for every sampling decision
//  (light selection, position on surface, emission direction) so
//  that MIS weights can be computed.  The existing RISE light
//  classes (SpotLight, PointLight, mesh luminaries) provide
//  generateRandomPhoton() but no PDF queries.  LightSampler wraps
//  these with explicit PDF computation.
//
//  LIGHT SELECTION:
//  Lights are selected proportional to their radiant exitance
//  (MaxValue of RGB), matching PhotonTracer's strategy.  This
//  means brighter lights get proportionally more subpath starts.
//
//  EMISSION SAMPLING:
//  - Non-mesh lights (point/spot): delta position (pdfPosition=1),
//    uniform directional sampling over the sphere (pdfDir=1/4pi).
//  - Mesh luminaries: uniform position on surface (pdfPos=1/area),
//    cosine-weighted hemisphere direction (pdfDir=cos/pi).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LIGHT_SAMPLER_
#define LIGHT_SAMPLER_

#include "../Interfaces/IScene.h"
#include "../Interfaces/ILight.h"
#include "../Interfaces/IObject.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Reference.h"
#include "../Rendering/LuminaryManager.h"

namespace RISE
{
	namespace Implementation
	{
		/// Describes a sampled emission event from a light or mesh luminary
		struct LightSample
		{
			Point3			position;		///< Emission point
			Vector3			normal;			///< Surface normal at emission point
			Vector3			direction;		///< Emission direction
			RISEPel			Le;				///< Emitted radiance
			Scalar			pdfPosition;	///< Area density on light surface
			Scalar			pdfDirection;	///< Solid angle density of emission direction
			Scalar			pdfSelect;		///< Probability of selecting this light
			bool			isDelta;		///< True for point/spot lights (delta position)
			const ILight*	pLight;			///< Non-null for non-mesh lights
			const IObject*	pLuminary;		///< Non-null for mesh lights
		};

		/// Wraps existing light/luminaire sampling with explicit PDF computation
		/// for bidirectional path tracing.  Light selection is proportional to
		/// radiant exitance, following the same strategy as PhotonTracer.
		class LightSampler : public virtual Reference
		{
		protected:
			virtual ~LightSampler();

		public:
			LightSampler();

			/// Selects a light proportional to exitance, samples an emission
			/// position and direction, and fills the LightSample struct.
			/// \return True if a valid sample was generated, false if no lights exist
			bool SampleLight(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				const RandomNumberGenerator& random,				///< [in] Random number generator
				LightSample& sample									///< [out] The generated light sample
				) const;

			/// Returns the probability of selecting a given non-mesh light
			/// \return Selection probability proportional to exitance
			Scalar PdfSelectLight(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				const ILight& light									///< [in] The light to query
				) const;

			/// Returns the probability of selecting a given mesh luminary
			/// \return Selection probability proportional to exitance
			Scalar PdfSelectLuminary(
				const IScene& scene,								///< [in] The scene containing lights
				const LuminaryManager::LuminariesList& luminaries,	///< [in] List of mesh luminaries
				const IObject& luminary								///< [in] The luminary to query
				) const;

		protected:
			/// Computes the total exitance across all non-mesh lights and mesh luminaries
			Scalar ComputeTotalExitance(
				const IScene& scene,								///< [in] The scene
				const LuminaryManager::LuminariesList& luminaries	///< [in] List of mesh luminaries
				) const;
		};
	}
}

#endif
