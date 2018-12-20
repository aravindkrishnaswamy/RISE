//////////////////////////////////////////////////////////////////////
//
//  IsotropicRGBDetectorSphere.cpp - Implements a detector sphere
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 11, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <fstream>
#include "IsotropicRGBDetectorSphere.h"
#include "../Geometry/SphereGeometry.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RTime.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

IsotropicRGBDetectorSphere::IsotropicRGBDetectorSphere( ) : 
  m_pTopPatches( 0 ),
  m_pBottomPatches( 0 ),
  m_numPatches( 0 )
{
}

IsotropicRGBDetectorSphere::~IsotropicRGBDetectorSphere( )
{
	if( m_pTopPatches ) {
		GlobalLog()->PrintDelete( m_pTopPatches, __FILE__, __LINE__ );
		delete [] m_pTopPatches;
		m_pTopPatches = 0;
	}

	if( m_pBottomPatches ) {
		GlobalLog()->PrintDelete( m_pBottomPatches, __FILE__, __LINE__ );
		delete [] m_pBottomPatches;
		m_pBottomPatches = 0;
	}
}

void IsotropicRGBDetectorSphere::InitPatches( 
	const unsigned int numPatches,
	const PatchDiscretization discretization
	)
{
	if( m_pTopPatches ) {
		GlobalLog()->PrintDelete( m_pTopPatches, __FILE__, __LINE__ );
		delete m_pTopPatches;
		m_pTopPatches = 0;
	}

	// Allocate the memory for the patches
	m_pTopPatches = new PATCH[numPatches];
	GlobalLog()->PrintNew( m_pTopPatches, __FILE__, __LINE__, "top patches" );

	memset( m_pTopPatches, 0, sizeof( PATCH ) * numPatches );

	m_numPatches = numPatches;

	// Setup the top hemisphere of patches
	int	unsigned i=0;

	Scalar		last_te = 0.0;

	const Scalar		psa_per_patch = PI / Scalar(numPatches);			// Hemisphere
	const Scalar		OV_Num_Theta = 1.0 / Scalar(numPatches);

	const Scalar		delta_the = PI_OV_TWO / Scalar(numPatches); // adjusted to sphere
	const Scalar		h = 1.0 / Scalar(numPatches);

	Scalar tb=0, te=0;

	for( i=0; i<m_numPatches; i++ )
	{
		if( discretization == eEqualAngles ) {
			// This keeps the angle the same for each of the patches
			tb = i*delta_the;
			te = tb + delta_the;
		} else if( discretization == eEqualAreas ) {
			// This keeps the area the same for each of the patches
			tb = asin( h*Scalar(i) );
			te = asin( h*Scalar(i+1) );
		} else if( discretization == eExponentiallyIncreasingSolidAngles ) {	
			// Right from the paper
			tb = acos( h*Scalar(i+1) );
			te = acos( h*Scalar(i) );
		} else if( discretization == eEqualPSA ) {
			// Keeps the PSA the same, but assumes that the coses are embedded in the integration
			tb = last_te;
			te = acos( sqrt( fabs( OV_Num_Theta - cos(last_te)*cos(last_te) ) ) );
			last_te = te;
		}

		last_te = te;

		PATCH&	patch = m_pTopPatches[i];

		patch.dThetaBegin = tb;
		patch.dThetaEnd = te;

		if( discretization == eEqualPSA ) {
			patch.dSolidProjectedAngle = psa_per_patch;
		}

		patch.dRatio = RISEPel(0,0,0);
	}

	if( discretization != eEqualPSA ) {
		ComputePatchAreas();
	}

	// Now setup the bottom hemisphere of patches, the bottom hemisphere can be a direct copy of
	// the top, except that the theta's just need to be adjusted to add PI_OV_TWO
	if( m_pBottomPatches ) {
		GlobalLog()->PrintDelete( m_pBottomPatches, __FILE__, __LINE__ );
		delete [] m_pBottomPatches;
		m_pBottomPatches = 0;
	}

	// Allocate the memory for the patches
	m_pBottomPatches = new PATCH[m_numPatches];
	GlobalLog()->PrintNew( m_pBottomPatches, __FILE__, __LINE__, "bottom patches" );

	memcpy( m_pBottomPatches, m_pTopPatches, sizeof( PATCH ) * m_numPatches );

	// Now go through and adjust the thetas
	for( i=0; i<m_numPatches; i++ ) {
		PATCH&	patch = m_pBottomPatches[i];
		patch.dThetaBegin = PI - patch.dThetaBegin;
		patch.dThetaEnd = PI - patch.dThetaEnd;
	}
}

void IsotropicRGBDetectorSphere::ComputePatchAreas( )
{
	for( unsigned int i=0; i<m_numPatches; i++ )
	{
		PATCH& p = m_pTopPatches[i];

		// Alternate method of computing the projected solid angle
		p.dSolidProjectedAngle = 0.5 * (TWO_PI) * (cos(p.dThetaBegin)*cos(p.dThetaBegin) - cos(p.dThetaEnd)*cos(p.dThetaEnd) );
	}
}

IsotropicRGBDetectorSphere::PATCH* IsotropicRGBDetectorSphere::PatchFromTheta( const Scalar theta ) const
{
	// Find out which side first
	if( theta <= PI_OV_TWO ) {
		// Top patches
		for( unsigned int i=0; i<m_numPatches; i++ )
		{
			const PATCH& p = m_pTopPatches[i];
			if( theta >= p.dThetaBegin && theta <= p.dThetaEnd ) {
				return &m_pTopPatches[i];
			}
		}
	}
	
	if( theta >= PI_OV_TWO ) {
		// Bottom patches
		for( unsigned int i=0; i<m_numPatches; i++ )
		{
			const PATCH& p = m_pBottomPatches[i];
			if( theta >= p.dThetaEnd && theta <= p.dThetaBegin ) {
				return &m_pBottomPatches[i];
			}
		}
	}

	return 0;
}

