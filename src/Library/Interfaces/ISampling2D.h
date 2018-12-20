//////////////////////////////////////////////////////////////////////
//
//  ISampling2D.h - Interface for sample generators.  These generate
//	a 2-D sampling kernel.
//                
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 30, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


#ifndef ISAMPLING_2D_
#define ISAMPLING_2D_

#include "IReference.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/Math3D/Math3D.h"
#include <vector>

namespace RISE
{
	//
	// Interface to a sample generator
	//
	class ISampling2D : public virtual IReference
	{
	protected:
		ISampling2D(){};
		virtual ~ISampling2D(){};

	public:	
		typedef std::vector<Point2> SamplesList2D;

		//! Sets the number of sample of points to generate
		virtual void SetNumSamples(
			const unsigned int num_samples						///< [in] Number of samples to generate
			) = 0;

		//! Returns the number of samples points to generate
		virtual unsigned int GetNumSamples() = 0;

		//! Retreives the list of samples
		virtual void GenerateSamplePoints( const RandomNumberGenerator& random, SamplesList2D& samples ) const = 0;

		//! Clones the sampling object
		/// \return A Clone of this sampling kernel
		virtual ISampling2D* Clone( ) const = 0;
	};
}

#endif
