//////////////////////////////////////////////////////////////////////
//
//  IProbabilityDensityFunction.h - Represents the probabilty density
//  for a particular event.  The most significant use is warping 
//  canonical samples into samples adequate for this PDF.  Note that
//  the CDF at the 'end' of the function is 1
//
//  This is the inteface for all PDFs.  A particular PDF can be 
//  implemented with a variety of underlying structures
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 18, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPROBABILITYDENSITYFUNCTION_
#define IPROBABILITYDENSITYFUNCTION_

#include "../Interfaces/IFunction1D.h"

namespace RISE
{
	class IProbabilityDensityFunction : public virtual IFunction1D
	{
	protected:
		IProbabilityDensityFunction(){};
		virtual ~IProbabilityDensityFunction(){};

	public:
		//! Given a canonical random number is warps it and returns the value that corresponds 
		//! to that probability
		/// \todo Move lookup table selection to be a parameter of the implementation (maybe) requires discussion
		virtual Scalar warp( 
			const Scalar canonical,					///< [in] Some canonical value from a uniformly distributed PDF
			Scalar& weight,							///< [out] The weight of this sample
			bool bUseLUT=true						///< [in] Should lookup tables be used for optimization
			) const = 0;
	};
}

#endif
