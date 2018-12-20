//////////////////////////////////////////////////////////////////////
//
//  IFunction2D.h - Contains an interface to a two dimensional 
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

#ifndef IFUNCTION_2D_
#define IFUNCTION_2D_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"
#include <functional>

namespace RISE
{
	//! A two dimensional function, takes two scalar values, returns the result of the function
	/// \sa IFunction1D
	/// \sa IFunction3D
	class IFunction2D : 
		public virtual IReference, 
		public std::binary_function<const Scalar,const Scalar,Scalar>
	{
	protected:
		IFunction2D( ){};
		virtual ~IFunction2D( ){};

	public:
		//! Evalues the function
		/// \return Scalar result of the function
		virtual Scalar Evaluate( 
			const Scalar x,						///< [in] X value to evaluate the function at
			const Scalar y						///< [in] Y value to evaluate the function at
			) const = 0;

		//! Evalues the function
		/// \return Scalar result of the function
		inline Scalar operator()( 
			const Scalar x,						///< [in] X value to evaluate the function at
			const Scalar y						///< [in] Y value to evaluate the function at
			){ return Evaluate( x, y ); }
	};
}

#endif
