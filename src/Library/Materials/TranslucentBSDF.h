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
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class TranslucentBSDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~TranslucentBSDF();

			const IPainter&	pRefFront;			// Reflectance
			const IPainter&	pTrans;				// Transmittance
			const IPainter& exponent;			// The exponent

		public:
			TranslucentBSDF( const IPainter& rF, const IPainter& T, const IPainter& exp );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