void IsotropicRGBDetectorSphere::PerformMeasurement(
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
				)
{
	// Clear the current results
	{
		for( int i=0, e=numPatches()/2; i<e; i++ ) {
			m_pTopPatches[i].dRatio = RISEPel(0,0,0);
			m_pBottomPatches[i].dRatio = RISEPel(0,0,0);
		}
	}

	srand( GetMilliseconds() );

	// The detector geometry itself, which is a sphere
	SphereGeometry*	pDetector = new SphereGeometry( 1.0 );
	GlobalLog()->PrintNew( pDetector, __FILE__, __LINE__, "Detector sphere geometry" );

	Scalar		power_each_sample = radiant_power / (Scalar(num_samples) * Scalar(samples_base));

	// See below, this is power distributed to every patch in the ring at the top of the detector
	//Scalar		specialCaseDistributedEnergy = power_each_sample / Scalar(m_numPatches);

	unsigned int i = 0;

	ISPF* pSPF = pSample.GetSPF();

	if( !pSPF ) {
		return;
	}

	const Point3 pointOnEmmitter = GeometricUtilities::CreatePoint3FromSpherical( 0, dEmitterTheta );
	const Point3 pointOnSpecimen( 0, 0, 0 );

	// The emmitter ray starts at where the emmitter is, and heads towards the world origin
	const Ray emmitter_ray( pointOnEmmitter, Vector3Ops::Normalize(Vector3Ops::mkVector3(pointOnSpecimen,pointOnEmmitter)) );
	RayIntersectionGeometric ri( emmitter_ray, nullRasterizerState );
	ri.ptIntersection = Point3(0,0,0);
//	ri.onb.CreateFromW( Vector3( 1, 0, 0 ) );

	const Scalar nmdiff = nmend-nmbegin;
	Scalar nm = 0;

	for( ; i<num_samples; i++ )
	{
		for( unsigned int j = 0; j<samples_base; j++ )
		{
			// For each sample, fire it down to the surface, the fire the reflected ray to see
			// which detector it hits
			ScatteredRayContainer scattered;

			if( bSpectral ) {
				nm = nmdiff*random.CanonicalRandom() + nmbegin;
				pSPF->ScatterNM( ri, random, nm, scattered, 0 );
			} else {
				pSPF->Scatter( ri, random, scattered, 0 );
			}

			ScatteredRay* pScat = scattered.RandomlySelect( random.CanonicalRandom(), false );

			if( pScat )
			{
				// If the ray wasn't absorbed, then fire it at the detector patches
				RayIntersectionGeometric	ri( pScat->ray, nullRasterizerState );
				Vector3Ops::NormalizeMag(ri.ray.dir);

				// Ignore the front faces, just hit the back faces!
				pDetector->IntersectRay( ri, false, true, false );

				if( ri.bHit )
				{
					// Compute the deposited power on this detector
					//
					// To do this we just follow the instructions as laid out in 
					// Baranoski and Rokne's Eurographics tutorial
					// "Simulation Of Light Interaction With Plants"
					// 
					// This is the ratio between the radiant power reaching the detector
					// and the incident radiant power (which is multiplied by the projected solid angle)
					// Note that rather than accruing the power reaching the detector, we could simply
					// count the number of rays that hit the detector and use that as the ratio (still multiplying)
					// by the projected solid angle of course... either way is fine, neither is more or less
					// beneficial... though I could see an application of this method to ensure materials
					// maintain energy conservation.
					//
					// Here we just store the incident power, which is simple enough to do
					Scalar	phi=0,theta=0;

					if( GeometricUtilities::GetSphericalFromPoint3( ri.ptIntersection, phi, theta ) ) {
						if( theta < PI_OV_TWO ) {
							theta = PI_OV_TWO - theta;
						} else {
							theta = PI_OV_TWO + PI - theta;
						}

						PATCH* patch = PatchFromTheta( theta );
						if( !patch ) {
							GlobalLog()->PrintEx( eLog_Warning, "IsotropicRGBDetectorSphere::PerformMeasurement, Couldn't find patch, phi: %f, theta: %f", phi, theta );
						} else {
							if( bSpectral ) {
								XYZPel thisNM( 0, 0, 0 );
								if( ColorUtils::XYZFromNM( thisNM, nm ) ) {
									patch->dRatio = patch->dRatio + thisNM * power_each_sample*pScat->krayNM;
								}
							} else {
								patch->dRatio = patch->dRatio + XYZPel(power_each_sample*pScat->kray);
							}
						}
					}
				}
			}
		}

		if( (i % progress_rate == 0) && pProgressFunc ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(num_samples) ) ) {
				break;		// abort
			}
		}
	}

	safe_release( pDetector );

	if( pProgressFunc ) {
		pProgressFunc->Progress( 1.0, 1.0 );
	}

	unsigned int e = m_numPatches;
	for( i=0; i<e; i++ )
	{
		// Now we compute the ratio, 
		// which is the power reaching the detector, divided by 
		// the total incident power times the solid projected angle
		m_pTopPatches[i].dRatio = m_pTopPatches[i].dRatio * (1.0 / (radiant_power * m_pTopPatches[i].dSolidProjectedAngle)); 
		m_pBottomPatches[i].dRatio = m_pBottomPatches[i].dRatio * (1.0 / (radiant_power * m_pBottomPatches[i].dSolidProjectedAngle)); 
	}
}

