//////////////////////////////////////////////////////////////////////
//
//  ISampler.h - Interface for abstract sampler used by BDPT
//    (and future MLT). ISampler wraps random number generation so
//    different sampling strategies (independent, primary sample
//    space MLT) can be swapped transparently.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 20, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISAMPLER_
#define ISAMPLER_

#include "Math3D/Math3D.h"

namespace RISE
{
	class ISampler
	{
	protected:
		ISampler(){};

	public:
		virtual ~ISampler(){};

		//! Returns a single uniform random sample in [0,1)
		virtual Scalar Get1D() = 0;

		//! Returns a 2D uniform random sample in [0,1)^2
		virtual Point2 Get2D() = 0;

		//! Start a new sample stream (for multiplexed MLT)
		//! streamIndex identifies which part of the path is consuming samples
		virtual void StartStream( int /*streamIndex*/ ){};

		//! Whether this sampler partitions dimensions into fixed-size
		//! phases and cannot tolerate variable-length consumption within
		//! a single phase.  When true, algorithms that consume an
		//! unpredictable number of dimensions (e.g., random-walk SSS)
		//! must use a separate IndependentSampler to avoid overflowing
		//! the phase budget and corrupting subsequent dimensions.
		//!
		//! Returns false by default.  SobolSampler overrides to true.
		//! PSSMLTSampler and IndependentSampler return false because
		//! they grow lazily or are purely random.
		virtual bool HasFixedDimensionBudget() const { return false; };
	};
}

#endif
