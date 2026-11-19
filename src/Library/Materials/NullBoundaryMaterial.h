//////////////////////////////////////////////////////////////////////
//
//  NullBoundaryMaterial.h - Unit, deterministic medium-enclosure
//    boundary with no optical or shading response.
//
//  This is deliberately distinct from NullMaterial (the terminating
//  "none" sentinel).  Transport recognizes only this exact dynamic type;
//  derived classes are ordinary unsupported materials.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef NULL_BOUNDARY_MATERIAL_
#define NULL_BOUNDARY_MATERIAL_

#include "../Interfaces/IMaterial.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Ray.h"

namespace RISE
{
	class IObject;
	class IORStack;

	namespace Implementation
	{
		class NullBoundaryMaterial :
			public virtual IMaterial,
			public virtual Reference
		{
		public:
			IBSDF* GetBSDF() const override;
			ISPF* GetSPF() const override;
			IEmitter* GetEmitter() const override;
		};

		//! Exact-type predicate shared by every transport surface.  A
		//! dynamic_cast is intentionally insufficient: subclasses do not inherit
		//! null-boundary transport semantics.
		bool IsExactNullBoundaryMaterial( const IMaterial* pMaterial );

		//! Apply the enclosure-only entering/exiting transition when pMaterial
		//! has the exact null-boundary dynamic type.  Returns true when handled.
		bool ApplyExactNullBoundaryTransition(
			const IMaterial* pMaterial,
			const IObject* pObject,
			IORStack& stack );

		//! Continue at the exact geometric boundary while preserving the ray's
		//! parameterization and free-propagating any screen-space differentials.
		Ray ContinueExactNullBoundaryRay( const Ray& ray, Scalar distance );
	}
}

#endif
