//////////////////////////////////////////////////////////////////////
//
//  IRasterImageAccessor.h - Defines an interface for a raster image 
//  accessor.  A raster image accessor is capable of accessing 
//  raster images sub-pixelly.
//                
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 26, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRASTERIMAGEACCESSOR_
#define IRASTERIMAGEACCESSOR_

#include "IFunction2D.h"
#include "IReference.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"

namespace RISE
{
	//! Provides access to a raster image.  This is mainly used to
	//! provide sub-pixel access to raster images
	class IRasterImageAccessor : public virtual IFunction2D
	{
	protected:
		IRasterImageAccessor( ){};
		virtual ~IRasterImageAccessor( ){};

	public:
		//! Gets an RISEColor
		virtual void GetPEL( 
			const Scalar x,						///< [in] sub-pixel X value of pixel
			const Scalar y,						///< [in] sub-pixel Y of pixel
			RISEColor& p							///< [out] Color of pixel
			) const = 0;

		//! Gets an RISEColor
		virtual void SetPEL( 
			const Scalar x,						///< [in] sub-pixel X value of pixel
			const Scalar y,						///< [in] sub-pixel Y value of pixel
			RISEColor& p							///< [in] Color to set
			) const = 0;

		//! Function2D requirements
		virtual Scalar	Evaluate( 
			const Scalar x,						///< [in] X
			const Scalar y						///< [in] Y
			) const = 0;
	};
}

#endif
