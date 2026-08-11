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
			OrenNayarMaterial(
				const IPainter& reflectance,
				const IScalarPainter& roughness
				)
			{
				pBRDF = new OrenNayarBRDF( reflectance, roughness );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new OrenNayarSPF( reflectance, roughness );
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
				const ScalarTriple roughness = pBRDF->GetRoughness().GetValuesAt(ri);
				return CreateOrenNayarContinuationClosurePel(
					ri,pBRDF->GetReflectance().GetColor(ri),
					RISEPel(roughness.v[0],roughness.v[1],roughness.v[2]),pathState);
			}

			const IContinuationClosureNM* MakeContinuationClosureNM(
				const RayIntersectionGeometric& ri,
				const IORStack&, const Scalar nm,
				const ContinuationPathState& pathState ) const override
			{
				return CreateOrenNayarContinuationClosureNM(
					ri,pBRDF->GetReflectance().GetColorNM(ri,nm),
					pBRDF->GetRoughness().GetValueAtNM(ri,nm),pathState);
			}

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to BOTH BRDF and SPF in lockstep.
			inline const IPainter&       GetReflectance() const { return pBRDF->GetReflectance(); }
			inline const IScalarPainter& GetRoughness()   const { return pBRDF->GetRoughness(); }
			inline void SetReflectance( const IPainter& v )       { pBRDF->SetReflectance( v ); pSPF->SetReflectance( v ); }
			inline void SetRoughness( const IScalarPainter& v )   { pBRDF->SetRoughness( v );   pSPF->SetRoughness( v ); }
		};
	}
}

#endif
