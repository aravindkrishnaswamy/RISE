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
	}
}

#ifdef INLINE_TEMPLATE_SPECIALIZATIONS
#include "KeyframableHelper.cpp"
#endif

#endif

