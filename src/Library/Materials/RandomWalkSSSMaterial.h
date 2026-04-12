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
			const IPainter&					iorPainter;
			const Scalar					surfaceRoughness;

			virtual ~RandomWalkSSSMaterial()
			{
				safe_release( pBSDF );
				safe_release( pSPF );
				iorPainter.release();
			}

		public:
			RandomWalkSSSMaterial(
				const IPainter& ior,
				const IPainter& absorption,
				const IPainter& scattering,
				const Scalar g,
				const Scalar roughness,
				const unsigned int maxBounces
				) :
			iorPainter( ior ),
			surfaceRoughness( roughness )
			{
				iorPainter.addref();

				pBSDF = new SubSurfaceScatteringBSDF( ior, absorption, scattering, g, roughness );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				pSPF = new SubSurfaceScatteringSPF( ior, absorption, scattering, g, roughness, true );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );

				// Evaluate painters at a dummy intersection to extract
				// scalar coefficients for the random walk.
				//
				// LIMITATION: textured or procedural painters are
				// sampled once here and flattened to constant values.
				// Spatially-varying absorption/scattering and
				// wavelength-dependent IOR for the subsurface walk
				// are not currently supported.  Supporting them would
				// require passing painter references through to the
				// walk algorithm and evaluating per-intersection.
				RayIntersectionGeometric dummyRI(
					Ray( Point3(0,0,0), Vector3(0,1,0) ),
					nullRasterizerState );
				dummyRI.bHit = true;
				dummyRI.ptIntersection = Point3( 0, 0, 0 );
				dummyRI.vNormal = Vector3( 0, 1, 0 );
				dummyRI.onb.CreateFromW( dummyRI.vNormal );

				m_rwParams.sigma_a = absorption.GetColor( dummyRI );
				m_rwParams.sigma_s = scattering.GetColor( dummyRI );
				SSSCoefficients::FromCoefficients(
					m_rwParams.sigma_a, m_rwParams.sigma_s,
					m_rwParams.sigma_t );
				m_rwParams.g = g;
				m_rwParams.ior = ior.GetColor( dummyRI )[0];
				m_rwParams.maxBounces = maxBounces;
			}

			/// \return The BSDF for this material.  NULL If there is no BSDF
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			// SSS materials scatter light diffusely through the volume,
			// so straight-line camera connections through them are unphysical.
			inline bool CouldLightPassThrough() const { return false; };

			/// Random-walk SSS handles subsurface transport volumetrically
			/// inside the mesh, but this is NOT the same as open-medium
			/// volumetric rendering.  Return false so the raycaster does
			/// not treat this as a participating medium.
			inline bool IsVolumetric() const { return false; };

			/// No diffusion profile — random walk replaces disk projection.
			inline ISubSurfaceDiffusionProfile* GetDiffusionProfile() const { return 0; };

			/// \return Random walk SSS parameters for the integrators.
			inline const RandomWalkSSSParams* GetRandomWalkSSSParams() const { return &m_rwParams; };

			SpecularInfo GetSpecularInfo(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack
				) const
			{
				SpecularInfo info;
				info.isSpecular = (surfaceRoughness * surfaceRoughness <= 1e-6);
				info.canRefract = true;
				info.ior = iorPainter.GetColor( ri )[0];
				info.valid = true;
				return info;
			}

			SpecularInfo GetSpecularInfoNM(
				const RayIntersectionGeometric& ri,
				const IORStack& ior_stack,
				const Scalar nm
				) const
			{
				SpecularInfo info;
				info.isSpecular = (surfaceRoughness * surfaceRoughness <= 1e-6);
				info.canRefract = true;
				info.ior = iorPainter.GetColorNM( ri, nm );
				info.valid = true;
				return info;
			}
		};
	}
}

#endif
