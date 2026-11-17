//////////////////////////////////////////////////////////////////////
//
//  ModifierStack.h - Composition of ray-intersection modifiers.
//
//  `Object::pModifier` is a single pointer (design docs/
//  RELIEF_MODIFIER_DESIGN.md section 2, "One modifier per object" --
//  since this class exists, "composition via modifier_stack").  A stack
//  is itself an IRayIntersectionModifier that holds an ORDERED list of
//  other modifiers and applies them in authored order, each one seeing
//  the PREVIOUS one's vNormal/onb -- so a stack of N modifiers behaves
//  exactly as if the object's single modifier slot held a hand-written
//  chain of N `Modify` calls.
//
//  ORDER SEMANTICS (docs/RELIEF_MODIFIER_DESIGN.md section 4), tested in
//  tests/ReliefModifierTest.cpp test 9:
//
//    normal_map -> relief   relief perturbs the NORMAL-MAPPED frame: fine
//                           procedural detail on top of a baked map --
//                           the usual case.
//    relief -> normal_map   the map is decoded in the RELIEF-TILTED
//                           frame; rarely wanted.
//    ... -> glint           glint should always be LAST: it replaces the
//                           normal with a facet normal drawn about the
//                           CURRENT one, so it must see the final smooth
//                           frame, not have something else perturb its
//                           facet normal afterward.
//
//  `relief` twice with different height fields is legitimate (a
//  coarse+fine two-frequency split) and is exactly how that split is
//  expressed when both terms are shading-only.
//
//  NESTING is allowed -- a stack may contain another stack -- and is
//  algebraically flat: `stack{A, stack{B,C}}` applies A, B, C in that
//  order, identically to `stack{A,B,C}` (tests/ReliefModifierTest.cpp
//  test 9c).  SELF-REFERENCE IS IMPOSSIBLE: modifier names resolve
//  through the modifier manager at PARSE time (Job::AddModifierStack
//  resolves every member name before the stack object is constructed),
//  so a stack cannot name itself or a not-yet-registered later stack --
//  there is no cycle to detect at runtime.
//
//  An EMPTY stack is a parse-time error (RISE_API_CreateModifierStack
//  rejects count == 0), matching glint_modifier's stance that an
//  authored no-op chunk is a mistake, not an intent.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 6, 2026
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef MODIFIER_STACK_
#define MODIFIER_STACK_

#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Utilities/Reference.h"

#include <vector>

namespace RISE
{
	namespace Implementation
	{
		class ModifierStack :
			public virtual IRayIntersectionModifier,
			public virtual Reference
		{
		protected:
			virtual ~ModifierStack();

			std::vector<const IRayIntersectionModifier*>	members;	///< Applied in this order; each addref'd in the ctor, released in the dtor

		public:
			//! `members_` must be non-empty and contain no null entries --
			//! enforced by RISE_API_CreateModifierStack, not here (this ctor
			//! trusts its caller, mirroring NormalMap/ReliefModifier).
			ModifierStack(
				const IRayIntersectionModifier* const* members_,	///< [in] The member modifiers, in authored (application) order (each addref'd)
				const unsigned int count							///< [in] Number of members; > 0
				);

			void Modify( RayIntersectionGeometric& ri ) const;

			//! Structural introspection (CstDeriveGoldenTest's DumpJob
			//! composition digest): the ordered member list.
			unsigned int MemberCount() const { return (unsigned int)members.size(); }
			const IRayIntersectionModifier* Member( unsigned int i ) const { return members[i]; }
		};
	}
}

#endif
