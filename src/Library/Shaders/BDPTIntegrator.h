//////////////////////////////////////////////////////////////////////
//
//  BDPTIntegrator.h - Core bidirectional path tracing algorithm.
//  Encapsulates light/eye subpath generation, connection strategies,
//  and MIS weight computation.  Separated from the rasterizer for
//  testability and future MLT reuse.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BDPT_INTEGRATOR_
#define BDPT_INTEGRATOR_

#include "../Interfaces/IReference.h"
#include "../Interfaces/IRayCaster.h"
#include "../Interfaces/IScene.h"
#include "../Interfaces/ICamera.h"
#include "../Utilities/Reference.h"
#include "../Utilities/ISampler.h"
#include "../Lights/LightSampler.h"
#include "BDPTVertex.h"
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		/// Encapsulates the full BDPT algorithm: subpath generation,
		/// connection, and MIS weight computation.
		class BDPTIntegrator :
			public virtual IReference,
			public virtual Reference
		{
		protected:
			unsigned int		maxEyeDepth;
			unsigned int		maxLightDepth;
			LightSampler*		pLightSampler;

			virtual ~BDPTIntegrator();

		public:
			BDPTIntegrator(
				unsigned int maxEye,
				unsigned int maxLight
				);

			void SetLightSampler( LightSampler* pSampler );

			/// Result of connecting a single (s,t) strategy
			struct ConnectionResult
			{
				RISEPel		contribution;	///< Unweighted path contribution
				Scalar		misWeight;		///< MIS weight for this (s,t) strategy
				Point2		rasterPos;		///< Pixel position for splatting (valid if needsSplat)
				bool		needsSplat;		///< True for light-side connections (s>=1, t<=1)
				bool		valid;			///< False if connection is invalid

				ConnectionResult() :
				contribution( RISEPel( 0, 0, 0 ) ),
				misWeight( 0 ),
				rasterPos( Point2( 0, 0 ) ),
				needsSplat( false ),
				valid( false )
				{
				}
			};

			/// Generates a light subpath starting from a sampled light source.
			/// \return Number of vertices stored
			unsigned int GenerateLightSubpath(
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices
				) const;

			/// Generates an eye subpath from a camera ray.
			/// \return Number of vertices stored
			unsigned int GenerateEyeSubpath(
				const RuntimeContext& rc,
				const Ray& cameraRay,
				const Point2& screenPos,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices
				) const;

			/// Connects and evaluates a single (s,t) strategy.
			ConnectionResult ConnectAndEvaluate(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,		///< Number of light subpath vertices used
				unsigned int t,		///< Number of eye subpath vertices used
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera
				) const;

			/// Evaluates all valid (s,t) strategies and returns results.
			std::vector<ConnectionResult> EvaluateAllStrategies(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera
				) const;

			/// Computes MIS weight using the balance heuristic (power=1).
			Scalar MISWeight(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,
				unsigned int t
				) const;

			//////////////////////////////////////////////////////////////////
			// Spectral (NM) variants -- single wavelength, scalar throughput
			//////////////////////////////////////////////////////////////////

			/// Result of connecting a single (s,t) strategy at a specific wavelength
			struct ConnectionResultNM
			{
				Scalar		contribution;
				Scalar		misWeight;
				Point2		rasterPos;
				bool		needsSplat;
				bool		valid;

				ConnectionResultNM() :
				contribution( 0 ),
				misWeight( 0 ),
				rasterPos( Point2( 0, 0 ) ),
				needsSplat( false ),
				valid( false )
				{
				}
			};

			unsigned int GenerateLightSubpathNM(
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices,
				const Scalar nm
				) const;

			unsigned int GenerateEyeSubpathNM(
				const RuntimeContext& rc,
				const Ray& cameraRay,
				const Point2& screenPos,
				const IScene& scene,
				const IRayCaster& caster,
				ISampler& sampler,
				std::vector<BDPTVertex>& vertices,
				const Scalar nm
				) const;

			ConnectionResultNM ConnectAndEvaluateNM(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				unsigned int s,
				unsigned int t,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				const Scalar nm
				) const;

			std::vector<ConnectionResultNM> EvaluateAllStrategiesNM(
				const std::vector<BDPTVertex>& lightVerts,
				const std::vector<BDPTVertex>& eyeVerts,
				const IScene& scene,
				const IRayCaster& caster,
				const ICamera& camera,
				const Scalar nm
				) const;

		protected:
			/// Helper: evaluates the BSDF at a surface vertex for given
			/// incoming and outgoing directions.
			RISEPel EvalBSDFAtVertex(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo
				) const;

			/// Helper: evaluates the SPF PDF at a surface vertex
			Scalar EvalPdfAtVertex(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo
				) const;

			/// Helper: checks if a connection between two points is unoccluded.
			bool IsVisible(
				const IRayCaster& caster,
				const Point3& p1,
				const Point3& p2
				) const;

			/// NM helper: evaluates the BSDF at a surface vertex for a single wavelength
			Scalar EvalBSDFAtVertexNM(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo,
				const Scalar nm
				) const;

			/// NM helper: evaluates the SPF PDF at a vertex for a single wavelength
			Scalar EvalPdfAtVertexNM(
				const BDPTVertex& vertex,
				const Vector3& wi,
				const Vector3& wo,
				const Scalar nm
				) const;

			/// NM helper: evaluates emitter radiance for a specific wavelength
			Scalar EvalEmitterRadianceNM(
				const BDPTVertex& vertex,
				const Vector3& outDir,
				const Scalar nm
				) const;
		};
	}
}

#endif
