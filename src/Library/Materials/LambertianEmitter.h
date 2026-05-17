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
			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pRadEx;
			const Scalar			scale;
			RISEPel					averageRadEx;
			VisibleSpectralPacket	averageSpectrum;

			//! Re-sample the radEx painter into `averageRadEx` /
			//! `averageSpectrum` over 100 random texture coords.
			//! Shared between the constructor and SetRadEx.
			void RefreshAverages();

			virtual ~LambertianEmitter();

		public:
			LambertianEmitter( const IPainter& radEx, const Scalar scale_ );

			virtual RISEPel	emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const;
			virtual Scalar	emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const;
			virtual RISEPel	averageRadiantExitance() const;
			virtual Scalar	averageRadiantExitanceNM( const Scalar nm ) const;
			virtual Vector3 getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random )	const;

			//! Read-back + rebind for the interactive editor.  SetRadEx
			//! rebinds the painter AND recomputes the cached averages
			//! (used by light-source importance sampling).  Stale
			//! averages would not be a correctness bug — just a sampling-
			//! efficiency one — but the recompute is cheap.
			inline const IPainter& GetRadEx() const { return *pRadEx; }
			inline Scalar          GetScale() const { return scale; }
			void SetRadEx( const IPainter& v );
		};
	}
}

#endif
