//////////////////////////////////////////////////////////////////////
//
//  IsotropicRGBDetectorSphere.h - A detector sphere that only
//    generates patches in theta and stores RGB values for 
//    each of the patches
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 26, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef ISOTROPIC_RGB_DETECTOR_SPHERE_H
#define ISOTROPIC_RGB_DETECTOR_SPHERE_H

#include "../Interfaces/IProgressCallback.h"
#include "../Interfaces/IMaterial.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	namespace Implementation
	{
		class IsotropicRGBDetectorSphere
		{
		public:
			struct PATCH
			{
				Scalar		dThetaBegin;
				Scalar		dThetaEnd;
				XYZPel		dRatio;
				Scalar		dSolidProjectedAngle;
			};

			enum PatchDiscretization
			{
				eEqualAngles						= 0,
				eEqualAreas							= 1, 
				eEqualPSA							= 2,
				eExponentiallyIncreasingSolidAngles	= 3
			};

		protected:
			PATCH*					m_pTopPatches;				// Top hemisphere patches
			PATCH*					m_pBottomPatches;			// Bottom hemisphere patches
			unsigned int			m_numPatches;

			const RandomNumberGenerator	random;

			// Computes the inverse of the areas of the sphere patches using a three point
			// gaussian quadrature.  This is then multiplied by the cosine of the 
			// reflected theta vector
			void ComputePatchAreas( );

			// Given theta, returns the corresponding patch
			PATCH* PatchFromTheta( const Scalar theta ) const;

		public:
			IsotropicRGBDetectorSphere( );		
			~IsotropicRGBDetectorSphere( );

			// Initializes the patches of the detector
			void InitPatches( const unsigned int numPathches, const PatchDiscretization discretization );

			// Performs a set of measurements, this will delete any measurements from before
			void PerformMeasurement( 
					const Scalar dEmitterTheta,						// Angle of the emitter
					const Scalar radiant_power,
					const IMaterial& pSample,
					const unsigned int num_samples,
					const unsigned int samples_base,
					const bool bSpectral,							// Are we doing spectral processing?
					const Scalar nmbegin,							// Begining wavelength for spectral processing
					const Scalar nmend,								// End wavelength for spectral processing
					IProgressCallback* pProgressFunc,
					int progress_rate
				);

			unsigned int numPatches() const { return m_numPatches*2; }

			inline PATCH* getTopPatches( ) const{ return m_pTopPatches; };
			inline PATCH* getBottomPatches( ) const{ return m_pBottomPatches; };
		};
	}
}

#endif

