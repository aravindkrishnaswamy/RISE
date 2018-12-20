//////////////////////////////////////////////////////////////////////
//
//  IrradianceCache.cpp - Implementation of the irradiance cache
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 28, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "IrradianceCache.h"

using namespace RISE;
using namespace RISE::Implementation;

/////////////////////////////////////////////////////////////////
// CacheElement implementations
/////////////////////////////////////////////////////////////////

IrradianceCache::CacheElement::CacheElement(
	const Point3& pos,
	const Vector3& norm,
	const RISEPel& rad,
	const Scalar r0_,
	const Scalar weight,
	const RISEPel* rot,
	const RISEPel* tran
	) :
  ptPosition( pos ),
  vNormal( norm ),
  cIRad( rad ),
  r0( r0_ ),
  dWeight( weight )
{
	if( rot ) {
		rotationalGradient[0] = rot[0];
		rotationalGradient[1] = rot[1];
		rotationalGradient[2] = rot[2];
	}

	if( tran ) {
		translationalGradient[0] = tran[0];
		translationalGradient[1] = tran[1];
		translationalGradient[2] = tran[2];
	}
}

IrradianceCache::CacheElement::CacheElement( const CacheElement& ce, const Scalar weight ) :
  ptPosition( ce.ptPosition ),
  vNormal( ce.vNormal ),
  cIRad( ce.cIRad ),
  r0( ce.r0 ),
  dWeight( weight )
{
}

IrradianceCache::CacheElement::~CacheElement( )
{

}

Scalar IrradianceCache::CacheElement::ComputeWeight( const Point3& pos, const Vector3& norm ) const
{
	const Scalar om_ndot = 1.0 - Vector3Ops::Dot( norm, vNormal );
	const Scalar denom = Point3Ops::Distance(pos, ptPosition)/r0 + sqrt( om_ndot );
	if (denom > 0) {
		return 1.0 / denom;
	}
	// This means the point is right at a sample point so weight it highly
	return 1e10;
}

/////////////////////////////////////////////////////////////////
// CacheNode implementations
/////////////////////////////////////////////////////////////////

IrradianceCache::CacheNode::CacheNode( const Scalar size, const Point3& center ) :
  ptCenter( center ), dSize( size ), dHalfSize( size * 0.5 )
{
	for( int i=0; i<8; i++ ) {
		pChildren[i] = 0;
	}
}

IrradianceCache::CacheNode::~CacheNode( )
{
	Clear();
}

void IrradianceCache::CacheNode::Clear()
{
	for( int i=0; i<8; i++ ) {
		if( pChildren[i] ) {
			GlobalLog()->PrintDelete( pChildren[i], __FILE__, __LINE__ );
			delete pChildren[i];
			pChildren[i] = 0;
		}
	}
}

unsigned char IrradianceCache::CacheNode::WhichNode( const Point3& pos )
{
	//
	// First check left or right
	//

	// On the right
	if( pos.x > ptCenter.x )
	{
		// Check top and bottom
		if( pos.y > ptCenter.y )
		{
			// On the top

			// Check front and back
			if( pos.z > ptCenter.z ) {
				// in the back
				return 0;
			} else {
				return 1;
			}
		}
		else
		{
			// On the bottom

			if( pos.z > ptCenter.z ) {
				// Back
				return 2;
			} else {
				return 3;
			}
		}
	}
	else
	{
		// Check top and bottom
		if( pos.y > ptCenter.y )
		{
			// On the top

			// Check front and back
			if( pos.z > ptCenter.z ) {
				// in the back
				return 4;
			} else {
				return 5;
			}
		}
		else
		{
			// On the bottom

			if( pos.z > ptCenter.z ) {
				// Back
				return 6;
			} else {
				return 7;
			}
		}
	}
}

void IrradianceCache::CacheNode::InsertElement(
	const CacheElement& elem,
	const Scalar tolerance
	)
{
	static const Scalar size_error = NEARZERO;

	//
	// If the radiant energy is big enough for this node, then insert, otherwise
	// recurse down to the children and see if they can handle it
	//

	// The 4.0 is there as a fudge to make sure that we don't have to insert this cache point into
	// multiple children.  Should probably fix this to be more robust.
	if( dSize < elem.r0/tolerance*4.0 ) {
		values.push_back( elem );
	} else {
		// Create children if we have to
		// First find out which child this value will be inserted into
		const unsigned char idx = WhichNode( elem.ptPosition );
		const Scalar dChildSize = dHalfSize + size_error;
		const Scalar dChildHalfSize = dHalfSize * 0.5 + size_error;

		if( pChildren[idx] == 0 )
		{
			switch( idx )
			{
			case 0:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( dChildHalfSize, dChildHalfSize, dChildHalfSize ) ) );
				break;
			case 1:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( dChildHalfSize, dChildHalfSize, -dChildHalfSize ) ) );
				break;
			case 2:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( dChildHalfSize, -dChildHalfSize, dChildHalfSize ) ) );
				break;
			case 3:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( dChildHalfSize, -dChildHalfSize, -dChildHalfSize ) ) );
				break;
			case 4:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( -dChildHalfSize, dChildHalfSize, dChildHalfSize ) ) );
				break;
			case 5:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( -dChildHalfSize, dChildHalfSize, -dChildHalfSize ) ) );
				break;
			case 6:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( -dChildHalfSize, -dChildHalfSize, dChildHalfSize ) ) );
				break;
			case 7:
				pChildren[idx] = new CacheNode( dChildSize, Point3Ops::mkPoint3( ptCenter, Vector3( -dChildHalfSize, -dChildHalfSize, -dChildHalfSize ) ) );
				break;
			}

			GlobalLog()->PrintNew( pChildren[idx], __FILE__, __LINE__, "child node" );
		}

		pChildren[idx]->InsertElement( elem, tolerance );
	}
}

