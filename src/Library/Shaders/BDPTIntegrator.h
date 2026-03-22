//////////////////////////////////////////////////////////////////////
//
//  BDPTIntegrator.h - Core bidirectional path tracing algorithm.
//
//    CONTEXT:
//    Standard unidirectional path tracing only discovers light paths
//    from the camera side.  For scenes where important light paths
//    are hard to find from the eye (caustics, small luminaries, SSS
//    materials), convergence is very slow.  BDPT generates subpaths
//    from both the camera and light sources, then connects them with
//    all possible (s,t) strategies, weighted by MIS to minimize
//    variance.
//
//    ARCHITECTURE:
//    BDPTIntegrator is a standalone algorithm class (not a rasterizer)
//    so it can be reused by both BDPTRasterizer (pixel-based) and
//    MLTRasterizer (Markov chain-based).  The public API is:
//
//    1. GenerateLightSubpath / GenerateEyeSubpath:
//       Trace a subpath from a sampled light or camera ray, storing
//       vertices with throughput (alpha), forward/reverse PDFs, and
//       delta flags.  Each vertex's throughput is the cumulative
//       path contribution from the subpath origin to that vertex.
//
//    2. ConnectAndEvaluate:
//       Evaluate a single (s,t) strategy — connect lightVerts[s-1]
//       to eyeVerts[t-1], check visibility, evaluate BSDFs, compute
//       the geometric term, and return the unweighted contribution.
//       Special cases: s=0 (eye path hits emitter), s=1 (next event
//       estimation), t=0/t=1 (light path connects to camera/sensor).
//
//    3. MISWeight:
//       Balance heuristic weight computed by walking along the full
//       path and accumulating ratios of forward/reverse PDFs at each
//       vertex, following Veach's thesis Section 10.2.1.
//
//    DIRECTION CONVENTIONS:
//    In RISE, BSDF::value(vLightIn, ri) expects:
//      - vLightIn (wi): direction AWAY from surface toward light
//      - ri.ray.Dir(): direction TOWARD surface (incoming viewer ray)
//    EvalBSDFAtVertex adapts to this by negating wo to build ri.
//    EvalPdfAtVertex negates wi (since SPF::Pdf expects the incoming
//    ray as ri.ray.Dir() and the outgoing direction as wo).
//
//    THROUGHPUT CONVENTION:
//    beta (path throughput) accumulates f*|cos|/pdf for non-delta
//    interactions, and kray directly for delta interactions.  The
//    stored vertex.throughput is the value of beta at that vertex,
//    representing the measurement contribution from the subpath
//    origin to that point.  The full path contribution for (s,t) is:
//      C = alpha_light * f_light * G * f_eye * alpha_eye
//    where alpha_light = lightVerts[s-1].throughput and similarly
//    for the eye side.
//
//    REFERENCES:
//    - Veach, E. "Robust Monte Carlo Methods for Light Transport
//      Simulation." PhD Thesis, Stanford, 1997. Chapters 8-10.
//    - Lafortune & Willems. "Bi-Directional Path Tracing." 1993.
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

			/// Helper: visibility test that passes through transparent/translucent
			/// materials (dielectric, SSS, translucent, etc).  Used for camera
			/// connections (t=0, t=1) so that splats behind transparent objects
			/// aren't blocked.
			bool IsVisibleThroughTransparents(
				const IScene& scene,
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

			/// Result of BSSRDF importance sampling at a surface vertex
			struct BSSRDFSampleResult
			{
				Point3				entryPoint;		///< Entry point on the surface
				Vector3				entryNormal;	///< Surface normal at entry point
				OrthonormalBasis3D	entryONB;		///< ONB at entry point
				Ray					scatteredRay;	///< Cosine-weighted ray from entry point
				RISEPel				weight;			///< BSSRDF weight: Rd * Ft(exit) * Ft(entry) / (c * pdfSurface)
				Scalar				weightNM;		///< Scalar weight for spectral path
				Scalar				cosinePdf;		///< PDF of the cosine-weighted direction
				Scalar				pdfSurface;		///< Spatial sampling PDF in area measure
				bool				valid;			///< True if sampling succeeded

				BSSRDFSampleResult() :
				weight( RISEPel(0,0,0) ), weightNM(0),
				cosinePdf(0), pdfSurface(0), valid(false) {}
			};

			/// Attempts BSSRDF importance sampling at a front-face hit
			/// on a material with a diffusion profile.
			///
			/// Uses the disk projection method (Christensen & Burley 2015):
			///   1. Choose a spectral channel uniformly (R, G, B)
			///   2. Choose a projection axis:
			///      normal (50%), tangent (25%), bitangent (25%)
			///   3. Sample radius r from the profile CDF for the channel
			///   4. Sample angle phi uniformly on [0, 2pi)
			///   5. Compute probe origin offset in the perpendicular plane
			///   6. Cast a probe ray along +-axis through the object
			///   7. If hit: evaluate Rd(r_actual), compute weight
			///   8. Generate cosine-weighted scattered ray from entry normal
			///
			/// The probe ray is cast against the specific object (pObject),
			/// not the whole scene, to ensure the entry point is on the
			/// same translucent surface.
			///
			/// \return A BSSRDFSampleResult with valid=true on success
			BSSRDFSampleResult SampleBSSRDFEntryPoint(
				const RayIntersectionGeometric& ri,		///< [in] Exit point intersection
				const IObject* pObject,					///< [in] Object to cast probe rays against
				const IMaterial* pMaterial,				///< [in] Material with diffusion profile
				const RandomNumberGenerator& rng		///< [in] RNG
				) const;
		};
	}
}

#endif
