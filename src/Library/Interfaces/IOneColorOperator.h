//////////////////////////////////////////////////////////////////////
//
//  IOneColorOperator.h - A color operator that performs its
//  operation on just one color, ie the source and 
//  destination of the operation are the same
// 
//  This is useful for stuff like filters
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 20, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_ONE_COLOR_OPERATOR_
#define I_ONE_COLOR_OPERATOR_

#include "../Utilities/Color/Color_Template.h"
#include "IReference.h"
#include "IRasterImage.h"

namespace RISE
{
	//! The one color operator performs a color operation on one color
	class IOneColorOperator : public virtual IReference
	{
	protected:
		virtual ~IOneColorOperator(){};

	public:
		//! Performs the operation on the given color
		virtual bool PerformOperation( 
			RISEColor& c					///< [in/out] Color to perform operation on
			) const = 0;

	};

	static void Apply1ColorOperator( IRasterImage& pImage, const IOneColorOperator& pOp )
	{
		unsigned int	nWidth = pImage.GetWidth();
		unsigned int	nHeight = pImage.GetHeight();

		for( unsigned int y=0; y<nHeight; y++ ) {
			for( unsigned int x=0; x<nWidth; x++ ) {
				RISEColor cCol = pImage.GetPEL( x, y );
				pOp.PerformOperation( cCol );
				pImage.SetPEL( x, y, cCol );				
			}
		}
	}
}

#endif
