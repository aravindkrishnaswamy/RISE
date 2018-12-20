//////////////////////////////////////////////////////////////////////
//
//  DetectorSphere.cpp - Implements a detector sphere
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
#include "DetectorSphere.h"
#include "../Geometry/SphereGeometry.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RTime.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

DetectorSphere::DetectorSphere( ) : 
  m_pTopPatches( 0 ),
  m_pBottomPatches( 0 ),
  m_numThetaPatches( 0 ),
  m_numPhiPatches( 0 ),
  m_dRadius( 0 ),
  m_discretization( eEqualPSA )
{
}

DetectorSphere::~DetectorSphere( )
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

void DetectorSphere::ComputePatchAreas( const Scalar radius )
{
//	Scalar	sqrRadius = radius * radius;

	for( unsigned int i=0; i<m_numThetaPatches/2; i++ )
	{
		for( unsigned int j=0; j<m_numPhiPatches; j++ )
		{
			const PATCH& p = m_pTopPatches[ i * m_numPhiPatches + j ];
			Scalar	patch_area = GeometricUtilities::SphericalPatchArea( PI_OV_TWO-p.dThetaEnd, PI_OV_TWO-p.dThetaBegin, p.dPhiBegin, p.dPhiEnd, radius );
			
			// approx. of theta_r

			// Cos(Theta) is approximated by averaging theta
//			Scalar	ctr = fabs( cos( (p.dThetaEnd-p.dThetaBegin)/2.0 + p.dThetaBegin ) );

			// Cos(Theta) is approximated by averaging the Cos(thetas)
			Scalar	ctr = (fabs( cos( p.dThetaBegin ) ) + fabs( cos( p.dThetaEnd ) )) / 2.0;

			{
				PATCH& p = m_pTopPatches[ i * m_numPhiPatches + j ];
				p.dArea = patch_area;
				p.dCosT = ctr;
				p.dAreaCos = p.dArea*p.dCosT;
//				p.dSolidProjectedAngle = p.dAreaCos / sqrRadius;
			
				// Alternate method of computing the projected solid angle
				p.dSolidProjectedAngle = 0.5 * (p.dPhiEnd-p.dPhiBegin) * (cos(p.dThetaBegin)*cos(p.dThetaBegin) - cos(p.dThetaEnd)*cos(p.dThetaEnd) );
			}
		}
	}
}

