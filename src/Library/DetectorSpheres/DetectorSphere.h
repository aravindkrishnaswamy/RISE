//////////////////////////////////////////////////////////////////////
//
//  DetectorSphere.h - An actual detector sphere for a virtual 
//  goniophotometer
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 11, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef DETECTOR_SPHERE_
#define DETECTOR_SPHERE_

#include "../Interfaces/IDetectorSphere.h"
#include "../Utilities/Reference.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/GeometricUtilities.h"

namespace RISE
{
	namespace Implementation
	{
		class DetectorSphere : public virtual IDetectorSphere, public virtual Reference
		{
		public:
			struct PATCH
			{
				Scalar		dThetaBegin;
				Scalar		dThetaEnd;
				Scalar		dPhiBegin;
				Scalar		dPhiEnd;
				Scalar		dRatio;
				Scalar		dAreaCos;
				Scalar		dArea;
				Scalar		dSolidProjectedAngle;
				Scalar		dCosT;
				Scalar		dKnownValue;
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
			unsigned int			m_numThetaPatches;
			unsigned int			m_numPhiPatches;
			Scalar					m_dRadius;
			PatchDiscretization		m_discretization;

			const RandomNumberGenerator	random;

			// Computes the inverse of the areas of the sphere patches using a three point
			// gaussian quadrature.  This is then multiplied by the cosine of the 
			// reflected theta vector
			void ComputePatchAreas( const Scalar radius );

			// Given the spherical angles, this functions finds which patch corresponds to those
			// angles
			PATCH* PatchFromAngles( const Scalar& theta, const Scalar phi ) const;

			virtual ~DetectorSphere( );	

		public:
			DetectorSphere( );		

			// Initializes the patches of the detector
			virtual void InitPatches( const unsigned int num_theta_patches, const unsigned int num_phi_patches, const Scalar radius, const PatchDiscretization discretization );

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

			virtual unsigned int numPatches() const { return m_numThetaPatches*m_numPhiPatches; }
			virtual unsigned int numThetaPatches() const { return m_numThetaPatches; }
			virtual unsigned int numPhiPatches() const { return m_numPhiPatches; }

			virtual void DumpToCSVFileForExcel( const char * szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const;
			virtual void DumpForMatlab( const char* szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const;

			virtual Scalar ComputeOverallRMSErrorIfPerfectlyDiffuse( ) const;

			virtual inline PATCH* getTopPatches( ) const{ return m_pTopPatches; };
			virtual inline PATCH* getBottomPatches( ) const{ return m_pBottomPatches; };
		};

		
		class PointSample :  public virtual ISampleGeometry, public virtual Reference
		{
		protected:
			Point3 pt;

		public:
			PointSample( const Point3 & p ) : pt( p ){}

			inline Point3 GetSamplePoint( ) const
			{
				return pt;
			}
		};

		class CircularDiskSample : public virtual ISampleGeometry, public virtual Reference
		{
		protected:
			Scalar 		radius;
			Matrix4	mx;

			RandomNumberGenerator		rng;

		public:
			CircularDiskSample( const Scalar radius_, Matrix4 mx_ ) : radius( radius_ ), mx( mx_ )
			{
			}

			Point3 GetSamplePoint( ) const
			{
				Point2 ran( rng.CanonicalRandom(), rng.CanonicalRandom() );
				Point2 ptDisk = GeometricUtilities::PointOnDisk( radius, ran );

				Point3 pt = Point3( ptDisk.x, ptDisk.y, 0 );
				return Point3Ops::Transform(mx, pt);
			}
		};
	}
}

#endif

