//////////////////////////////////////////////////////////////////////
//
//  ISampling1D.h - Interface for sample generators.  These generate
//	a 1-D sampling kernel.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 23, 2004
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISAMPLING_1D_
#define ISAMPLING_1D_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/RandomNumbers.h"
#include <vector>

namespace RISE
{
	//
	// Interface to a sample generator
	//
	class ISampling1D : 
	public virtual IReference
	{
	protected:
		ISampling1D(){};
		virtual ~ISampling1D(){};

	public:	
		typedef std::vector<Scalar> SamplesList1D;

		//! Sets the number of sample of points to generate
		virtual void SetNumSamples(
			const unsigned int num_samples						///< [in] Number of samples to generate
			) = 0;

		//! Returns the number of samples points to generate
		virtual unsigned int GetNumSamples() = 0;

		//! Retreives the list of samples
		virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList1D& samples ) const = 0;

		//! Clones the sampling object
		/// \return A Clone of this sampling kernel
		virtual ISampling1D* Clone( ) const = 0;
	};
}

#endif