void DetectorSphere::InitPatches( 
								 const unsigned int num_theta_patches,
								 const unsigned int num_phi_patches,
								 const Scalar radius, 
								 const PatchDiscretization discretization
								 )
{
	if( m_pTopPatches ) {
		GlobalLog()->PrintDelete( m_pTopPatches, __FILE__, __LINE__ );
		delete m_pTopPatches;
		m_pTopPatches = 0;
	}

	m_dRadius = radius;
	m_discretization = discretization;

	// Allocate the memory for the patches
	m_pTopPatches = new PATCH[num_theta_patches*num_phi_patches/2];
	GlobalLog()->PrintNew( m_pTopPatches, __FILE__, __LINE__, "top patches" );

	memset( m_pTopPatches, 0, sizeof( PATCH ) * num_theta_patches*num_phi_patches/2 );

	m_numThetaPatches = num_theta_patches;
	m_numPhiPatches = num_phi_patches;

	const Scalar		delta_phi = 2.0 * PI/Scalar(num_phi_patches);
	const Scalar		delta_the = PI_OV_TWO/Scalar(num_theta_patches/2); // adjusted to sphere

	const Scalar		h = radius / Scalar(num_theta_patches/2);
	//Scalar		delta_costheta = 1.0 / Scalar(num_theta_patches/2);

	// Setup the top hemisphere of patches
	int	unsigned i=0, j=0;

	Scalar		last_te = 0.0;

	const Scalar		psa_per_patch = PI / Scalar(num_theta_patches/2);			// Hemisphere
	const Scalar		OV_Num_Theta = 1.0 / Scalar(num_theta_patches/2);

	for( i=0; i<num_theta_patches/2; i++ )
	{
		Scalar tb=0, te=0;

		if( m_discretization == eEqualAngles ) {
			// This keeps the angle the same for each of the patches
			tb = i*delta_the;
			te = tb + delta_the;
		} else if( m_discretization == eEqualAreas ) {
			// This keeps the area the same for each of the patches
			tb = asin( h*Scalar(i) / radius );
			te = asin( h*Scalar(i+1) / radius );

			// This also keeps the area the same for each of the patches, equal intervals in CosT space
//			tb = acos( delta_costheta * Scalar(i+1) );
//			te = acos( delta_costheta * Scalar(i) );
		} else if( m_discretization == eExponentiallyIncreasingSolidAngles ) {	
			// Right from the paper
			tb = acos( h*Scalar(i+1) / radius );
			te = acos( h*Scalar(i) / radius );
		} else if( m_discretization == eEqualPSA ) {

			// Keeps the PSA the same, but assumes that the coses are embedded in the integration
			tb = last_te;
			te = acos( sqrt( fabs( OV_Num_Theta - cos(last_te)*cos(last_te) ) ) );
			last_te = te;
		}

		for( j=0; j<num_phi_patches; j++ )
		{
			PATCH&	patch = m_pTopPatches[ i*num_phi_patches + j ];

			patch.dThetaBegin = tb;
			patch.dThetaEnd = te;

			patch.dPhiBegin = j*delta_phi;
			patch.dPhiEnd = patch.dPhiBegin + delta_phi;

			if( m_discretization == eEqualPSA ) {
				patch.dArea = GeometricUtilities::SphericalPatchArea( patch.dThetaBegin, patch.dThetaEnd, patch.dPhiBegin, patch.dPhiEnd, radius );
				patch.dSolidProjectedAngle = psa_per_patch / Scalar(num_phi_patches);
			}

			patch.dKnownValue = INV_PI;
			patch.dRatio = 0;
		}
	}

	if( m_discretization != eEqualPSA ) {
		ComputePatchAreas( m_dRadius );
	}

	// Now setup the bottom hemisphere of patches, the bottom hemisphere can be a direct copy of
	// the top, except that the theta's just need to be adjusted to add PI_OV_TWO
	if( m_pBottomPatches ) {
		GlobalLog()->PrintDelete( m_pBottomPatches, __FILE__, __LINE__ );
		delete [] m_pBottomPatches;
		m_pBottomPatches = 0;
	}

	// Allocate the memory for the patches
	m_pBottomPatches = new PATCH[num_theta_patches*num_phi_patches/2];
	GlobalLog()->PrintNew( m_pBottomPatches, __FILE__, __LINE__, "bottom patches" );

	memcpy( m_pBottomPatches, m_pTopPatches, sizeof( PATCH ) * num_theta_patches*num_phi_patches/2 );

	// Now go through and adjust the thetas
	for( i=0; i<num_theta_patches/2; i++ ) {
		for( j=0; j<num_phi_patches; j++ ) {
			PATCH&	patch = m_pBottomPatches[ i*num_phi_patches + j ];
			patch.dThetaBegin = PI - patch.dThetaBegin;
			patch.dThetaEnd = PI - patch.dThetaEnd;
		}
	}
}

