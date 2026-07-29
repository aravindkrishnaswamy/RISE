//////////////////////////////////////////////////////////////////////
//
//  MediumTransport.h - Medium transport utilities for participating
//    media integration in the path tracer
//
//  Provides adapter classes and helper functions needed to perform
//  next-event estimation (NEE) at medium scatter points.  A scatter
//  point has no surface BRDF — instead the phase function determines
//  the angular scattering distribution.  To reuse the existing
//  LightSampler::EvaluateDirectLighting infrastructure, we provide:
//
//  1. MediumScatterBSDF — adapts IPhaseFunction to IBSDF so the
//     light sampler can evaluate the scattered contribution.
//
//  2. MediumScatterMaterial — adapts IPhaseFunction to IMaterial
//     for MIS PDF queries against BSDF sampling.
//
//  3. EvaluateInScattering() — constructs a synthetic
//     RayIntersectionGeometric at the scatter point and calls
//     through to the LightSampler with the adapted BSDF/Material.
//
//  This follows the adapter pattern used by BSSRDF entry-point
//  sampling in PathTracingShaderOp.cpp (BSSRDFEntryBSDF/Material).
//
//  Aligned with Blender/Cycles scatter-point NEE in
//  integrator_shade_volume() (shade_volume.h).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 31, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MEDIUM_TRANSPORT_
#define MEDIUM_TRANSPORT_

#include "../Interfaces/IPhaseFunction.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IMedium.h"
#include "../Interfaces/IRayCaster.h"
#include "../Utilities/ISampler.h"

namespace RISE
{
	class IObject;
	class LightSampler;

	namespace Implementation
	{
		class LightSampler;
	}

	namespace MediumTransport
	{
		/// Owns the phase object used for one medium collision.  NM fire
		/// collisions can only acquire a wavelength-bound MakePhaseClosure;
		/// there is no code path from that branch to GetPhaseFunction.  Ordinary
		/// media borrow their legacy stateless phase.  Pel fire is unsupported
		/// until the separate preview increment and therefore resolves to null.
		class CollisionPhaseClosure
		{
			const IPhaseFunction* m_pPhase;
			bool m_owned;

			CollisionPhaseClosure( const CollisionPhaseClosure& ) = delete;
			CollisionPhaseClosure& operator=( const CollisionPhaseClosure& ) = delete;

		public:
			CollisionPhaseClosure(
				const IMedium& medium,
				const Point3& scatterPoint,
				const Scalar nm,
				const bool spectral
				);
			~CollisionPhaseClosure();

			const IPhaseFunction* Get() const { return m_pPhase; }
		};

		/// Closed preflight table that Phase B must consult before asking a medium
		/// for an NM continuation closure.  It checks the exact dynamic-type row
		/// and the phase parameters already available on that instance.  It does
		/// not claim that the Phase-B continuation factory/availability record has
		/// already been constructed: Phase B must add and call that
		/// default-unsupported factory after this gate before enabling competition.
		/// Derived and plugin types remain default-denied even when they inherit an
		/// eligible built-in.
		bool IsContinuationPhaseClosureNMPreflightAllowlisted( const IMedium& medium );

		/// Adapts an IPhaseFunction to the IBSDF interface so that
		/// LightSampler::EvaluateDirectLighting can evaluate the
		/// phase function at a medium scatter point.
		///
		/// Stack-allocated only — IReference stubs are no-ops.
		class MediumScatterBSDF : public IBSDF
		{
			const IPhaseFunction* m_pPhase;
			Vector3 m_wo;		///< Travel direction of arriving photon

		public:
			MediumScatterBSDF(
				const IPhaseFunction* pPhase,
				const Vector3& wo
				);

			// IReference stubs — lives on stack only
			void addref() const {}
			bool release() const { return false; }
			unsigned int refcount() const { return 1; }

			RISEPel value(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri
				) const;

			Scalar valueNM(
				const Vector3& vLightIn,
				const RayIntersectionGeometric& ri,
				const Scalar nm
				) const;
		};

		/// Adapts an IPhaseFunction to the IMaterial interface for
		/// MIS PDF queries.  Only Pdf() and PdfNM() are meaningful;
		/// all other IMaterial methods return null/defaults.
		///
		/// Stack-allocated only — IReference stubs are no-ops.
		class MediumScatterMaterial : public IMaterial
		{
			const IPhaseFunction* m_pPhase;
			Vector3 m_wo;

		public:
			MediumScatterMaterial(
				const IPhaseFunction* pPhase,
				const Vector3& wo
				);

			// IReference stubs
			void addref() const {}
			bool release() const { return false; }
			unsigned int refcount() const { return 1; }

			// IMaterial interface — only Pdf methods are meaningful
			IBSDF* GetBSDF() const { return 0; }
			ISPF* GetSPF() const { return 0; }
			IEmitter* GetEmitter() const { return 0; }

			Scalar Pdf(
				const Vector3& vToLight,
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const;

			Scalar PdfNM(
				const Vector3& vToLight,
				const RayIntersectionGeometric& ri,
				const Scalar nm,
				const IORStack& ior_stack
				) const;
		};


		/// Evaluates in-scattering at a medium scatter point using
		/// the LightSampler's NEE infrastructure.  Constructs a
		/// synthetic intersection at the scatter point with the
		/// incoming direction as the normal, and uses MediumScatterBSDF
		/// and MediumScatterMaterial as adapters.
		///
		/// \return In-scattered radiance (RGB)
		RISEPel EvaluateInScattering(
			const Point3& scatterPoint,								///< [in] World-space scatter point
			const Vector3& wo,										///< [in] Travel direction of arriving photon (= ray.Dir())
			const IMedium* pMedium,									///< [in] Current medium
			const IPhaseFunction* pPhase,							///< [in] Exact closure retained for this collision
			const IRayCaster& caster,								///< [in] Ray caster for shadow tests
			const Implementation::LightSampler* pLightSampler,		///< [in] Light sampler for NEE
			ISampler& sampler,										///< [in] Low-discrepancy sampler
			const RasterizerState& rast,							///< [in] Rasterizer state
			const IObject* pMediumObject							///< [in] Object enclosing the medium (NULL for global medium)
			);

		/// Spectral variant of EvaluateInScattering
		/// \return In-scattered radiance for a single wavelength
		Scalar EvaluateInScatteringNM(
			const Point3& scatterPoint,								///< [in] World-space scatter point
			const Vector3& wo,										///< [in] Travel direction of arriving photon (= ray.Dir())
			const IMedium* pMedium,									///< [in] Current medium
			const IPhaseFunction* pPhase,							///< [in] Exact wavelength-bound closure retained for this collision
			const Scalar nm,										///< [in] Wavelength in nanometers
			const IRayCaster& caster,								///< [in] Ray caster for shadow tests
			const Implementation::LightSampler* pLightSampler,		///< [in] Light sampler for NEE
			ISampler& sampler,										///< [in] Low-discrepancy sampler
			const RasterizerState& rast,							///< [in] Rasterizer state
			const IObject* pMediumObject							///< [in] Object enclosing the medium (NULL for global medium)
			);
	}
}

#endif
