//////////////////////////////////////////////////////////////////////
//
//  PhongEmitter.h - Defines a material that 
//  emits light using the phong distribution over its surface
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 25, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PHONG_EMITTER_
#define PHONG_EMITTER_

#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PhongEmitter : public virtual IEmitter, public virtual Reference
		{
		protected:
			const IPainter&			radEx;
			const Scalar			scale;
			const IPainter&			phongN;
			RISEPel					averageRadEx;
			VisibleSpectralPacket	averageSpectrum;
			
			virtual ~PhongEmitter();

		public:
			PhongEmitter( const IPainter& radEx_, const Scalar scale_, const IPainter& N );

			virtual RISEPel	emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const;
			virtual Scalar	emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const;
			virtual RISEPel	averageRadiantExitance() const;
			virtual Scalar	averageRadiantExitanceNM( const Scalar nm ) const;
			virtual Vector3 getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random )	const;
		};
	}
}

#endif

