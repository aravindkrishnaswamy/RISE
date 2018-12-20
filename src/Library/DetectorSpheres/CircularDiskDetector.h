//////////////////////////////////////////////////////////////////////
//
//  CircularDiskDetector.h - A detector for a virtual goniophotometer
//    made up of patches of circular disks
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 5, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef CIRCULARDISK_DETECTOR_
#define CIRCULARDISK_DETECTOR_

#include "../Interfaces/IDetectorSphere.h"
#include "../Interfaces/IObjectPriv.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RandomNumbers.h"

namespace RISE
{
	namespace Implementation
	{
		class CircularDiskDetector : public virtual IDetectorSphere, public virtual Reference
		{
		public:
			struct PATCH
			{
				Scalar			dTheta;
				Scalar			dRatio;
				Scalar			dAreaCos;
				Scalar			dArea;
				Scalar			dSolidProjectedAngle;
				Scalar			dCosT;
				IObjectPriv*	pGeometry;
			};

		protected:
			PATCH*					m_pPatches;
			unsigned int			m_numThetaPatches;
			Scalar					m_dRadius;						///< Radius of the detector itself
			Scalar					m_dPatchRadius;					///< Radius of the patches making up the detector

			const RandomNumberGenerator	random;

			virtual ~CircularDiskDetector( );	

			int FindDepositedDetector( const Ray& ray_from_mat );

		public:
			CircularDiskDetector( );		

			// Initializes the patches of the detector
			virtual void InitPatches( const unsigned int num_theta_patches, const Scalar radius, const Scalar patchRadius);

			// Performs a set of measurements, this will delete any measurements from before
			//
			// vEmmitterLocation - The exact location in R3 of the emmitter 
			// radiant_power - The radiant power of the emmitter, given in watts
			// pSample - What we are measuring the BRDF of.. this is a material that fill out
			//           the material record properly
			// num_samples - The number of rays we will fire from the emmitter for this
			//             measurement
			//
			virtual void PerformMeasurement( 
				const ISampleGeometry& pEmmitter,							///< [in] Sample geometry of the emmitter
				const ISampleGeometry& pSpecimenGeom,						///< [in] Sample geometry of the specimen
				const Scalar& radiant_power,
				const IMaterial& pSample,
				const unsigned int num_samples,
				const unsigned int samples_base,
				IProgressCallback* pProgressFunc = 0,
				int progress_rate=0
				);

			virtual unsigned int numPatches() const { return m_numThetaPatches; }
			virtual unsigned int numThetaPatches() const { return m_numThetaPatches; }

			virtual void DumpToCSVFileForExcel( const char * szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const;
			virtual void DumpForMatlab( const char* szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const;

			virtual inline PATCH* getPatches( ) const{ return m_pPatches; };
			virtual inline Scalar getPatchRadius() const{ return m_dPatchRadius; };
		};
	}
}

#endif

