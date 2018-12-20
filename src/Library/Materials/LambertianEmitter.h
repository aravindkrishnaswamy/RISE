//////////////////////////////////////////////////////////////////////
//
//  LambertianEmitter.h - Defines an emitter that 
//  emits light uniformly over its surface, and regardless of the 
//  incoming angle.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 21, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef LAMBERTIAN_EMITTER_
#define LAMBERTIAN_EMITTER_

#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class LambertianEmitter : public virtual IEmitter, public virtual Reference
		{
		protected:
			const IPainter&			radExPainter;
			const Scalar			scale;
			RISEPel					averageRadEx;
			VisibleSpectralPacket	averageSpectrum;

			virtual ~LambertianEmitter();

		public:
			LambertianEmitter( const IPainter& radEx, const Scalar scale_ );
			
			virtual RISEPel	emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const;
			virtual Scalar	emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const;
			virtual RISEPel	averageRadiantExitance() const;
			virtual Scalar	averageRadiantExitanceNM( const Scalar nm ) const;
			virtual Vector3 getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random )	const;
		};
	}
}

#endif
