//////////////////////////////////////////////////////////////////////
//
//  CircularDiskDetector.cpp - Implements the detector made up
//    of circular disks
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 5, 2003
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CircularDiskDetector.h"
#include "../RISE_API.h"
#include "../Utilities/GeometricUtilities.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RTime.h"
#include "../Interfaces/ILog.h"
#include <algorithm>
#include <fstream>

using namespace RISE;
using namespace RISE::Implementation;

CircularDiskDetector::CircularDiskDetector( ) : 
  m_pPatches( 0 ), m_numThetaPatches( 0 ), m_dRadius( 0 )
{
}

CircularDiskDetector::~CircularDiskDetector( )
{
	if( m_pPatches ) {

		for( unsigned int i=0; i<m_numThetaPatches; i++ ) {
			safe_release( m_pPatches[i].pGeometry );
		}

		GlobalLog()->PrintDelete( m_pPatches, __FILE__, __LINE__ );
		delete [] m_pPatches;
		m_pPatches = 0;
	}
}

void CircularDiskDetector::InitPatches( const unsigned int num_theta_patches, const Scalar radius, const Scalar patchRadius )
{
	if( m_pPatches ) {
		GlobalLog()->PrintDelete( m_pPatches, __FILE__, __LINE__ );
		delete m_pPatches;
		m_pPatches = 0;
	}

	const Scalar	sqrRadius = radius * radius;

	m_dRadius = radius;
	m_dPatchRadius = patchRadius;

	// Allocate the memory for the patches
	m_pPatches = new PATCH[num_theta_patches];
	GlobalLog()->PrintNew( m_pPatches, __FILE__, __LINE__, "Patches" );

	memset( m_pPatches, 0, sizeof( PATCH ) * num_theta_patches );

	m_numThetaPatches = num_theta_patches;

	const Scalar		delta_the = PI/Scalar(num_theta_patches); // adjusted to hemi-sphere
	
	unsigned int i;
	for( i=0; i<num_theta_patches; i++ )
	{
		// This keeps the angle the same for each of the patches
		const Scalar tb=i*delta_the;
		const Scalar te=tb + delta_the;
		const Scalar tavg = (tb+te) * 0.5 - PI_OV_TWO;

		PATCH&	patch = m_pPatches[i];

		patch.dTheta = tavg;

		{
			IGeometry* pGeometry = 0;
			RISE_API_CreateCircularDiskGeometry( &pGeometry, m_dPatchRadius, 'z' );

			RISE_API_CreateObject( &patch.pGeometry, pGeometry );
//			patch.pGeometry->SetOrientation(  Vector3( 0, 0, PI_OV_TWO + tavg ) );
//			patch.pGeometry->SetPosition( Point3( m_dRadius*cos(tavg), m_dRadius*sin(tavg), 0 ) );

//			patch.pGeometry->SetOrientation(  Vector3( PI_OV_TWO - tavg, 0, PI_OV_TWO ) );
//			patch.pGeometry->SetPosition( Point3( 0, m_dRadius*sin(tavg), m_dRadius*cos(tavg) ) );

			patch.pGeometry->SetOrientation(  Vector3( 0, tavg, 0 ) );
			patch.pGeometry->SetPosition( Point3( m_dRadius*sin(tavg), 0, m_dRadius*cos(tavg) ) );

			patch.pGeometry->FinalizeTransformations();			

			patch.dArea = patch.pGeometry->GetArea();
			patch.dCosT = fabs(cos( tavg ));
			patch.dAreaCos = patch.dArea * patch.dCosT;
			patch.dSolidProjectedAngle = patch.dAreaCos / sqrRadius;
			
			safe_release( pGeometry );
		}

		patch.dRatio = 0;		
	}
}

void CircularDiskDetector::DumpToCSVFileForExcel( const char * szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const
{
	// We write our results to a CSV file so that it can be loaded in excel and nice graphs can be 
	// made from it...
	std::ofstream	f( szFile );
	f << "Theta, Patch area, Cos T, Solid Proj. Angle, Ratio\n";;
	for( int i=0, e = numPatches(); i<e; i++ ) {
		f << m_pPatches[i].dTheta * RAD_TO_DEG << ", "  << m_pPatches[i].dArea << ", " << m_pPatches[i].dCosT << ", " << m_pPatches[i].dSolidProjectedAngle << ", " << m_pPatches[i].dRatio << "\n";
	}
}

void CircularDiskDetector::DumpForMatlab( const char* szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const
{
	std::ofstream	f( szFile );
		
	for( unsigned int i=0; i<m_numThetaPatches; i++ ) {
		f << m_pPatches[i].dRatio << std::endl;
	}
}

int CircularDiskDetector::FindDepositedDetector( const Ray& ray_from_mat )
{
	RayIntersectionGeometric	ri( ray_from_mat, nullRasterizerState );
	Vector3Ops::NormalizeMag(ri.ray.dir);

	// Patches better not overlap!  We assume they don't
	for( unsigned int i=0; i<m_numThetaPatches; i++ ) {
		if( m_pPatches[i].pGeometry->IntersectRay_IntersectionOnly( ray_from_mat, INFINITY, true, true ) ) {
			return i;
		}
	}

	return -1;
}

void CircularDiskDetector::PerformMeasurement( 
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
		for( int i=0, e=numPatches(); i<e; i++ ) {
			m_pPatches[i].dRatio = 0;
		}
	}

	srand( GetMilliseconds() );

	// The emmitter ray starts at where the emmitter is, and heads towards the world origin
	Point3 pointOnEmmitter = pEmmitter.GetSamplePoint();
	Point3 pointOnSpecimen = pSpecimenGeom.GetSamplePoint();

	Ray			emmitter_ray( pointOnEmmitter, Vector3Ops::Normalize(Vector3Ops::mkVector3(pointOnSpecimen,pointOnEmmitter)) );

	RayIntersectionGeometric ri( emmitter_ray, nullRasterizerState );
	ri.ptIntersection = Point3(0,0,0);
//	ri.onb.CreateFromW( Vector3( 1, 0, 0 ) );

	Scalar		power_each_sample = radiant_power / (Scalar(num_samples) * Scalar(samples_base));

	unsigned int i = 0;

	ISPF* pSPF = pSample.GetSPF();

	if( !pSPF ) {
		return;
	}

	IORStack ior_stack( 1.0 );

	for( ; i<num_samples; i++ )
	{
		for( unsigned int j = 0; j<samples_base; j++ )
		{
			// For each sample, fire it down to the surface, the fire the reflected ray to see
			// which detector it hits
			ScatteredRayContainer scattered;

			pSPF->Scatter( ri, random, scattered, 0 );

			ScatteredRay* pScat = scattered.RandomlySelect( random.CanonicalRandom(), false );

			if( pScat )
			{
				// If the ray wasn't absorbed, then fire it at the detector patches
				int idx = FindDepositedDetector( pScat->ray );
				if( idx != -1 ) {
					m_pPatches[ idx ].dRatio += power_each_sample*ColorMath::MaxValue(pScat->kray);
				}
			}
		}


		if( (i % progress_rate == 0) && pProgressFunc ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(num_samples) ) ) {
				break;	// abort
			}
		}
	}

	unsigned int e = m_numThetaPatches;
	for( i=0; i<e; i++ )
	{
		//
		// Now we compute the ratio, 
		// which is the power reaching the detector, divided by 
		// the total incident power times the solid projected angle
		m_pPatches[i].dRatio /= radiant_power * m_pPatches[i].dSolidProjectedAngle; 
	}
}