Scalar IrradianceCache::CacheNode::Query(
	const Point3& ptPosition,
	const Vector3& vNormal,
	std::vector<CacheElement>& results,
	const Scalar invTolerance
	) const
{
	std::vector<CacheElement>::const_iterator		i, e;
	Scalar		accruedWeights = 0;

	for( i=values.begin(), e=values.end(); i!=e; i++ )
	{
		const CacheElement& elem = *i;

		// Constraint for where the sample point is behind the irradiance point
		//const Scalar behind_test_criteria = Vector3Ops::Dot( Vector3Ops::mkVector3(elem.ptPosition, ptPosition), (elem.vNormal + vNormal) * 0.5);

	//	if (behind_test_criteria > 0)
		{
			Scalar	thisWeight = r_min( 1e10, elem.ComputeWeight( ptPosition, vNormal ) );

			if( thisWeight > invTolerance ) {
				CacheElement newelem( elem, thisWeight );
				results.push_back( newelem );
				accruedWeights += thisWeight;
			}
		}
	}

	// Short circuit, no need to check children
	if (accruedWeights > 0) return accruedWeights;

	for( int x = 0; x<8; x++ )
	{
		if( (pChildren[x]) &&
			(pChildren[x]->ptCenter.x - dHalfSize <= ptPosition.x && ptPosition.x <= pChildren[x]->ptCenter.x + dHalfSize ) &&
			(pChildren[x]->ptCenter.y - dHalfSize <= ptPosition.y && ptPosition.y <= pChildren[x]->ptCenter.y + dHalfSize ) &&
			(pChildren[x]->ptCenter.z - dHalfSize <= ptPosition.z && ptPosition.z <= pChildren[x]->ptCenter.z + dHalfSize ) )
		{
			accruedWeights += pChildren[x]->Query( ptPosition, vNormal, results, invTolerance );
		}
	}

	return accruedWeights;
}

bool IrradianceCache::CacheNode::IsSampleNeeded(
	const Point3& ptPosition,
	const Vector3& vNormal,
	const Scalar invTolerance
	) const
{
	std::vector<CacheElement>::const_iterator		i, e;

	// Base case
	// Check the values at this node first and see if any will do, if they suffice, we're ok and we say no
	for( i=values.begin(), e=values.end(); i!=e; i++ ) {
		const CacheElement& elem = *i;
		Scalar	thisWeight = r_min( 1e10, elem.ComputeWeight( ptPosition, vNormal ) );

		if( thisWeight > invTolerance ) {
			return false;
		}
	}

	// Otherwise check children
	for( int x = 0; x<8; x++ )
	{
		if( (pChildren[x]) &&
			(pChildren[x]->ptCenter.x - dHalfSize <= ptPosition.x && ptPosition.x <= pChildren[x]->ptCenter.x + dHalfSize ) &&
			(pChildren[x]->ptCenter.y - dHalfSize <= ptPosition.y && ptPosition.y <= pChildren[x]->ptCenter.y + dHalfSize ) &&
			(pChildren[x]->ptCenter.z - dHalfSize <= ptPosition.z && ptPosition.z <= pChildren[x]->ptCenter.z + dHalfSize ) )
		{
			bool bChildSampleCheck = pChildren[x]->IsSampleNeeded( ptPosition, vNormal, invTolerance );
			if( !bChildSampleCheck ) {
				return false;
			}
		}
	}

	return true;
}

/////////////////////////////////////////////////////////////////
// IrradianceCache implementations
/////////////////////////////////////////////////////////////////

IrradianceCache::IrradianceCache(
	const Scalar size,
	const Scalar tol,
	const Scalar min,
	const Scalar max
	) :
  root( CacheNode( size, Point3( 0, 0, 0 ) ) ),
  tolerance( tol ),
  invTolerance( 1.0/tol ),
  min_spacing( min ),
	max_spacing (max ),
  bPreComputed( false )
{
	if (max_spacing <= min_spacing) {
		// Hack in case the input got screwed up
		max_spacing = min_spacing * 100.0;
	}
}

IrradianceCache::~IrradianceCache( )
{
}
