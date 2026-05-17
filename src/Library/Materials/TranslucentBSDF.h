//////////////////////////////////////////////////////////////////////
//
//  TranslucentBSDF.h - Defines a translucent BSDF 
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 27, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef TRANSLUCENT_BRDF_
#define TRANSLUCENT_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentBSDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~TranslucentBSDF();

			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pRefFront;			// Reflectance (color)
			const IPainter*			pTrans;				// Transmittance (color)
			const IScalarPainter*	pExponent;			// Phong exponent (physical scalar)

		public:
			TranslucentBSDF( const IPainter& rF, const IPainter& T, const IScalarPainter& exp );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back + rebind for the interactive editor.  Exponent
			//! getter is named `GetN` for symmetry with the SPF's `N`
			//! parameter and the scene-file slot name.
			inline const IPainter&       GetRefFront() const { return *pRefFront; }
			inline const IPainter&       GetTrans()    const { return *pTrans; }
			inline const IScalarPainter& GetN()        const { return *pExponent; }
			void SetRefFront( const IPainter& v );
			void SetTrans( const IPainter& v );
			void SetN( const IScalarPainter& v );
		};
	}
}

#endif

