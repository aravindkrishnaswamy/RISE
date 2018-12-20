//////////////////////////////////////////////////////////////////////
//
//  ProbabilityDensityFunction.h - Represents the probabilty density
//  for a particular event.  The most significant use is warping 
//  canonical samples into samples adequate for this PDF.  Note that
//  the CDF at the 'end' of the function is 1
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 15, 2001
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PROBABILITYDENSITYFUNCTION_
#define PROBABILITYDENSITYFUNCTION_

#include <vector>
#include <algorithm>
#include "../Utilities/Color/Color.h"
#include "../Utilities/Reference.h"
#include "../Interfaces/IProbabilityDensityFunction.h"

namespace RISE
{
	namespace Implementation
	{
		class ProbabilityDensityFunction : public virtual IProbabilityDensityFunction, public virtual Reference
		{
		protected:
			typedef std::pair<Scalar,Scalar> ProbPair;
			typedef std::vector<ProbPair> DensityList;
			DensityList values;

			Scalar		LUTwarped[1024];
			Scalar		LUTweight[1024];
			Scalar		meanProbability;

			void Normalize( );
			void Normalize( const Scalar total );

			void BuildLUT( );

			virtual ~ProbabilityDensityFunction();

		public:
			//! Default constructor
			ProbabilityDensityFunction();

			//! This constructor builds a PDF from a given function
			ProbabilityDensityFunction( 
				const IFunction1D* pFunc,				///< [in] A 1-D function to build a PDF of
				const unsigned int numsteps				///< [in] Interval to sample the function
				);

			//! This constructor builds a PDF from a spectral packet
			ProbabilityDensityFunction( 
				const SpectralPacket& func				///< [in] The spectral packet
				);

			//! To satisfy the IFunction1D interface
			/// \return Returns the warped value of variable
			virtual Scalar Evaluate( 
				const Scalar variable					///< [in] Value to evaluate
				) const;

			//! Given a canonical random number is warps it and returns the value that corresponds 
			//! to that probability
			virtual Scalar warp( 
				const Scalar canonical,					///< [in] Some canonical value from a uniformly distributed PDF
				Scalar& weight,							///< [out] The weight of this sample
				bool bUseLUT=true						///< [in] Should lookup tables be used for optimization
				) const;
		};
	}
}

#endif
