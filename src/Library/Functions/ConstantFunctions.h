//////////////////////////////////////////////////////////////////////
//
//  ConstantFunctions.h - Contains contant value versions of the three
//    basic types of functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 29, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CONSTANT_FUNCTIONS_
#define CONSTANT_FUNCTIONS_

#include "../Interfaces/IFunction1D.h"
#include "../Interfaces/IFunction2D.h"
#include "../Interfaces/IFunction3D.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	namespace Implementation
	{
		class ConstantFunction1D : public virtual IFunction1D, public virtual Reference
		{
		protected:
			Scalar	val;
			virtual ~ConstantFunction1D(){};

		public:
			ConstantFunction1D( const Scalar val_ ) : val( val_ ) {};
			inline Scalar Evaluate( const Scalar ) const { return val; };
		};

		class ConstantFunction2D : public virtual IFunction2D, public virtual Reference
		{
		protected:
			Scalar	val;
			virtual ~ConstantFunction2D(){};

		public:
			ConstantFunction2D( const Scalar val_ ) : val( val_ ) {};
			inline Scalar Evaluate( const Scalar, const Scalar ) const { return val; };
		};

		class ConstantFunction3D : public virtual IFunction3D, public virtual Reference
		{
		protected:
			Scalar	val;
			virtual ~ConstantFunction3D(){};

		public:
			ConstantFunction3D( const Scalar val_ ) : val( val_ ) {};
			inline Scalar Evaluate( const Scalar, const Scalar, const Scalar ) const { return val; };
		};
	}
}

#endif

