//////////////////////////////////////////////////////////////////////
//
//  AdaptiveDetectorSphere.cpp - Implements an adaptive detector sphere
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 2, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include <fstream>
#include "AdaptiveDetectorSphere.h"
#include "../Geometry/SphereGeometry.h"
#include "../Utilities/RandomNumbers.h"
#include "../Utilities/RTime.h"
#include "../Interfaces/ILog.h"
#include <algorithm>

using namespace RISE;
using namespace RISE::Implementation;

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// PatchQuatreeNode implementation
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

AdaptiveDetectorSphere::PatchQuatree::PatchQuatreeNode::PatchQuatreeNode() : 
  bChildren( false ),
  dTotalPower( 0 )
{
	for( int i=0; i<4; i++ ) {
		pChildren[i] = 0;
	}
}

AdaptiveDetectorSphere::PatchQuatree::PatchQuatreeNode::~PatchQuatreeNode()
{
	for( int i=0; i<4; i++ ) {
		if( pChildren[i] ) {
			GlobalLog()->PrintDelete( pChildren[i], __FILE__, __LINE__ );
			delete pChildren[i];
			pChildren[i] = 0;
		}
	}
}

void AdaptiveDetectorSphere::PatchQuatree::PatchQuatreeNode::DepositSample( 
					const Scalar dThetaBegin,				///< [in] 
					const Scalar dThetaEnd,					///< [in] 
					const Scalar dPhiBegin,					///< [in] 
					const Scalar dPhiEnd,					///< [in] 
					const unsigned int nMaxPatches,			///< [in] Maximum number of patches to tesselate to
					unsigned int& nNumPatches,				///< [out] A place where we can store the number of patches
					const Scalar dThreshold,				///< [in] Amount of power to store in the bin before splitting
					const Scalar dTheta,					///< [in] Angle in $\Theta$ for the sample
					const Scalar dPhi,						///< [in] Angle in $\Phi$ for the sample
					const Scalar dPower						///< [in] Amount of flux in the sample
					)
{
	// Check to see if we have children
	// If we have children, then just pass the call to each of the children

	//
	// Child 0 is thetabegin -> (thetaend+thetabegin)/2 and phibegin -> (phiend+phiend)/2
	// Child 1 is thetabegin -> (thetaend+thetabegin)/2 and phiend/2 -> (phiend+phiend)
	// Child 2 is (thetaend+thetabegin)/2 -> thetaend and phibegin -> (phiend+phiend)/2
	// Child 3 is (thetaend+thetabegin)/2 -> thetaend and (phiend+phiend)/2 -> phiend
	//

	if( bChildren ) {

		Scalar dThetaMid = (dThetaEnd+dThetaBegin)/2.0;
		Scalar dPhiMid = (dPhiEnd+dPhiBegin)/2.0;

		if( 
			(dTheta >= dThetaBegin) && 
			(dTheta < dThetaMid) &&
			(dPhi >= dPhiBegin) && 
			(dPhi < dPhiMid) )
		{
			// Child  0
			pChildren[0]->DepositSample( dThetaBegin, dThetaMid-NEARZERO, dPhiBegin, dPhiMid-NEARZERO, 
					nMaxPatches, nNumPatches, dThreshold, dTheta, dPhi, dPower );
		} 
		else if( 
			(dTheta >= dThetaBegin) && 
			(dTheta < dThetaMid) &&
			(dPhi >= dPhiMid) && 
			(dPhi <= dPhiEnd) )
		{
			// Child 1
			pChildren[1]->DepositSample( dThetaBegin, dThetaMid-NEARZERO, dPhiMid, dPhiEnd, 
					nMaxPatches, nNumPatches, dThreshold, dTheta, dPhi, dPower );
		}
		else if( 
			(dTheta >= dThetaMid) && 
			(dTheta <= dThetaEnd) &&
			(dPhi >= dPhiBegin) && 
			(dPhi < dPhiMid) )
		{
			// Child 2
			pChildren[2]->DepositSample( dThetaMid, dThetaEnd, dPhiBegin, dPhiMid-NEARZERO, 
					nMaxPatches, nNumPatches, dThreshold, dTheta, dPhi, dPower );
		}
		else 
		{
			// Child 3
			pChildren[3]->DepositSample( dThetaMid, dThetaEnd, dPhiMid, dPhiEnd, 
					nMaxPatches, nNumPatches, dThreshold, dTheta, dPhi, dPower );
		}

	} else {
		// Otherwise no children, so deposit here
		DeposittedPhoton		phot;
		phot.dTheta = dTheta;
		phot.dPhi = dPhi;
		phot.dDepositedPower = dPower;

		storedPhotons.push_back( phot );
		dTotalPower += dPower;


		// Check to see if the total power here is greater than the threshold, if it is
		// and we haven't yet reached the maximum number of patches, split!

		if( (dTotalPower > dThreshold) &&
			(nNumPatches < nMaxPatches) )
		{
			// Split off
			{for( int i=0; i<4; i++ ) {
				pChildren[i] = new PatchQuatreeNode();
			}}
			bChildren = true;

			nNumPatches += 4;

			Scalar dThetaMid = (dThetaEnd+dThetaBegin)/2.0;
			Scalar dPhiMid = (dPhiEnd+dPhiBegin)/2.0;

			// Iterate though our stored photons and deposit the appropriate ones into the child
			DeposittedPhotonsType::const_iterator	i, e;

			for( i=storedPhotons.begin(), e=storedPhotons.end(); i!=e; i++ ) 
			{
				const DeposittedPhoton& phot = *i;

				// Find out which child and give it to that child
				if( 
					(phot.dTheta >= dThetaBegin) && 
					(phot.dTheta < dThetaMid) &&
					(phot.dPhi >= dPhiBegin) && 
					(phot.dPhi < dPhiMid) )
				{
					// Child  0
					pChildren[0]->DepositSample( dThetaBegin, dThetaMid-NEARZERO, dPhiBegin, dPhiMid-NEARZERO, 
							nMaxPatches, nNumPatches, dThreshold, phot.dTheta, phot.dPhi, phot.dDepositedPower );
				} 
				else if( 
					(phot.dTheta >= dThetaBegin) && 
					(phot.dTheta < dThetaMid) &&
					(phot.dPhi >= dPhiMid) && 
					(phot.dPhi <= dPhiEnd) )
				{
					// Child 1
					pChildren[1]->DepositSample( dThetaBegin, dThetaMid-NEARZERO, dPhiMid, dPhiEnd, 
							nMaxPatches, nNumPatches, dThreshold, phot.dTheta, phot.dPhi, phot.dDepositedPower );
				}
				else if( 
					(phot.dTheta >= dThetaMid) && 
					(phot.dTheta <= dThetaEnd) &&
					(phot.dPhi >= dPhiBegin) && 
					(phot.dPhi < dPhiMid) )
				{
					// Child 2
					pChildren[2]->DepositSample( dThetaMid, dThetaEnd, dPhiBegin, dPhiMid-NEARZERO, 
							nMaxPatches, nNumPatches, dThreshold, phot.dTheta, phot.dPhi, phot.dDepositedPower );
				}
				else 
				{
					// Child 3
					pChildren[3]->DepositSample( dThetaMid, dThetaEnd, dPhiMid, dPhiEnd, 
							nMaxPatches, nNumPatches, dThreshold, phot.dTheta, phot.dPhi, phot.dDepositedPower );
				}
			}


			// Now clear the list
			storedPhotons.clear();

			DeposittedPhotonsType empty;
			storedPhotons.swap( empty );
		}
	}
}

