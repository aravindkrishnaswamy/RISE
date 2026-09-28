//////////////////////////////////////////////////////////////////////
//
//  BSSRDFEntryAdapters.h - Stack-local IBSDF/IMaterial adapters for
//    NEE at subsurface scattering entry points.
//
//    These are used by both BDPTIntegrator and
//    PathTracingIntegrator for direct lighting evaluation at
//    BSSRDF entry points.  The IReference stubs are safe because
//    EvaluateDirectLighting never ref-counts its arguments.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 10, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BSSRDF_ENTRY_ADAPTERS_
#define BSSRDF_ENTRY_ADAPTERS_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ISubSurfaceDiffusionProfile.h"
#include "../Utilities/BSSRDFSampling.h"

namespace RISE
{
namespace BSSRDFAdapters
{
	/// Entry adapters have fixed outward support, Sw(wi) for Ns.wi > 0,
	/// independent of the nonlocal chord to the diffusion exit. A RIG stores
	/// an incoming ray; this frame makes view-facing light gates use that same
	/// outward hemisphere. Do not use it for ordinary surface BSDF vertices.
	inline Ray EntryEvaluationRay( const Point3& position, const Vector3& normal )
	{
		return Ray( position, -normal );
	}

	/// Adapter BSDF for NEE at disk-projection BSSRDF entry points.
	/// Uses the diffusion profile's FresnelTransmission for Sw.
	class BSSRDFEntryBSDF : public IBSDF
	{
		ISubSurfaceDiffusionProfile* pProfile;

	public:
		BSSRDFEntryBSDF(
			ISubSurfaceDiffusionProfile* profile,
			const Scalar /*originalHitEta*/
			) : pProfile( profile )
		{
			// Keep the constructor signature for source compatibility, but do not
			// cache their original-hit IOR. A textured profile can evaluate a
			// different IOR at the entry record supplied to value/valueNM.
		}

		void addref() const {}
		bool release() const { return false; }
		unsigned int refcount() const { return 1; }

		/// DL-49: the Sw normalization belongs to the SAME relative index
		/// the profile's FresnelTransmission evaluates -- the material's
		/// index over the record's exterior index (`ri.ambientIOR`, which
		/// every caller's entry record carries from the exit hit).
		Scalar RelativeEta( const RayIntersectionGeometric& ri ) const
		{
			return BSSRDFSampling::RelativeBoundaryIOR(
				pProfile->GetIOR( ri ), BSSRDFSampling::ExteriorIOR( ri ) );
		}

		RISEPel value(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return RISEPel( 0, 0, 0 );
			}
			const Scalar Ft = pProfile->FresnelTransmission( cosTheta, ri );
			const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( Ft, RelativeEta( ri ) );
			return RISEPel( Sw, Sw, Sw );
		}

		Scalar valueNM(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const Scalar nm
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return 0;
			}
			const Scalar Ft = pProfile->FresnelTransmission( cosTheta, ri );
			return BSSRDFSampling::EvaluateSwWithFresnel( Ft, RelativeEta( ri ) );
		}
	};

	/// Adapter BSDF for NEE at random-walk SSS entry points.
	/// Uses Schlick Fresnel with the material's stored (ABSOLUTE) IOR over
	/// the evaluation record's exterior index (DL-49).
	class RandomWalkEntryBSDF : public IBSDF
	{
		Scalar ior;

		/// Sw = Ft(cos; eta) / (c(eta) * PI) for the RELATIVE index
		/// eta = ior / ri.ambientIOR.  Every caller's evaluation record
		/// carries the exterior index of the exit hit (PT stamps its entry
		/// record, BDPT replays `BDPTVertex::mediumIOR`); a record without
		/// one defaults to air, which is the pre-DL-49 behaviour exactly.
		Scalar Sw( const Scalar cosTheta, const RayIntersectionGeometric& ri ) const
		{
			const Scalar eta = BSSRDFSampling::RelativeBoundaryIOR(
				ior, BSSRDFSampling::ExteriorIOR( ri ) );
			const Scalar c = BSSRDFSampling::SchlickTransmissionNormalization( eta );
			const Scalar swScale = (c > 1e-20) ? 1.0 / (c * PI) : 0;
			return BSSRDFSampling::RandomWalkSchlickTransmission( cosTheta, eta ) * swScale;
		}

	public:
		RandomWalkEntryBSDF(
			const Scalar eta
			) : ior( eta )
		{
		}

		void addref() const {}
		bool release() const { return false; }
		unsigned int refcount() const { return 1; }

		RISEPel value(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return RISEPel( 0, 0, 0 );
			}
			const Scalar s = Sw( cosTheta, ri );
			return RISEPel( s, s, s );
		}

		Scalar valueNM(
			const Vector3& vLightIn,
			const RayIntersectionGeometric& ri,
			const Scalar /*nm*/
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( vLightIn, ri.vNormal );
			if( cosTheta <= 0 ) {
				return 0;
			}
			return Sw( cosTheta, ri );
		}
	};

	/// Lightweight IMaterial adapter for MIS at BSSRDF entry points.
	/// Pdf returns cosine-weighted hemisphere PDF (cos/PI).
	class BSSRDFEntryMaterial : public IMaterial
	{
	public:
		BSSRDFEntryMaterial() {}

		void addref() const {}
		bool release() const { return false; }
		unsigned int refcount() const { return 1; }

		IBSDF* GetBSDF() const { return 0; }
		ISPF* GetSPF() const { return 0; }
		IEmitter* GetEmitter() const { return 0; }

		Scalar Pdf(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const IORStack& ior_stack
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( wo, ri.vNormal );
			return (cosTheta > 0) ? cosTheta * INV_PI : 0;
		}

		Scalar PdfNM(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const Scalar nm,
			const IORStack& ior_stack
			) const
		{
			return Pdf( wo, ri, ior_stack );
		}
	};

} // namespace BSSRDFAdapters
} // namespace RISE

#endif
