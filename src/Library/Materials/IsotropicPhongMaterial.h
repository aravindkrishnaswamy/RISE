//////////////////////////////////////////////////////////////////////
//
//  IsotropicMaterial.h - Defines a material that is an isotropic 
//  phong reflector 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISOTROPICPHONG_MATERIAL_
#define ISOTROPICPHONG_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "IsotropicPhongBRDF.h"
#include "IsotropicPhongSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class IsotropicPhongMaterial : public virtual IMaterial, public virtual Reference
		{
		protected:
			virtual ~IsotropicPhongMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

			IsotropicPhongBRDF*			pBRDF;
			IsotropicPhongSPF*			pSPF;

		public:
			IsotropicPhongMaterial( const IPainter& Rd_, const IPainter& Rs_, const IScalarPainter& exp )
			{
				pBRDF = new IsotropicPhongBRDF( Rd_, Rs_, exp );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new IsotropicPhongSPF( Rd_, Rs_, exp );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {		return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			//! Read-back + rebind for the interactive editor.  Material
			//! forwards to BOTH BRDF and SPF in lockstep so the two
			//! lobe instances never reference different painters.
			inline const IPainter&       GetRd()       const { return pBRDF->GetRd(); }
			inline const IPainter&       GetRs()       const { return pBRDF->GetRs(); }
			inline const IScalarPainter& GetExponent() const { return pBRDF->GetExponent(); }
			inline void SetRd( const IPainter& v )       { pBRDF->SetRd( v );       pSPF->SetRd( v ); }
			inline void SetRs( const IPainter& v )       { pBRDF->SetRs( v );       pSPF->SetRs( v ); }
			inline void SetExponent( const IScalarPainter& v ) { pBRDF->SetExponent( v ); pSPF->SetExponent( v ); }
		};
	}
}

#endif
