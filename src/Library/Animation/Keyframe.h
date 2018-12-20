//////////////////////////////////////////////////////////////////////
//
//  Keyframe.h - Implementation of the keyframe class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 26, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef KEYFRAME_
#define KEYFRAME_

#include "../Interfaces/IKeyframable.h"
#include "../Interfaces/IFullInterpolator.h"
#include <map>

namespace RISE
{
	namespace Implementation
	{
		class KeyframeParameterDispatch
		{
		protected:
			IKeyframeParameter& param;

		public:
			/*
			KeyframeParameterDispatch(
				IKeyframeParameter& p,
				const bool turf_for_me = false
				) : 
			param( *(p.Clone()) )
			{
				if( turf_for_me ) {
					p.release();
				}
			}
			*/

			KeyframeParameterDispatch(
				IKeyframeParameter& p,
				const bool stealreference = false
				) : 
			param( p )
			{
				if( !stealreference ) {
					param.addref();
				}
			}

			// Copy constructor
			KeyframeParameterDispatch(
				const KeyframeParameterDispatch& copy
				) : 
			param( *(copy.param.Clone()) )
			{
			}

			virtual ~KeyframeParameterDispatch()
			{
				param.release();
			}

			// Scalar Multiplication (*) 
			inline friend KeyframeParameterDispatch operator*( const KeyframeParameterDispatch &v, const Scalar t )  
			{
				IKeyframeParameter* kfp = v.param.Clone();
				kfp->ScalarMult( *kfp, v.param, t );
				return KeyframeParameterDispatch( *kfp, true );
			}

			// Scalar Multiplication (*)
			inline friend KeyframeParameterDispatch operator*( const Scalar t, const KeyframeParameterDispatch &v )  
			{ 
				IKeyframeParameter* kfp = v.param.Clone();
				kfp->ScalarMult( *kfp, v.param, t );
				return KeyframeParameterDispatch( *kfp, true );
			}

			// Addition (+)
			inline friend KeyframeParameterDispatch operator+( const KeyframeParameterDispatch &a, const KeyframeParameterDispatch &b )  
			{
				IKeyframeParameter* kfp = a.param.Clone();
				kfp->Add( *kfp, a.param, b.param );
				return KeyframeParameterDispatch( *kfp, true );
			}

			// Subtraction (-)
			inline friend KeyframeParameterDispatch operator-( const KeyframeParameterDispatch &a, const KeyframeParameterDispatch &b )  
			{
				IKeyframeParameter* kfp = a.param.Clone();
				kfp->Subtract( *kfp, a.param, b.param );
				return KeyframeParameterDispatch( *kfp, true );
			}

			inline KeyframeParameterDispatch& operator=( const KeyframeParameterDispatch& v )  
			{
				param.release();
				param = *(v.param.Clone());
				return *this;  // Assignment operator returns left side.
			}

			inline IKeyframeParameter& GetParam()
			{
				return param;
			}
		};

		class Keyframe
		{
		public:
			IKeyframeParameter& param;
			const Scalar time;
			const IFullInterpolator<KeyframeParameterDispatch>* pInterp;

			Keyframe( 
				IKeyframeParameter& p,
				const Scalar t,
				const IFullInterpolator<KeyframeParameterDispatch>* interp
				) : 
			param( p ),
			time( t ), 
			pInterp( interp )
			{
				param.addref();

				if( pInterp ) {
					pInterp->addref();
				}
			}

			virtual ~Keyframe()
			{
				param.release();
				safe_release( pInterp );
			}

			static inline bool KeyframeCompare( Keyframe* lhs, Keyframe* rhs )
			{
				return lhs->time < rhs->time;
			}
		};
	}
}

#endif
