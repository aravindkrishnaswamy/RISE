//////////////////////////////////////////////////////////////////////
//
//  ITwoColorOperator.h - A color operator that performs its
//  operation on two colors, one is the destination, which may
//  also be processed (depeding on the operation), and the other
//  is some kind of source.
// 
//  This is useful for stuff like composite operations
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 20, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef I_TWO_COLOR_OPERATOR_
#define I_TWO_COLOR_OPERATOR_

#include <algorithm>
#include "../Utilities/Color/Color.h"
#include "../Utilities/Color/Color_Template.h"
#include "IReference.h"
#include "IRasterImage.h"

namespace RISE
{
	//! The two color operator performs a color operation on one color with another color
	//! as a parameter
	class ITwoColorOperator : public virtual IReference
	{
	protected:
		virtual ~ITwoColorOperator(){};

	public:
		//! Performs the operator
		/// \return TRUE if the operation was successful, FALSE otherwise
		virtual bool PerformOperation( 
			RISEColor& dest,					///< [in/out] Destination/Source1 color
			const RISEColor& src				///< [in] Source2 color
			) const = 0;

	};

	static void Apply2ColorOperator( IRasterImage& pDest, const IRasterImage& pSrc, const ITwoColorOperator& pOp )
	{
		// The area of the operation is the minimum size between the two
		unsigned int	nWidth = std::min<unsigned int>( pSrc.GetWidth(), pDest.GetWidth() );
		unsigned int	nHeight = std::min<unsigned int>( pSrc.GetHeight(), pDest.GetHeight() );

		for( unsigned int y=0; y<nHeight; y++ ) {
			for( unsigned int x=0; x<nWidth; x++ ) {
				RISEColor dest = pDest.GetPEL( x, y );
				pOp.PerformOperation( dest, pSrc.GetPEL( x, y ) );
			}
		}
	}
}

#endif
