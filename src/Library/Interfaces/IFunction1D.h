//////////////////////////////////////////////////////////////////////
//
//  IFunction1D.h - Contains an interface to a one dimensional 
//                 function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef IFUNCTION1D_
#define IFUNCTION1D_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"
#include <functional>

namespace RISE
{
	//! A one dimensional function, takes a scalar value, returns the result of the function
	/// \sa IFunction2D
	/// \sa IFunction3D
	class IFunction1D : 
		public virtual IReference, 
		public std::unary_function<const Scalar,Scalar>
	{
	protected:
		IFunction1D( ){};
		virtual ~IFunction1D( ){};

	public:
		//! Evalues the function
		/// \return Scalar result of the function
		virtual Scalar Evaluate( 
			const Scalar variable						///< [in] The point to evaluate the function at
			) const = 0;

		//! Evalues the function
		/// \return Scalar result of the function
		Scalar operator()( 
			const Scalar v								///< [in] The point to evaluate the function at
			){ return Evaluate( v ); }
	};
}

#endif