void DetectorSphere::DumpToCSVFileForExcel( const char * szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const
{
	int i, e;

	// We write our results to a CSV file so that it can be loaded in excel and nice graphs can be 
	// made from it...
	std::ofstream	f( szFile );
	f << "Top hemisphere" << std::endl;
	f << "Theta begin, Theta end, Phi begin, Phi end, Patch area, Cos T, Solid Proj. Angle, Ratio\n";;
	for( i=0, e = numPatches()/2; i<e; i++ ) {
		f << m_pTopPatches[i].dThetaBegin * RAD_TO_DEG << ", " << m_pTopPatches[i].dThetaEnd * RAD_TO_DEG << ", " << m_pTopPatches[i].dPhiBegin * RAD_TO_DEG << ", " << m_pTopPatches[i].dPhiEnd * RAD_TO_DEG << ", "  << m_pTopPatches[i].dArea << ", " << m_pTopPatches[i].dCosT << ", " << m_pTopPatches[i].dSolidProjectedAngle << ", " << m_pTopPatches[i].dRatio << "\n";
	}

	f << std::endl << std::endl;

	f << "Bottom hemisphere" << std::endl;
	f << "Theta begin, Theta end, Phi begin, Phi end, Patch area, Cos T, Solid Proj. Angle, Ratio\n";;
	for( i=0, e = numPatches()/2; i<e; i++ ) {
		f << m_pBottomPatches[i].dThetaBegin * RAD_TO_DEG << ", " << m_pBottomPatches[i].dThetaEnd * RAD_TO_DEG << ", " << m_pBottomPatches[i].dPhiBegin * RAD_TO_DEG << ", " << m_pBottomPatches[i].dPhiEnd * RAD_TO_DEG << ", "  << m_pBottomPatches[i].dArea << ", " << m_pBottomPatches[i].dCosT << ", " << m_pBottomPatches[i].dSolidProjectedAngle << ", " << m_pBottomPatches[i].dRatio << "\n";
	}

}

void DetectorSphere::DumpForMatlab( const char* szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const
{
	std::ofstream	f( szFile );

	unsigned int i, j;

	for( i=0; i<m_numThetaPatches/2; i++ ) {
		for( j=0; j<m_numPhiPatches; j++ ) {
			f << m_pTopPatches[i*m_numPhiPatches+j].dRatio << " ";
		}

		f << std::endl;
	}

	for( i=0; i<m_numThetaPatches/2; i++ ) {
		for( j=0; j<m_numPhiPatches; j++ ) {
			f << m_pBottomPatches[i*m_numPhiPatches+j].dRatio << " ";
		}

		f << std::endl;
	}
}

DetectorSphere::PATCH* DetectorSphere::PatchFromAngles( const Scalar& theta, const Scalar phi ) const
{
	// Optimization: Finding out which phi is trivial, its which theta that is tricky...

	// This says its the nth phi patch on the mth theta ring
	const unsigned int	phi_num = int( (phi / TWO_PI) * Scalar(m_numPhiPatches));

	// Find out which side first
	if( theta <= PI_OV_TWO ) {
		// Top patches
		for( unsigned int i=0; i<m_numThetaPatches/2; i++ )
		{
			const unsigned int iPatchIdx = i*m_numPhiPatches+phi_num;
			const PATCH& p = m_pTopPatches[iPatchIdx];
			if( theta >= p.dThetaBegin && theta <= p.dThetaEnd ) {
				// This is the right ring...
				return &m_pTopPatches[iPatchIdx];
			}
		}
	}
	
	if( theta >= PI_OV_TWO ) {
		// Bottom patches
		for( unsigned int i=0; i<m_numThetaPatches/2; i++ )
		{
			const unsigned int iPatchIdx = i*m_numPhiPatches+phi_num;
			const PATCH& p = m_pBottomPatches[iPatchIdx];
			if( theta >= p.dThetaEnd && theta <= p.dThetaBegin ) {
				// This is the right ring...
				return &m_pBottomPatches[iPatchIdx];
			}
		}
	}

	return 0;
}

void DetectorSphere::PerformMeasurement(
				const ISampleGeometry& pEmmitter,							///< [in] Sample geometry of the emmitter
				const ISampleGeometry& pSpecimenGeom,						///< [in] Sample geometry of the specimen
				const Scalar& radiant_power,
				const IMaterial& pSample,
				const unsigned int num_samples,
				const unsigned int samples_base,
				IProgressCallback* pProgressFunc,
				int progress_rate
				)
{
	// Clear the current results
	{
		for( int i=0, e=numPatches()/2; i<e; i++ ) {
			m_pTopPatches[i].dRatio = 0;
			m_pBottomPatches[i].dRatio = 0;
		}
	}

	srand( GetMilliseconds() );

	// The detector geometry itself, which is a sphere
	SphereGeometry*	pDetector = new SphereGeometry( m_dRadius );
	GlobalLog()->PrintNew( pDetector, __FILE__, __LINE__, "Detector sphere geometry" );

	//Scalar		theta_size = Scalar(m_numThetaPatches) * INV_PI;
	//Scalar		phi_size = 0.5 * Scalar(m_numPhiPatches) * INV_PI;

	Scalar		power_each_sample = radiant_power / (Scalar(num_samples) * Scalar(samples_base));

	// See below, this is power distributed to every patch in the ring at the top of the detector
	Scalar		specialCaseDistributedEnergy = power_each_sample / Scalar(m_numPhiPatches);

	unsigned int i = 0;

	ISPF* pSPF = pSample.GetSPF();

	if( !pSPF ) {
		return;
	}

	for( ; i<num_samples; i++ )
	{
		for( unsigned int j = 0; j<samples_base; j++ )
		{
			Point3 pointOnEmmitter = pEmmitter.GetSamplePoint();
			Point3 pointOnSpecimen = pSpecimenGeom.GetSamplePoint();

			// The emmitter ray starts at where the emmitter is, and heads towards the world origin
			Ray			emmitter_ray( pointOnEmmitter, Vector3Ops::Normalize(Vector3Ops::mkVector3(pointOnSpecimen,pointOnEmmitter)) );
			RayIntersectionGeometric ri( emmitter_ray, nullRasterizerState );
			ri.ptIntersection = Point3(0,0,0);
		//	ri.onb.CreateFromW( Vector3( 1, 0, 0 ) );


			// For each sample, fire it down to the surface, the fire the reflected ray to see
			// which detector it hits
			ScatteredRayContainer scattered;

			pSPF->Scatter( ri, random, scattered, 0 );
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
					Scalar	phi=0, theta=0;

					if( Point3Ops::AreEqual(ri.ptIntersection, Point3( 0, 0, 1 ), NEARZERO ) )
					{
						// Special case:
						// The ray is reflected completely straight up, at precisely the point
						// where all the patches of one ring meet.  This was causing accuracy problems with 
						// certain BRDFs, so instead, the power is equally distributed to all the patches
						// that particular ring
						for( unsigned int k=(m_numThetaPatches-2)*m_numPhiPatches/2; k<m_numPhiPatches*m_numThetaPatches/2; k++ ) {
							m_pTopPatches[ k ].dRatio += specialCaseDistributedEnergy;
						}
					}
					else if( Point3Ops::AreEqual(ri.ptIntersection, Point3( 0, 0, -1 ), NEARZERO ) )
					{
						for( unsigned int k=(m_numThetaPatches-2)*m_numThetaPatches/2; k<m_numPhiPatches*m_numThetaPatches/2; k++ ) {
							m_pBottomPatches[ k ].dRatio += specialCaseDistributedEnergy;
						}
					}
					else
					{
						if( GeometricUtilities::GetSphericalFromPoint3( ri.ptIntersection, phi, theta ) ) {

#if 1
							if( theta < PI_OV_TWO ) {
								theta = PI_OV_TWO - theta;
							} else {
								theta = PI_OV_TWO + PI - theta;
							}
	
							if( phi < PI ) {
								phi = PI + phi;
							} else {
								phi = phi - PI;
							}
#endif

							PATCH* patch = PatchFromAngles( theta, phi );
							if( !patch ) {
								GlobalLog()->PrintEx( eLog_Warning, "DetectorSphere::PerformMeasurement, Couldn't find patch, phi: %f, theta: %f", phi, theta );
							} else {
								patch->dRatio += power_each_sample*ColorMath::MaxValue(pScat->kray);
							}
						}
					}
				}
			}

		}

		if( (i % progress_rate == 0) && pProgressFunc ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(num_samples) ) ) {
				break;	// abort
			}
		}
	}

	safe_release( pDetector );

	if( pProgressFunc ) {
		pProgressFunc->Progress( 1.0, 1.0 );
	}

