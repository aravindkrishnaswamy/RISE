//////////////////////////////////////////////////////////////////////
//
//  IDetectorSphere.h - Defines an interface for a detector sphere
//  for virtual goniophotometers
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 11, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IDETECTOR_SPHERE_
#define IDETECTOR_SPHERE_

#include "IMaterial.h"
#include "IReference.h"
#include "IProgressCallback.h"

namespace RISE
{
	//! The sample geometry is what allows us to do collimnated samples or 
	//! samples of a disk
	class ISampleGeometry : public virtual IReference
	{
	protected:
		virtual ~ISampleGeometry(){};

	public:
		//! Returns a sample point
		virtual Point3 GetSamplePoint() const = 0;
	};

	//! The detector sphere represents the virtual detector sphere in a virtual goniophotometer
	//! It has a set of patches which record the amount of absolute reflectance during a trial. 
	//! This absolute reflectance is then converted into a BRDF (Bi-directional Reflectance Distribution Function)
	/// \sa IBSDF
	class IDetectorSphere : public virtual IReference
	{
	protected:
		IDetectorSphere(){};
		virtual ~IDetectorSphere(){}

	public:
		//! Performs a set of measurements, this will delete any measurements from before
		virtual void PerformMeasurement(
			const ISampleGeometry& pEmmitter,							///< [in] Sample geometry of the emmitter
			const ISampleGeometry& pSpecimenGeom,						///< [in] Sample geometry of the specimen
			const Scalar& radiant_power,								///< [in] Radiant power of the emmitter, given in watts
			const IMaterial& pSample,									///< [in] What we are measuring the BRDF of
			const unsigned int num_samples,								///< [in] Number of rays we will fire from the emitter for this measurement
			const unsigned int samples_base,							///< [in] Base for the number of rays, this is for casting a large number of samples
			IProgressCallback* pProgressFunc = 0,						///< [in] Callback functor to report progress
			int progress_rate = 0										///< [in] How often to update the callback function (number of rays to between each update)
			) = 0;

		//! Total number of patches
		/// \return Total number of patches in the sphere
		virtual unsigned int numPatches() const  = 0;

		//! Dumps the results to a CSV file for reading in Excel
		virtual void DumpToCSVFileForExcel(
			const char * szFile,										///< [in] Name of the file to dump to
			const Scalar phi_begin,										///< [in] phi angle to start dump
			const Scalar phi_end, 										///< [in] phi angle to end dump
			const Scalar theta_begin,									///< [in] theta angle to start dump
			const Scalar theta_end										///< [in] theta angle to end dump
			) const = 0;

		//! Dumps to a raw file for reading into Matlab
		virtual void DumpForMatlab(
			const char * szFile,										///< [in] Name of the file to dump to
			const Scalar phi_begin,										///< [in] phi angle to start dump
			const Scalar phi_end, 										///< [in] phi angle to end dump
			const Scalar theta_begin,									///< [in] theta angle to start dump
			const Scalar theta_end										///< [in] theta angle to end dump
			) const = 0;
	};
}

#endif

