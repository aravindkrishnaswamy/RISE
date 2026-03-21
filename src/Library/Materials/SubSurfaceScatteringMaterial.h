//////////////////////////////////////////////////////////////////////
//
//  SubSurfaceScatteringMaterial.h - Defines a material that
//  simulates subsurface scattering via a volumetric random walk
//  approximation.  Provides both SPF (for path tracing) and
//  BSDF (for direct lighting evaluation in BDPT).
//
//  Parameters:
//    ior     - Index of refraction at the surface boundary
//    sigma_a - Absorption coefficient (per unit distance, per channel)
//    sigma_s - Scattering coefficient (per unit distance, per channel)
//    g       - Henyey-Greenstein asymmetry parameter (-1 to 1)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SUBSURFACE_SCATTERING_MATERIAL_
#define SUBSURFACE_SCATTERING_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "SubSurfaceScatteringBSDF.h"
#include "SubSurfaceScatteringSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class SubSurfaceScatteringMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		protected:
			SubSurfaceScatteringBSDF*		pBSDF;
			SubSurfaceScatteringSPF*		pSPF;

			virtual ~SubSurfaceScatteringMaterial()
			{
				safe_release( pBSDF );
				safe_release( pSPF );
			}

		public:
			SubSurfaceScatteringMaterial(
				const IPainter& ior,
				const IPainter& absorption,
				const IPainter& scattering,
				const Scalar g,
				const Scalar roughness
				)
			{
				pBSDF = new SubSurfaceScatteringBSDF( ior, absorption, scattering, g, roughness );
				GlobalLog()->PrintNew( pBSDF, __FILE__, __LINE__, "BSDF" );

				pSPF = new SubSurfaceScatteringSPF( ior, absorption, scattering, g, roughness );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BSDF for this material.  NULL If there is no BSDF
			inline IBSDF* GetBSDF() const {			return pBSDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };
		};
	}
}

#endif
