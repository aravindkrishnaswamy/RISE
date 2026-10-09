//////////////////////////////////////////////////////////////////////
//
//  RandomWalkSSSMaterial.h - Material that uses random-walk
//  subsurface scattering (Chiang & Burley, SIGGRAPH 2016).
//
//  Subsurface transport is performed by tracing a volumetric random
//  walk inside the mesh geometry, using Beer-Lambert free-flight
//  distance sampling and Henyey-Greenstein phase function scattering.
//  The walk exits through the mesh surface and produces a re-emission
//  vertex.  This replaces the analytical diffusion profile (Rd(r))
//  and disk-projection sampling used by SubSurfaceScatteringMaterial.
//
//  The surface boundary is handled identically: GGX microfacet
//  reflection via SubSurfaceScatteringBSDF and SubSurfaceScatteringSPF.
//
//  Parameters:
//    ior         - Index of refraction at the surface boundary
//    sigma_a     - Absorption coefficient (per unit distance, per channel)
//    sigma_s     - Scattering coefficient (per unit distance, per channel)
//    g           - Henyey-Greenstein asymmetry parameter (-1 to 1)
//    roughness   - Surface roughness for microfacet boundary [0, 1]
//    max_bounces - Maximum walk steps (default 64)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 7, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RANDOM_WALK_SSS_MATERIAL_
#define RANDOM_WALK_SSS_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "SubSurfaceScatteringBSDF.h"
#include "SubSurfaceScatteringSPF.h"
#include "../Utilities/SSSCoefficients.h"

namespace RISE
{
	namespace Implementation
	{
		class RandomWalkSSSMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SubSurfaceScatteringBSDF*		pBSDF;
			SubSurfaceScatteringSPF*		pSPF;
			RandomWalkSSSParams				m_rwParams;
			//! Pointer so the interactive editor can rebind via SetIOR
			//! (the random-walk params capture a SCALAR snapshot of `ior`
			//! at construction; per-slot live edit on the walk params is
			//! out of scope).
			const IScalarPainter*			pIORPainter;
			//! DL-374: the coefficient painters are kept so the spectral
			//! query can evaluate them at the requested wavelength.
			const IScalarPainter*			pAbsorptionPainter;
			const IScalarPainter*			pScatteringPainter;
			const Scalar					surfaceRoughness;

			//! The fixed snapshot record every coefficient query reads
			//! (constructor, NM query and SetIOR use the same point).
			static RayIntersectionGeometric SnapshotRecord()
			{
				RayIntersectionGeometric ri(
					Ray( Point3(0,0,0), Vector3(0,1,0) ),
					nullRasterizerState );
				ri.bHit = true;
				ri.ptIntersection = Point3( 0, 0, 0 );
				ri.vNormal = Vector3( 0, 1, 0 );
				ri.onb.CreateFromW( ri.vNormal );
				return ri;
			}

			virtual ~RandomWalkSSSMaterial()
			{
				safe_release( pBSDF );
				safe_release( pSPF );
				safe_release( pIORPainter );
				safe_release( pAbsorptionPainter );
				safe_release( pScatteringPainter );
			}

		public:
			RandomWalkSSSMaterial(
				const IScalarPainter& ior,
				const IScalarPainter& absorption,
				const IScalarPainter& scattering,
				const Scalar g,
				const Scalar roughness,
				const unsigned int maxBounces
				) :
			pIORPainter( &ior ),
			pAbsorptionPainter( &absorption ),
			pScatteringPainter( &scattering ),
			surfaceRoughness( roughness )
			{
				pIORPainter->addref();
				pAbsorptionPainter->addref();
				pScatteringPainter->addref();

				pBSDF = new SubSurfaceScatteringBSDF( ior, g, roughness );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				pSPF = new SubSurfaceScatteringSPF( ior, g, roughness, true );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				// Evaluate painters at a dummy intersection to extract
				// scalar coefficients for the random walk.  Same
				// flatten-to-constant LIMITATION as before.
				const RayIntersectionGeometric dummyRI = SnapshotRecord();

				const ScalarTriple sa_t = absorption.GetValuesAt( dummyRI );
				const ScalarTriple ss_t = scattering.GetValuesAt( dummyRI );
				m_rwParams.sigma_a = RISEPel( sa_t.v[0], sa_t.v[1], sa_t.v[2] );
				m_rwParams.sigma_s = RISEPel( ss_t.v[0], ss_t.v[1], ss_t.v[2] );
				SSSCoefficients::FromCoefficients(
					m_rwParams.sigma_a, m_rwParams.sigma_s,
					m_rwParams.sigma_t );
				m_rwParams.g = g;
				m_rwParams.ior = ior.GetValuesAt( dummyRI ).v[0];
				m_rwParams.maxBounces = maxBounces;
			}

