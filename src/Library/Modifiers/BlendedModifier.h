//////////////////////////////////////////////////////////////////////
//
//  BlendedModifier.h - Weighted blend of two ray-intersection modifiers.
//
//  Evaluates two child modifiers (each optional; null = unperturbed
//  shading normal) independently against the PRE-modifier hit frame,
//  blends their resulting shading normals linearly by a scalar painter
//  weight t in [0, 1] (matching Cycles' ShaderNodeMix on normal vectors),
//  normalizes, and rebuilds the shading ONB while preserving coherent
//  tangents and mirrored-instance handedness via `ModifierFrame`.
//
//  When t <= 0 or t >= 1, only the selected child runs (zero wasted work).
//  The blend weight is always evaluated on the pre-modifier hit record.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BLENDED_MODIFIER_
#define BLENDED_MODIFIER_

#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class BlendedModifier :
			public virtual IRayIntersectionModifier,
			public virtual Reference
		{
		protected:
			virtual ~BlendedModifier();

			const IRayIntersectionModifier*	pModA;		///< Nullable (null = unperturbed surface normal)
			const IRayIntersectionModifier*	pModB;		///< Nullable (null = unperturbed surface normal)
			const IScalarPainter*			pWeight;	///< Required non-null blend factor in [0, 1] (0=A, 1=B)

		public:
			BlendedModifier(
				const IRayIntersectionModifier* pModA_,
				const IRayIntersectionModifier* pModB_,
				const IScalarPainter* pWeight_
				);

			void Modify( RayIntersectionGeometric& ri ) const override;
			unsigned int GetPathDependence() const override
			{
				return ( pModA ? pModA->GetPathDependence() : 0u ) |
				       ( pModB ? pModB->GetPathDependence() : 0u ) |
				       ( pWeight ? pWeight->GetPathDependence() : 0u );
			}

			const IRayIntersectionModifier* ModA() const { return pModA; }
			const IRayIntersectionModifier* ModB() const { return pModB; }
			const IScalarPainter* Weight() const { return pWeight; }
		};
	}
}

#endif
