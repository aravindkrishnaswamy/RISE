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
#include "../Interfaces/IScalarPainter.h"
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
			//! Physical-scalar Beer-Lambert coefficient -- see the member
			//! comment on `CompositeSPF::extinction` for why this is an
			//! `IScalarPainter` and not an `IPainter`.  It matters twice here:
			//! `emittedRadianceNM` used to read it through the JH albedo
			//! uplift (saturating every value above ~1) while the constructor's
			//! average derived from the UNSATURATED `GetColor`, so the emitter
			//! disagreed with itself between its average and its per-hit
			//! radiance.  One scalar source now feeds both.
			const IScalarPainter&	extinction;
			const Scalar			thickness;

			RISEPel					averageRadEx;
			VisibleSpectralPacket	averageSpectrum;

			virtual ~CompositeEmitter();

		public:
			CompositeEmitter(
				const IEmitter& top_,
				const IEmitter& bottom_,
				const IScalarPainter& extinction_,
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
