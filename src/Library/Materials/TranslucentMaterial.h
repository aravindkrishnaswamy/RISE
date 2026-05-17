//////////////////////////////////////////////////////////////////////
//
//  TranslucentMaterial.h - Defines a material that is partially
//  transparent (like a lampshade)
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef TRANSLUCENT_MATERIAL_
#define TRANSLUCENT_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "TranslucentBSDF.h"
#include "TranslucentSPF.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentMaterial :
			public virtual IMaterial, 
			public virtual Reference
		{
		protected:
			TranslucentBSDF*				pBRDF;
			TranslucentSPF*					pSPF;

			virtual ~TranslucentMaterial( )
			{
				safe_release( pBRDF );
				safe_release( pSPF );
			}

		public:
			TranslucentMaterial( const IPainter& rF, const IPainter& T, const IScalarPainter& ext, const IScalarPainter& N_, const IScalarPainter& scat )
			{
				pBRDF = new TranslucentBSDF( rF, T, N_ );
				GlobalLog()->PrintNew( pBRDF, __FILE__, __LINE__, "BRDF" );

				pSPF = new TranslucentSPF( rF, T, ext, N_, scat );
				GlobalLog()->PrintNew( pSPF, __FILE__, __LINE__, "SPF" );
			}

			/// \return The BRDF for this material.  NULL If there is no BRDF
			inline IBSDF* GetBSDF() const {			return pBRDF; };

			/// \return The SPF for this material.  NULL If there is no SPF
			inline ISPF* GetSPF() const {			return pSPF; };

			/// \return The emission properties for this material.  NULL If there is not an emitter
			inline IEmitter* GetEmitter() const {	return 0; };

			// Translucent materials scatter light diffusely through the
			// surface, so straight-line camera connections (t=0, t=1) through
			// them are unphysical.  Light transport through translucent objects
			// is handled correctly by the eye/light subpath tracing.
			inline bool CouldLightPassThrough() const { return false; };

			//! Read-back + rebind for the interactive editor.  `ref`/`tau`/`N`
			//! exist on both BSDF and SPF — Material forwards in lockstep.
			//! `ext`/`scat` exist only on the SPF (BSDF doesn't carry them).
			inline const IPainter&       GetRefFront()   const { return pSPF->GetRefFront(); }
			inline const IPainter&       GetTrans()      const { return pSPF->GetTrans(); }
			inline const IScalarPainter& GetExtinction() const { return pSPF->GetExtinction(); }
			inline const IScalarPainter& GetN()          const { return pSPF->GetN(); }
			inline const IScalarPainter& GetScat()       const { return pSPF->GetScat(); }
			inline void SetRefFront( const IPainter& v )         { pBRDF->SetRefFront( v ); pSPF->SetRefFront( v ); }
			inline void SetTrans( const IPainter& v )            { pBRDF->SetTrans( v );    pSPF->SetTrans( v ); }
			inline void SetExtinction( const IScalarPainter& v ) { pSPF->SetExtinction( v ); }
			inline void SetN( const IScalarPainter& v )          { pBRDF->SetN( v );        pSPF->SetN( v ); }
			inline void SetScat( const IScalarPainter& v )       { pSPF->SetScat( v ); }
		};
	}
}

#endif

