//////////////////////////////////////////////////////////////////////
//
//  CompositeEmitter.h - Defines an emitter that composites
//  emission from two layers, attenuating the bottom layer's
//  emission through Beer's law absorption
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 6, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COMPOSITE_EMITTER_
#define COMPOSITE_EMITTER_

#include "../Interfaces/IEmitter.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class CompositeEmitter : public virtual IEmitter, public virtual Reference
		{
		protected:
			const IEmitter&			topEmitter;
			const IEmitter&			bottomEmitter;
			const IPainter&			extinction;
			const Scalar			thickness;

			RISEPel					averageRadEx;
			VisibleSpectralPacket	averageSpectrum;

			virtual ~CompositeEmitter();

		public:
			CompositeEmitter(
				const IEmitter& top_,
				const IEmitter& bottom_,
				const IPainter& extinction_,
				const Scalar thickness_
				);

			virtual RISEPel	emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N ) const;
			virtual Scalar	emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const;
			virtual RISEPel	averageRadiantExitance() const;
			virtual Scalar	averageRadiantExitanceNM( const Scalar nm ) const;
			virtual Vector3 getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random ) const;
		};
	}
}

#endif
