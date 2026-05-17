//////////////////////////////////////////////////////////////////////
//
//  LambertianBRDF.h - Defines a lambertian BRDF, which just 
//  reflects light equally in all directions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 06, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////
	
#ifndef LAMBERTIAN_BRDF_
#define LAMBERTIAN_BRDF_

#include "../Interfaces/IBSDF.h"
#include "../Interfaces/IPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class LambertianBRDF : public virtual IBSDF, public virtual Reference
		{
		protected:
			virtual ~LambertianBRDF();

			//! Pointer (not reference) so the interactive editor can
			//! rebind via `SetReflectance` — see Phase 4 in
			//! MaterialIntrospection.h.  Lifetime: owned via addref
			//! in the constructor and SetReflectance; released in
			//! the destructor and on each rebind.  Never null after
			//! construction.
			const IPainter*	pReflectance;

		public:
			LambertianBRDF( const IPainter& reflectance );

			virtual RISEPel value( const Vector3& vLightIn, const RayIntersectionGeometric& ri ) const;
			virtual Scalar valueNM( const Vector3& vLightIn, const RayIntersectionGeometric& ri, const Scalar nm ) const;
			virtual RISEPel albedo( const RayIntersectionGeometric& ri ) const;

			//! Read-back accessor for the interactive editor's
			//! `MaterialIntrospection` — reverse-lookup the painter's
			//! registered name via the IPainterManager.
			inline const IPainter& GetReflectance() const { return *pReflectance; }

			//! Rebind the reflectance painter.  Releases the prior
			//! painter and addrefs the new one.  Caller (typically
			//! `MaterialIntrospection::SetSlot` via SceneEditor) is
			//! responsible for the cancel-and-park gate against the
			//! render thread — the pointer swap itself is a single
			//! word write, but the prior painter's `release()` could
			//! free a painter a worker is mid-sample on.
			void SetReflectance( const IPainter& reflectance );
		};
	}
}

#endif

