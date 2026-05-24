//////////////////////////////////////////////////////////////////////
//
//  KeyframableHelper.h - Some utilities to help keyframable classes
//    go along their way.
//
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 25, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "../Interfaces/IKeyframable.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef KEYFRAMABLE_HELPER_
#define KEYFRAMABLE_HELPER_

namespace RISE
{
	namespace Implementation
	{
		template< class T >
		class Parameter : 
			public virtual IKeyframeParameter,
			public virtual Reference
		{
		protected:
			T	value;

		public:
			Parameter( T _value, unsigned int id ) :
			value( _value )
			{
				paramid = id;
			}

			virtual ~Parameter( )
			{
			}

			void Add( IKeyframeParameter& result, const IKeyframeParameter& a, const IKeyframeParameter& b )
			{
				T	res = *(T*)a.getValue() + *(T*)b.getValue();
				result.setValue( &res );
			}

			void Subtract( IKeyframeParameter& result, const IKeyframeParameter& a, const IKeyframeParameter& b )
			{
				T	res = *(T*)a.getValue() - *(T*)b.getValue();
				result.setValue( &res );
			}

			void ScalarMult( IKeyframeParameter& result, const IKeyframeParameter& a, const Scalar& t )
			{
				T	res = *(T*)a.getValue() * t;
				result.setValue( &res );
			}

			void* getValue( ) const
			{
				return (void*)&value;
			}

			void setValue( void* v )
			{
				T*	pValue = (T*)v;
				value = *pValue;
			}

			IKeyframeParameter* Clone( ) const
			{
				IKeyframeParameter* pClone = new Parameter<T>( value, paramid );
				GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "IKeyframeparameter clone" );
				return pClone;
			}
		};

		/////////////////////////////
		// Vector3 specializations
		/////////////////////////////

		template<>
		void Parameter<Vector3>::Add( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ );

		template<>
		void Parameter<Vector3>::Subtract( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ );

		/////////////////////////////
		// Point3 specializations
		/////////////////////////////

		template<>
		void Parameter<Point3>::Add( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ );

		template<>
		void Parameter<Point3>::Subtract( IKeyframeParameter& result, const IKeyframeParameter& a_, const IKeyframeParameter& b_ );

		template<>
		void Parameter<Point3>::ScalarMult( IKeyframeParameter& result, const IKeyframeParameter& a_, const Scalar& t );

		typedef Parameter<Point3> Point3Keyframe;
		typedef Parameter<Vector3> Vector3Keyframe;

		//! Strict scalar parse used by light KeyframeFromParameters.
		//! Returns true and fills `out` only if `s` parsed to a finite
		//! scalar (rejects "garbage", "nan", "inf").  Without this,
		//! the prior `atof(value.c_str())` silently mapped invalid
		//! input to 0 / NaN / ±inf and the keyframe machinery happily
		//! wrote it through to light state — the interactive editor
		//! then displayed a phantom-successful edit while the render
		//! either went dark (energy=0) or produced NaN pixels.  Caller
		//! should return null (i.e. reject the edit) on false.
		//!
		//! IMPORTANT: this code is compiled with `-ffast-math`, which
		//! permits the compiler to assume NaN / Inf never occur and
		//! optimise `std::isfinite` / `std::isnan` to constants — even
		//! a bit-pattern check after `memcpy` gets dead-code-eliminated
		//! because the optimiser tracks that `parsed` "must" be finite
		//! under fast-math.  Defence-in-depth is THREE layers:
		//!   1. Textual pre-check rejects "nan" / "inf" / "infinity"
		//!      (case-insensitive, with optional sign + whitespace)
		//!      before strtod even runs.  `strtod` is what would
		//!      produce the NaN/Inf for those inputs.
		//!   2. `volatile` on the parsed result blocks the optimiser
		//!      from re-deriving the value across the bit-pattern
		//!      check.
		//!   3. Bit-pattern check via memcpy as backup for cases like
		//!      "1e1000" that overflow to +Inf during strtod.
		//! Marked `inline` because on Windows
		//! `INLINE_TEMPLATE_SPECIALIZATIONS` re-includes the .cpp into
		//! every TU; the volatile/memcpy/textual layers above defeat
		//! `-ffast-math` from inside the body regardless of inlining.
		bool ParseStrictScalar( const String& s, Scalar& out );

		//! Strict 3-vector parse — tokenises on whitespace, requires
		//! exactly three components, each routed through
		//! ParseStrictScalar.  Use this in place of `sscanf("%lf %lf
		//! %lf", ...)` for editable vec3 fields (color, direction,
		//! target, position, orientation, scale): plain `sscanf`
		//! accepts "nan nan nan" and "inf 0 0" and writes the
		//! resulting non-finite values into light/transform state.
		//! Returns false (and leaves `out` untouched) on any parse
		//! failure or non-finite component.
		bool ParseStrictVec3( const String& s, double out[3] );
	}
}

#ifdef INLINE_TEMPLATE_SPECIALIZATIONS
#include "KeyframableHelper.cpp"
#endif

#endif

