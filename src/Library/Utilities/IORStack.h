//////////////////////////////////////////////////////////////////////
//
//  IORStack.h - Index of refraction stack
//
//  Tracks the chain of media the ray is currently inside, keyed by
//  the IObject* pointer at each push.  `containsCurrent()` uses this
//  key to decide entering vs. exiting when the ray hits a refractor.
//
//  Known limitation (object-pointer keying)
//
//    The object-pointer key works correctly for every natural scene
//    topology — single closed watertight meshes, nested DIFFERENT-
//    material volumes (glass sphere containing water sphere), two
//    disjoint objects of the same material, and concentric same-
//    material volumes.  See tests/IORStackBehaviorTest.cpp for the
//    exhaustive trace.
//
//    It is INCORRECT for the narrow case where:
//      (a) a single conceptual volume is modeled as multiple
//          IObject*s with the SAME material (the "slab-from-planes"
//          pattern, where two refractor planes stand in for a glass
//          slab of finite thickness), AND
//      (b) the ray continues on to hit another refractor after
//          traversing the slab.
//
//    In (a)+(b), the stack accumulates one entry per plane rather
//    than collapsing to a single medium, and the next refractor
//    reads the polluted top-of-stack as its outer IOR instead of
//    air.  The single-slab case (a) alone works optically because
//    Ni == Nt gives no refraction either way; the bug only surfaces
//    when (b) adds a downstream refractor whose outer IOR comes
//    from the stack.
//
//    Scene authors should avoid modeling a solid glass volume as
//    an open collection of interface planes — use a closed mesh or
//    a CSG object (CSGObject sets ri.pObject = the CSG wrapper, so
//    the stack remains clean).  The companion fix in
//    Optics::CalculateRefractedRay makes the SINGLE-SLAB version of
//    this topology render correctly regardless of where the stack
//    ends up (see sms_k1_botonly_ref test scene).
//
//    If this limitation needs to be lifted in the future, three
//    approaches were evaluated — see git log and
//    IORStackBehaviorTest.cpp:ScenarioE_SlabPollutionLeaksToNextRefractor
//    for the details.  Briefly: (1) switch to IMaterial*-pointer
//    keying (fixes slab case, breaks concentric same-material
//    volumes), (2) derive entering/exiting from the geometric sign
//    of dot(ray, normal) with stack fallback at grazing angles
//    (most robust but touches every refractor SPF), or (3) add a
//    scene-level medium_id tag to IMaterial (cleanest API but needs
//    a parser change).
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 25, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IOR_STACK_
#define IOR_STACK_

#include "../Interfaces/IReference.h"
#include "../Interfaces/IObject.h"
#include <stack>
#include <vector>

namespace RISE
{
	class IORStack
	{
	protected:

		struct IORDATA
		{
			const IObject* pObj;
			Scalar ior;

			IORDATA(
				const IObject* pObj_,
				Scalar ior_
				) : 
			pObj( pObj_ ),
			ior( ior_ )
			{}

			IORDATA( const IORDATA& that ) : 
			  pObj( that.pObj ),
			  ior( that.ior )
			{}
		};

		class MyIORStack : public std::stack< IORDATA, std::vector<IORDATA> >
		{
		public:

			MyIORStack(){};
			MyIORStack( const MyIORStack& s ) : 
			  std::stack< IORDATA, std::vector<IORDATA> >( s )
			{}

			~MyIORStack()
			{};

			bool find_and_destroy( const IObject* r )
			{
				std::vector<IORDATA>::reverse_iterator i, e;
				for( i=c.rbegin(), e=c.rend(); i!=e; i++ ) {
					if( i->pObj == r ) {
						c.erase( (i.base())-1 );
						return true;
					}
				}

				return false;
			}

			bool containsObject( const IObject* r ) const
			{
				std::vector<IORDATA>::const_reverse_iterator i, e;
				for( i=c.rbegin(), e=c.rend(); i!=e; i++ ) {
					if( i->pObj == r ) {
						return true;
					}
				}
				return false;
			}
		};

		MyIORStack iorstack;
		mutable const IObject* pCurrentObject;

	public:
		// Explicit to prevent implicit conversion from Scalar / integer
		// literal when a function expects `const IORStack&`.  A bare `0`
		// at such a call site used to construct IORStack(0), giving an
		// environment IOR of 0 and causing Ni=0 refraction errors.
		explicit IORStack( const Scalar ior ) :
		  pCurrentObject( 0 )
		{
			// An empty IOR stack always has the environment's IOR
			iorstack.push( IORDATA(0,ior) );
		}

		IORStack( const IORStack& s ) : 
		  iorstack( s.iorstack ),
		  pCurrentObject( s.pCurrentObject )
		{}

		~IORStack()
		{
		}

		// Ability to push stuff onto the stack
		inline void push( const Scalar ior )
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::push Asked to push item onto stack with no object" );
			} else {
				iorstack.push( IORDATA(pCurrentObject,ior) );
			}
		}

		// Ability to pop something off the stack
		inline void pop()
		{
			if( !pCurrentObject ) {
				GlobalLog()->PrintEasyWarning( "IORStack::pop Asked to pop item onto stack but no object" );
			} else {
				// Don't pop the default air entry in the IOR stack
				if( iorstack.size() > 1 ) {
					if( !iorstack.find_and_destroy( pCurrentObject ) ) {
						GlobalLog()->PrintEasyWarning( "IORStack::pop Failed to find object to pop IOR for" );
					}
				} else {
					GlobalLog()->PrintEasyWarning( "IORStack::pop Trying to pop IOR stack with only global IOR in it, cannot allow." );
				}
			}
		}

		// Returns the IOR of the top of the stack
		inline Scalar top() const
		{
			return iorstack.top().ior;
		}

		// Checks if the current object is already in the IOR stack.
		// Used as the authoritative "is the ray inside this object"
		// signal rather than relying on the surface normal sign, which
		// can be unreliable at grazing angles due to numerical precision.
		//
		// Known limitation: keys on the IObject* pointer, so a
		// conceptual volume modeled as MULTIPLE distinct objects of
		// the same material (slab-from-planes) is not recognized as a
		// single volume.  See the file-header comment for the full
		// scenario audit and tests/IORStackBehaviorTest.cpp for a
		// locked-in regression guard.
		inline bool containsCurrent() const
		{
			if( !pCurrentObject ) {
				return false;
			}
			return iorstack.containsObject( pCurrentObject );
		}

		// Returns the object at the top of the IOR stack (innermost enclosing object).
		// Returns 0 for the environment (root entry with no object).
		// Analogous to Cycles' volume stack top entry.
		inline const IObject* topObject() const
		{
			return iorstack.top().pObj;
		}

		// Sets the current object
		inline void SetCurrentObject( const IObject* pObj ) const
		{
			if( pObj ) {
				pCurrentObject = pObj;
			} else {
				GlobalLog()->PrintEasyWarning( "IORStack::SetCurrentObject Called with no object" );
			}
		}
	};
}

#endif


