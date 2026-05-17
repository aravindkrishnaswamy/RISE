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
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class PhongEmitter : public virtual IEmitter, public virtual Reference
		{
		protected:
			//! Pointer storage so the interactive editor can rebind via
			//! Set*.  See LambertianBRDF for pattern + lifetime contract.
			const IPainter*			pRadEx;
			const Scalar			scale;
			const IScalarPainter*	pPhongN;
			RISEPel					averageRadEx;
			VisibleSpectralPacket	averageSpectrum;

			//! Re-sample the radEx painter into the cached averages.
			//! phongN doesn't affect the averages (they sample colour at
			//! random texture coords, not direction), so SetN does not
			//! refresh.
			void RefreshAverages();

			virtual ~PhongEmitter();

		public:
			PhongEmitter( const IPainter& radEx_, const Scalar scale_, const IScalarPainter& N );

			virtual RISEPel	emittedRadiance( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N) const;
			virtual Scalar	emittedRadianceNM( const RayIntersectionGeometric& ri, const Vector3& out, const Vector3& N, const Scalar nm ) const;
			virtual RISEPel	averageRadiantExitance() const;
			virtual Scalar	averageRadiantExitanceNM( const Scalar nm ) const;
			virtual Vector3 getEmmittedPhotonDir( const RayIntersectionGeometric& ri, const Point2& random )	const;

			//! Read-back + rebind for the interactive editor.  SetRadEx
			//! recomputes the cached averages; SetN does not.
			inline const IPainter&       GetRadEx() const { return *pRadEx; }
			inline const IScalarPainter& GetN()     const { return *pPhongN; }
			inline Scalar                GetScale() const { return scale; }
			void SetRadEx( const IPainter& v );
			void SetN( const IScalarPainter& v );
		};
	}
}

#endif

