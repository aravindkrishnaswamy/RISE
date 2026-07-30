//////////////////////////////////////////////////////////////////////
//
//  LambertianMaterial.h - Defines a material that is a perfect
//  lambertian reflector (ie, light is reflected in all directions
//  with a cosine distribution and with the same intensity)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LAMBERTIAN_MATERIAL_
#define LAMBERTIAN_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/ILog.h"
#include "LambertianBRDF.h"
#include "LambertianSPF.h"
#include "../Interfaces/IContinuationClosure.h"

namespace RISE
{
	namespace Implementation
	{
		class LambertianMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			LambertianBRDF*				pBRDF;
			LambertianSPF*				pSPF;

			virtual ~LambertianMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			LambertianMaterial( const IPainter& ref )
			{
				pBRDF = new LambertianBRDF( ref );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new LambertianSPF( ref );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const override {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const override {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const override {	return 0; };

			const IContinuationClosurePel* MakeContinuationClosurePel(
				const RayIntersectionGeometric& ri,
				const IORStack&,
				const ContinuationPathState& pathState ) const override
			{
				return CreateLambertianContinuationClosurePel(
					ri,pBRDF->GetReflectance().GetColor(ri),pathState);
			}

			const IContinuationClosureNM* MakeContinuationClosureNM(
				const RayIntersectionGeometric& ri,
				const IORStack&, const Scalar nm,
				const ContinuationPathState& pathState ) const override
			{
				return CreateLambertianContinuationClosureNM(
					ri,pBRDF->GetReflectance().GetColorNM(ri,nm),pathState);
			}

			//! Read-back accessor for the interactive editor's
			//! `MaterialIntrospection`.  Returns the painter the BRDF
			//! was constructed with — reverse-lookup against the
			//! IPainterManager gives the user-visible name.
			inline const IPainter& GetReflectance() const { return pBRDF->GetReflectance(); }

			//! Rebind the reflectance painter on BOTH the BRDF and
			//! the SPF in lockstep — they MUST point at the same
			//! painter instance for the rasterizer's BRDF-shaded and
			//! SPF-scattered paths to agree.  Caller is responsible
			//! for the cancel-and-park gate against the render
			//! thread; see `MaterialIntrospection::SetSlot` and the
			//! controller's Material SetProperty branch.
			inline void SetReflectance( const IPainter& reflectance ) {
				pBRDF->SetReflectance( reflectance );
				pSPF->SetReflectance( reflectance );
			}
		};
	}
}

#endif
