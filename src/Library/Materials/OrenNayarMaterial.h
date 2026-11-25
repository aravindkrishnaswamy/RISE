//////////////////////////////////////////////////////////////////////
//
//  OrenNayarMaterial.h - 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 12, 2004
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ORENNAYAR_MATERIAL_
#define ORENNAYAR_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "OrenNayarBRDF.h"
#include "OrenNayarSPF.h"
#include "../Interfaces/IContinuationClosure.h"

namespace RISE
{
	namespace Implementation
	{
		class OrenNayarMaterial : 
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			OrenNayarBRDF*				pBRDF;
			OrenNayarSPF*				pSPF;

			virtual ~OrenNayarMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			//! `energy_preserving` picks the model for the BRDF and SPF
			//! together: false the classic Oren-Nayar, true EON (see
			//! OrenNayarBRDF::EONValue).
			OrenNayarMaterial(
				const IPainter& reflectance,
				const IScalarPainter& roughness,
				const bool energy_preserving = false
				)
			{
				pBRDF = new OrenNayarBRDF( reflectance, roughness, energy_preserving );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new OrenNayarSPF( reflectance, roughness, energy_preserving );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const override {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const override {			return pSPF; };

			//! DL-471: every lobe is the diffuse one (IMaterial::ConnectionScatterTypes).
			inline unsigned int ConnectionScatterTypes() const override { return 1u << ScatteredRay::eRayDiffuse; }


			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const override {	return 0; };

			const IContinuationClosurePel* MakeContinuationClosurePel(
				const RayIntersectionGeometric& ri,
				const IORStack&,
				const ContinuationPathState& pathState ) const override
			{
				const ScalarTriple roughness = pBRDF->GetRoughness().GetValuesAt(ri);
				return CreateOrenNayarContinuationClosurePel(
					ri,ReflectanceColor(pBRDF->GetReflectance(),ri),
					RISEPel(roughness.v[0],roughness.v[1],roughness.v[2]),pathState,
					pBRDF->IsEnergyPreserving());
			}

			const IContinuationClosureNM* MakeContinuationClosureNM(
				const RayIntersectionGeometric& ri,
				const IORStack&, const Scalar nm,
				const ContinuationPathState& pathState ) const override
			{
				return CreateOrenNayarContinuationClosureNM(
					ri,ReflectanceColorNM(pBRDF->GetReflectance(),ri,nm),
					pBRDF->GetRoughness().GetValueAtNM(ri,nm),pathState,
					pBRDF->IsEnergyPreserving());
			}

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to BOTH BRDF and SPF in lockstep.
			inline const IPainter&       GetReflectance() const { return pBRDF->GetReflectance(); }
			inline const IScalarPainter& GetRoughness()   const { return pBRDF->GetRoughness(); }
			inline bool IsEnergyPreserving()               const { return pBRDF->IsEnergyPreserving(); }
			inline void SetReflectance( const IPainter& v )       { pBRDF->SetReflectance( v ); pSPF->SetReflectance( v ); }
			inline void SetRoughness( const IScalarPainter& v )   { pBRDF->SetRoughness( v );   pSPF->SetRoughness( v ); }

			unsigned int GetPathDependence() const override
			{
				return IMaterial::GetPathDependence() |
					pBRDF->GetReflectance().GetPathDependence() |
					pBRDF->GetRoughness().GetPathDependence();
			}
		};
	}
}

#endif
