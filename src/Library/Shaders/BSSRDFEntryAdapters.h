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
			const Scalar Sw = BSSRDFSampling::EvaluateSwWithFresnel( Ft, pProfile->GetIOR(ri) );
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
			return BSSRDFSampling::EvaluateSwWithFresnel( Ft, pProfile->GetIOR(ri) );
		}
	};

	/// Adapter BSDF for NEE at random-walk SSS entry points.
	/// Uses Schlick Fresnel with stored IOR.
	class RandomWalkEntryBSDF : public IBSDF
	{
		Scalar swScale;
		Scalar ior;

	public:
		RandomWalkEntryBSDF(
			const Scalar eta
			) : ior( eta )
		{
			const Scalar c = BSSRDFSampling::SchlickTransmissionNormalization( eta );
			swScale = (c > 1e-20) ? 1.0 / (c * PI) : 0;
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
			const Scalar F0v = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
			const Scalar F = F0v + (1.0 - F0v) * pow( 1.0 - cosTheta, 5.0 );
			const Scalar Ft = 1.0 - F;
			const Scalar Sw = Ft * swScale;
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
			const Scalar F0v = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
			const Scalar F = F0v + (1.0 - F0v) * pow( 1.0 - cosTheta, 5.0 );
			return (1.0 - F) * swScale;
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