//	Scalar	sqrRadius = (m_dRadius*m_dRadius);

	unsigned int e = m_numThetaPatches*m_numPhiPatches/2;
	for( i=0; i<e; i++ )
	{
		//
		// Now we compute the ratio, 
		// which is the power reaching the detector, divided by 
		// the total incident power times the solid projected angle
		m_pTopPatches[i].dRatio /= radiant_power * m_pTopPatches[i].dSolidProjectedAngle; 
		m_pBottomPatches[i].dRatio /= radiant_power * m_pBottomPatches[i].dSolidProjectedAngle; 
	}
}

Scalar DetectorSphere::ComputeOverallRMSErrorIfPerfectlyDiffuse( ) const
{
	//
	// Given the known value for each of the patches, it computes the overall error for
	// the entire detector
	//

	//
	// RMS error = sqrt( 1/M sum_i sum_j e^2(i,j) where e(i,j) = abs( f(i,j) - fr(i,j) )
	// where f(i,j) is the estimated value for the patch
	// and fr(i,j) is the actual value for the patch
	//
	Scalar		accruedSum=0;

	for( unsigned int i=0; i<numPatches(); i++ ) {
		Scalar		eij = m_pTopPatches[i].dRatio - m_pTopPatches[i].dKnownValue;
		accruedSum += eij*eij;
	}

	return sqrt(accruedSum / Scalar(numPatches()) );
}