			/// \return The BSDF for this material.  NULL If there is no BSDF
			inline IBSDF* GetBSDF() const override {			return pBSDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const override {			return pSPF; };

			//! DL-471 (IMaterial::ConnectionScatterTypes): a subsurface material counts nothing at a connection endpoint (DL-482).
			inline unsigned int ConnectionScatterTypes() const override { return 0u; }

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const override {	return 0; };

			// SSS materials scatter light diffusely through the volume,
			// so straight-line camera connections through them are unphysical.
			inline bool CouldLightPassThrough() const override { return false; };

			/// Random-walk SSS handles subsurface transport volumetrically
			/// inside the mesh, but this is NOT the same as open-medium
			/// volumetric rendering.  Return false so the raycaster does
			/// not treat this as a participating medium.
			inline bool IsVolumetric() const override { return false; };

			/// No diffusion profile — random walk replaces disk projection.
			inline ISubSurfaceDiffusionProfile* GetDiffusionProfile() const override { return 0; };

			/// \return Random walk SSS parameters for the integrators.
			inline const RandomWalkSSSParams* GetRandomWalkSSSParams() const override { return &m_rwParams; };

			//! DL-374: the coefficients AND the boundary IOR at lambda.
			//! The walk's NM mode prices one wavelength, so it needs the
			//! absorption/scattering spectra evaluated there
			//! (IScalarPainter::GetValueAtNM, the same query the diffusion
			//! profiles' EvaluateProfileNM makes), broadcast to all three
			//! channels per IMaterial's contract.  Before DL-374 this
			//! returned the RGB snapshot unchanged and RandomWalkSSS
			//! collapsed it to its Rec.709 luminance -- one grey walk for
			//! every wavelength, so every spectral render of this material
			//! was achromatic.  Same flatten-to-constant snapshot point as
			//! the RGB parameters.
			bool GetRandomWalkSSSParamsNM( const Scalar nm, RandomWalkSSSParams& out ) const override
			{
				out = m_rwParams;
				const RayIntersectionGeometric ri = SnapshotRecord();
				const Scalar sa = pAbsorptionPainter->GetValueAtNM( ri, nm );
				const Scalar ss = pScatteringPainter->GetValueAtNM( ri, nm );
				out.sigma_a = RISEPel( sa, sa, sa );
				out.sigma_s = RISEPel( ss, ss, ss );
				SSSCoefficients::FromCoefficients( out.sigma_a, out.sigma_s, out.sigma_t );
				out.ior = pIORPainter->GetValueAtNM( ri, nm );
				return true;
			}

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const override
			{
				SpecularInfo info;
				info.isSpecular = (surfaceRoughness * surfaceRoughness <= 1e-6);
				info.canRefract = true;
				info.ior = pIORPainter->GetValuesAt( ri ).v[0];
				info.valid = true;
				return info;
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const override
			{
				SpecularInfo info;
				info.isSpecular = (surfaceRoughness * surfaceRoughness <= 1e-6);
				info.canRefract = true;
				info.ior = pIORPainter->GetValueAtNM( ri, nm );
				info.valid = true;
				return info;
			}

			//! Read-back + rebind for the interactive editor.  Only the
			//! IOR painter is rebindable.  Absorption/scattering were
			//! sampled-once into m_rwParams at construction time and
			//! stay frozen (changing them at runtime is out of scope).
			//! IOR rebinding hits BSDF, SPF, the cached pointer, AND
			//! re-flattens m_rwParams.ior so the random walk's Fresnel,
			//! refraction-into-medium, and SampleExit code use the new
			//! value coherently with the surface boundary.
			inline const IScalarPainter& GetIOR() const { return *pIORPainter; }
			inline void SetIOR( const IScalarPainter& v ) {
				if( !v.IsPositionIndependent() ) {
					GlobalLog()->PrintSourceError( "randomwalk_sss_material (DL-314): ior rebinding requires a position-independent painter; keeping the previous ior", __FILE__, __LINE__ );
					return;
				}
				v.addref();
				safe_release( pIORPainter );
				pIORPainter = &v;
				pBSDF->SetIOR( v );
				pSPF->SetIOR( v );

				// Re-flatten m_rwParams.ior from the NEW painter at the
				// same dummy intersection used in the constructor.  See
				// the ctor body above — the snapshot model is unchanged;
				// only the painter being snapshotted changes.
				const RayIntersectionGeometric dummyRI = SnapshotRecord();
				m_rwParams.ior = v.GetValuesAt( dummyRI ).v[0];
			}
		};
	}
}

#endif
