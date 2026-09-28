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

		// DL-09: re-record the IOR of the innermost enclosing medium (the
		// top entry) as the walk moves through it.  Only meaningful for a
		// medium whose `ior` varies with position (GradedIndexMedium.h);
		// the root (environment) entry is never rewritten.
		inline void SetTopIOR( const Scalar ior )
		{
			if( iorstack.size() > 1 ) {
				iorstack.top().ior = ior;
			}
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

	//! Basic-radiance scale for one interface crossing, RADIANCE mode only.
	//!
	//! Radiance is NOT invariant across a smooth interface between media of
	//! different refractive index -- L / n^2 is (the "basic radiance";
	//! Preisendorfer 1965, Veach 1997 5.2, PBRT-v4 9.5.2).  A walk that
	//! gathers RADIANCE (anything rooted at a camera: the PT / BDPT-eye /
	//! VCM-eye / MLT-eye subpaths, the legacy shader-op chain, a final
	//! gather) must therefore multiply its throughput by
	//! (eta_before / eta_after)^2 every time the scattered ray's medium
	//! changes.  A walk that carries IMPORTANCE or FLUX (a BDPT/VCM light
	//! subpath, any photon tracer, an SMS photon seed, a detector-sphere
	//! measurement rig) gets NO factor: that asymmetry IS the non-symmetry
	//! of refractive scattering, and applying the factor on both sides
	//! would cancel it back out.
	//!
	//! `ScatteredRay::kray` / `krayNM` deliberately EXCLUDE this factor
	//! (see the contract on those fields in ISPF.h) -- ~60 SPF
	//! implementations would otherwise each need a TransportMode
	//! parameter.  It is applied at the CONSUMER instead, from the two
	//! stacks the consumer already holds.
	//!
	//! IMPORTANT: this reads ONLY `before.top()` and `after->top()` -- it
	//! does not, and cannot, consult whatever IOR value the SPF itself
	//! used for its own Fresnel/refraction calculation (a DielectricSPF's
	//! `rIndex`, `exitIOR`, or similar locals).  For every topology this
	//! file's header documents as CORRECT (single closed volumes, nested
	//! different-material volumes, concentric same-material volumes,
	//! disjoint same-material objects) with a spatially UNIFORM ior, the
	//! two agree, because `top()` after the SPF's own `push`/`pop` IS the
	//! medium it computed against.  Two known exceptions (review round 2,
	//! 2026-09-12 added the second):
	//!
	//! - **Overlapping solids.**  Under this file's documented
	//!   OVERLAPPING-SOLIDS pathology, they can silently disagree:
	//!   `pop()`'s `find_and_destroy` can remove an entry that is NOT at
	//!   the top (a slab-from-planes object hit downstream of another
	//!   refractor), leaving `top()` reading a medium the SPF never
	//!   actually refracted from or into. In that case this function can
	//!   return exactly 1 -- or the wrong ratio -- for a real medium
	//!   change DielectricSPF priced between two other indices.  Scenes
	//!   that avoid that pathology (see the file header's guidance) are
	//!   unaffected.
	//! - **Spatially varying `ior` -- DL-09, fixed 2026-09-28 by keeping
	//!   `before.top()` CURRENT, not by changing this function.**  Read
	//!   docs/DL09_GRADED_INDEX_INTERIOR_FACTOR.md before touching either.
	//!   Basic radiance `L / n^2` is conserved ALONG a ray inside a graded
	//!   medium as well as across an interface, so a camera(air) ->
	//!   A(`n_A`) -> B(`n_B`) -> air trip is
	//!   `(1/n_A)^2 * (n_A/n_B)^2 * (n_B/1)^2 = 1`.  Walks that support
	//!   graded media (PT, BDPT eye AND light, and so VCM and MLT) now call
	//!   `GradedIndexMedium::Advance` at every vertex inside a medium whose
	//!   `ior` is a world-position field: it pays the interior-segment
	//!   factor `(n_start/n_end)^2` and RE-RECORDS the stack top as the
	//!   local index.  At the exit hit this function therefore reads the
	//!   FRESH exit-point index as `before.top()` -- the "exit-read switch"
	//!   -- and it cannot happen without the interior factor, or vice versa.
	//!   Each half alone is one of the two failed rounds: substituting the
	//!   fresh exit value without the interior factor nets `(n_B/n_A)^2`
	//!   (2.25x brighter behind a passive 1.2 -> 1.8 slab, reverted
	//!   2026-09-14); the interior factor with a stale exit read nets
	//!   `(n_A/n_B)^2` (0.444).  `tests/RadianceEtaScaleGradedIndexTest.cpp`
	//!   pins the through-slab net at 1 (it catches both half-fixes) and
	//!   `tests/GradedIndexInteriorFactorTest.cpp` pins the interior gathers
	//!   that never reach this exit read.  A walk that does NOT Advance (the
	//!   legacy shader-op chain, the photon tracers, SMS chains) still sees
	//!   the entry-time value here -- the pre-DL-09 accounting, which nets
	//!   correctly on every completed through-trip (DL-292).
	//!
	//! @param before  the walk's current IOR stack at the scattering vertex
	//! @param after   the scattered ray's stack, or NULL when the SPF left
	//!                the stack unchanged (the common case: every non-
	//!                transmissive lobe).  A reflection lobe from inside a
	//!                dielectric allocates an unchanged COPY rather than
	//!                leaving this null, which is why the test below
	//!                compares the top IORs and does not just check for
	//!                non-null.
	//! @return        (eta_before / eta_after)^2 computed from
	//!                `before.top()` and `after->top()` (see above for
	//!                what that does and does not guarantee), or exactly
	//!                1 when the two agree or the medium did not change.
	inline Scalar RadianceEtaScale( const IORStack& before, const IORStack* after )
	{
		if( !after ) {
			return Scalar( 1 );
		}
		const Scalar etaBefore = before.top();
		const Scalar etaAfter  = after->top();
		// Equal IORs -> exact 1 with no division, which keeps the
		// overwhelmingly common reflection / same-index case bit-identical
		// to the pre-2026-09 behaviour.  Non-positive IORs cannot arise
		// from a well-formed stack (IORStack's ctor is explicit precisely
		// to stop a bare `0` becoming an environment IOR of 0), but a
		// scene can author `ior 0` on a dielectric, and a 0 here would
		// otherwise produce an infinite or zero throughput rather than a
		// dark-but-finite material.
		if( etaBefore == etaAfter || etaBefore <= Scalar( 0 ) || etaAfter <= Scalar( 0 ) ) {
			return Scalar( 1 );
		}
		const Scalar ratio = etaBefore / etaAfter;
		return ratio * ratio;
	}
}

#endif