void AdaptiveDetectorSphere::PatchQuatree::PatchQuatreeNode::FlattenTree( 
				const Scalar dThetaBegin,				///< [in] 
				const Scalar dThetaEnd,					///< [in] 
				const Scalar dPhiBegin,					///< [in] 
				const Scalar dPhiEnd,					///< [in] 
				PatchListType& patches					///< [out] Vector to store all patches in
				)
{
	if( !bChildren )  {
		// Pretty easy
		PATCH	p;
		p.dThetaBegin = dThetaBegin;
		p.dThetaEnd = dThetaEnd;
		p.dPhiBegin = dPhiBegin;
		p.dPhiEnd = dPhiEnd;
		p.dRatio = dTotalPower;

		p.dSolidProjectedAngle = 0.5 * (p.dPhiEnd-p.dPhiBegin) * (cos(p.dThetaBegin)*cos(p.dThetaBegin) - cos(p.dThetaEnd)*cos(p.dThetaEnd) );

		patches.push_back( p );
	}
	else 
	{
		Scalar dThetaMid = (dThetaEnd+dThetaBegin)/2.0;
		Scalar dPhiMid = (dPhiEnd+dPhiBegin)/2.0;

		// Pass it down to all the children
		pChildren[0]->FlattenTree( dThetaBegin, dThetaMid, dPhiBegin, dPhiMid, patches );
		pChildren[1]->FlattenTree( dThetaBegin, dThetaMid, dPhiMid, dPhiEnd, patches );
		pChildren[2]->FlattenTree( dThetaMid, dThetaEnd, dPhiBegin, dPhiMid, patches );
		pChildren[3]->FlattenTree( dThetaMid, dThetaEnd, dPhiMid, dPhiEnd, patches );
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// PatchQuatree implementation
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

AdaptiveDetectorSphere::PatchQuatree::PatchQuatree(
				const Scalar dThetaBegin,
				const Scalar dThetaEnd,
				const Scalar dPhiBegin,
				const Scalar dPhiEnd
				) : 
  m_dThetaBegin( dThetaBegin ),
  m_dThetaEnd( dThetaEnd ),
  m_dPhiBegin( dPhiBegin ),
  m_dPhiEnd( dPhiEnd )
{

}

AdaptiveDetectorSphere::PatchQuatree::~PatchQuatree()
{

}

void AdaptiveDetectorSphere::PatchQuatree::DepositSample( 
	const unsigned int nMaxPatches,			///< [in] Maximum number of patches to tesselate to
	unsigned int& nNumPatches,				///< [out] A place where we can store the number of patches
	const Scalar dThreshold,				///< [in] Amount of power to store in the bin before splitting
	const Scalar dTheta,					///< [in] Angle in $\Theta$ for the sample
	const Scalar dPhi,						///< [in] Angle in $\Phi$ for the sample
	const Scalar dPower						///< [in] Amount of flux in the sample
	)
{
	// Check if the sample is even within this detector sphere
	if( (dTheta < m_dThetaBegin) ||
		(dTheta > m_dThetaEnd) ||
		(dPhi < m_dPhiBegin) ||
		(dPhi > m_dPhiEnd) ) {
		return;
	}

	// I guess it is, toss it to root and let it handle this business
	root.DepositSample( m_dThetaBegin, m_dThetaEnd, m_dPhiBegin, m_dPhiEnd, nMaxPatches, nNumPatches, dThreshold, dTheta, dPhi, dPower );
}

void AdaptiveDetectorSphere::PatchQuatree::FlattenTree( 
	PatchListType& patches					///< [out] Vector to store all patches in
				)
{
	root.FlattenTree( m_dThetaBegin, m_dThetaEnd, m_dPhiBegin, m_dPhiEnd, patches );
}

/////////////////////////////////////////////////////////////////////////////////////////////////////
//
// AdaptiveDetectorSphere implementation
//
/////////////////////////////////////////////////////////////////////////////////////////////////////

AdaptiveDetectorSphere::AdaptiveDetectorSphere( ) : 
  m_pRoot( 0 ),
  m_MaxPatches( 0 ),
  m_dRadius( 0 ),
  m_dThreshold( 1.0 )
{
}

AdaptiveDetectorSphere::~AdaptiveDetectorSphere( )
{
	if( m_pRoot ) {
		GlobalLog()->PrintDelete( m_pRoot, __FILE__, __LINE__ );
		delete m_pRoot;
		m_pRoot = 0;
	}
}

/*
void AdaptiveDetectorSphere::ComputePatchAreas( const Scalar radius )
{
	Scalar	sqrRadius = radius * radius;

	for( unsigned int i=0; i<m_numThetaPatches/2; i++ )
	{
		for( unsigned int j=0; j<m_numPhiPatches; j++ )
		{
			const PATCH& p = m_pTopPatches[ i * m_numPhiPatches + j ];
			Scalar	patch_area = GeometricUtilities::SphericalPatchArea( p.dThetaBegin, p.dThetaEnd, p.dPhiBegin, p.dPhiEnd, radius );
			
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
*/

void AdaptiveDetectorSphere::InitPatches( 
								const unsigned int max_patches,								///< [in] Maximum number of patches to tesselate to
								const Scalar radius,										///< [in] Radius of the detector sphere
								const Scalar threshold										///< [in] Amount of power to store in the bin before splitting
								 )
{
	if( m_pRoot ) {
		GlobalLog()->PrintDelete( m_pRoot, __FILE__, __LINE__ );
		delete m_pRoot;
		m_pRoot = 0;
	}

	m_pRoot = new PatchQuatree( 0, PI_OV_TWO, 0, TWO_PI );
	m_dRadius = radius;	
	m_dThreshold = threshold;
	m_MaxPatches = max_patches;
}

void AdaptiveDetectorSphere::DumpToCSVFileForExcel( const char * szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const
{
	/*
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
	*/
}

void AdaptiveDetectorSphere::DumpForMatlab( const char* szFile, const Scalar phi_begin, const Scalar phi_end, const Scalar theta_begin, const Scalar theta_end ) const
{
/*
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
*/

	if( szFile ) {
		std::ofstream	f( szFile );

		PatchListType::const_iterator m, n;
		for( m=m_vTopPatches.begin(), n=m_vTopPatches.end(); m!=n; m++ ) {
			const PATCH& p = *m;
			f << p.dThetaBegin << " " << p.dThetaEnd << " " << p.dPhiBegin << " " << p.dPhiEnd << " " << p.dRatio << std::endl;
		}
	}
}

void AdaptiveDetectorSphere::PerformMeasurement(
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
	if( m_pRoot ) {
		GlobalLog()->PrintDelete( m_pRoot, __FILE__, __LINE__ );
		delete m_pRoot;
		m_pRoot = 0;
	}

	ISPF* pSPF = pSample.GetSPF();

	if( !pSPF ) {
		return;
	}

	unsigned int m_nNumPatches = 0;
	m_pRoot = new PatchQuatree( 0, PI_OV_TWO, 0, TWO_PI );

	srand( GetMilliseconds() );

	const Scalar power_each_sample = radiant_power / (Scalar(num_samples) * Scalar(samples_base));

	// The detector geometry itself, which is a sphere
	SphereGeometry*	pDetector = new SphereGeometry( m_dRadius );
	GlobalLog()->PrintNew( pDetector, __FILE__, __LINE__, "Detector sphere geometry" );

	unsigned int i = 0;

	for( ; i<num_samples; i++ )
	{
		for( unsigned int j = 0; j<samples_base; j++ )
		{
			Point3 pointOnEmmitter = pEmmitter.GetSamplePoint();
			Point3 pointOnSpecimen = pSpecimenGeom.GetSamplePoint();

			// The emmitter ray starts at where the emmitter is, and heads towards the world origin
			Ray emmitter_ray( pointOnEmmitter, 
					Vector3Ops::Normalize(Vector3Ops::mkVector3(pointOnSpecimen,pointOnEmmitter) )
					);

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
				Vector3Ops::NormalizeMag(ri.ray.dir );

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

						m_pRoot->DepositSample( m_MaxPatches, m_nNumPatches, m_dThreshold, theta, phi, power_each_sample*ColorMath::MaxValue(pScat->kray) );
					}
				}
			}

		}

		if( (i % progress_rate == 0) && pProgressFunc ) {
			if( !pProgressFunc->Progress( static_cast<double>(i), static_cast<double>(num_samples) ) ) {
				// abort
				break;
			}
		}
	}

	safe_release( pDetector );

//	Scalar	sqrRadius = (m_dRadius*m_dRadius);

	m_vTopPatches.clear();
	m_pRoot->FlattenTree( m_vTopPatches );

	PatchListType::iterator m, n;

	for( m=m_vTopPatches.begin(), n=m_vTopPatches.end(); m!=n; m++ ) {
		PATCH& p = *m;
		p.dRatio /= radiant_power * p.dSolidProjectedAngle;
	}

	/*
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
	*/
}

