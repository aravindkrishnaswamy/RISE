//////////////////////////////////////////////////////////////////////
//
//  BSSRDFEntryAdapters.h - Stack-local IBSDF/IMaterial adapters for
//    NEE at subsurface scattering entry points.
//
//    These are used by both PathTracingShaderOp and
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

namespace RISE
{
namespace BSSRDFAdapters
{
	/// Adapter BSDF for NEE at disk-projection BSSRDF entry points.
	/// Uses the diffusion profile's FresnelTransmission for Sw.
	class BSSRDFEntryBSDF : public IBSDF
	{
		ISubSurfaceDiffusionProfile* pProfile;
		Scalar swScale;

	public:
		BSSRDFEntryBSDF(
			ISubSurfaceDiffusionProfile* profile,
			const Scalar eta
			) : pProfile( profile )
		{
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;
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
			const Scalar Ft = pProfile->FresnelTransmission( cosTheta, ri );
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
			const Scalar Ft = pProfile->FresnelTransmission( cosTheta, ri );
			return Ft * swScale;
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
			const Scalar F0 = ((eta - 1.0) / (eta + 1.0)) * ((eta - 1.0) / (eta + 1.0));
			const Scalar c = (41.0 - 20.0 * F0) / 42.0;
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
			const IORStack* ior_stack
			) const
		{
			const Scalar cosTheta = Vector3Ops::Dot( wo, ri.vNormal );
			return (cosTheta > 0) ? cosTheta * INV_PI : 0;
		}

		Scalar PdfNM(
			const Vector3& wo,
			const RayIntersectionGeometric& ri,
			const Scalar nm,
			const IORStack* ior_stack
			) const
		{
			return Pdf( wo, ri, ior_stack );
		}
	};

} // namespace BSSRDFAdapters
} // namespace RISE

#endif
