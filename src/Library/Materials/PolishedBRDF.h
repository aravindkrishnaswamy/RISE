//////////////////////////////////////////////////////////////////////
//
//  PolishedBRDF.h - Defines the BRDF for a polished material
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  WARNING!!!  This is experimental and unverified code
//             from ggLibrary, DO NOT USE!
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POLISHED_BRDF_
#define POLISHED_BRDF_

#include "../Interfaces/IFunction1D.h"
#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PolishedBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			const IPainter&				pReflectance;
			const IPainter&				phongN;
			const IPainter&				Nt;

			virtual ~PolishedBRDF();

		public:
			PolishedBRDF( const IPainter& reflectance, const IPainter& phongN_, const IPainter& Nt_ );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
		};
	}
}

#endif

