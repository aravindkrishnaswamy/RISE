//////////////////////////////////////////////////////////////////////
//
//  IFunction3D.h - Contains an interface to a three dimensional 
//                 function
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 11, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IFUNCTION_3D_
#define IFUNCTION_3D_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	//! A three dimensional function, takes three scalar values, returns the result of the function
	/// \sa IFunction1D
	/// \sa IFunction2D
	class IFunction3D : public virtual IReference
	{
	protected:
		IFunction3D( ){};
		virtual ~IFunction3D( ){};

	public:
		//! Evalues the function
		/// \return Scalar result of the function
		virtual Scalar Evaluate( 
			const Scalar x,						///< [in] X value to evaluate the function at
			const Scalar y,						///< [in] Y value to evaluate the function at
			const Scalar z						///< [in] Z value to evaluate the function at
			) const = 0;
	};
}

#endif
