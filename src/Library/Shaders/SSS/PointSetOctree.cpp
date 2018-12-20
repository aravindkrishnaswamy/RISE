//////////////////////////////////////////////////////////////////////
//
//  PointSetOctree.cpp - Implementation of the point set octree class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2005
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "PointSetOctree.h"

using namespace RISE;
using namespace RISE::Implementation;

/////////////////////////////////////
// PointSetOctreeNode implementation
/////////////////////////////////////

PointSetOctree::PointSetOctreeNode::PointSetOctreeNode() : 
  pChildren( 0 ),
  pElements( 0 ),
  irrad( RISEPel(0,0,0) )
{
}

PointSetOctree::PointSetOctreeNode::~PointSetOctreeNode()
{
	if( pChildren ) {
		for( int i=0; i<8; i++ ) {
			if( pChildren[i] ) {
				GlobalLog()->PrintDelete( pChildren[i], __FILE__, __LINE__ );
				delete pChildren[i];
				pChildren[i] = 0;
			}
		}

		GlobalLog()->PrintDelete( pChildren, __FILE__, __LINE__ );
		delete pChildren;
		pChildren = 0;
	}

	if( pElements ) {
		GlobalLog()->PrintDelete( pElements, __FILE__, __LINE__ );
		delete pElements;
		pElements = 0;
	}
}

Scalar PointSetOctree::PointSetOctreeNode::HowFarIsPointFromYou(
	Vector3& dir,
	const Point3& point,
	const BoundingBox bbox,
	const char which_child
	) const
{
	BoundingBox my_bb;
	MyBBFromParent( bbox, which_child, my_bb );
	if( GeometricUtilities::IsPointInsideBox( point, my_bb.ll, my_bb.ur ) ) {
		return 0;
	}

	dir = Vector3Ops::mkVector3( my_bb.GetCenter(), point );
	return Vector3Ops::Magnitude( dir ) - Vector3Ops::Magnitude(my_bb.GetExtents()) * 0.5;
}

void PointSetOctree::PointSetOctreeNode::MyBBFromParent( 
	const BoundingBox& bbox, 
	char which_child, 
	BoundingBox& my_bb 
	) const
{
	// Figure out our bouding box based on the parent's bounding box and which child
	// we are... 
	Point3 ptBoxCenter = Point3Ops::WeightedAverage2( bbox.ll, bbox.ur, 0.5 );

	const Scalar&	AvgX = ptBoxCenter.x;
	const Scalar&	AvgY = ptBoxCenter.y;
	const Scalar&	AvgZ = ptBoxCenter.z;

	static const Scalar box_error = NEARZERO;

	switch( which_child )
	{
	case 99:
		// Entire
		my_bb = bbox;
		break;
	case 0:
		// Sub node 1, same LL as us, UR as our center
		my_bb.ll = Point3( bbox.ll.x-box_error, bbox.ll.y-box_error, bbox.ll.z-box_error );
		my_bb.ur = Point3( ptBoxCenter.x+box_error, ptBoxCenter.y+box_error, ptBoxCenter.z+box_error );
		break;
	case 1:
		// Sub node 2, almost the same LL as us, but x is now averaged with max
		// UR is our UR but z and y is averaged with min
		my_bb.ll = Point3( AvgX-box_error, bbox.ll.y-box_error, bbox.ll.z-box_error );
		my_bb.ur = Point3( bbox.ur.x+box_error, AvgY+box_error, AvgZ+box_error );
		break;
	case 2:
		// Sub node 3, almost same LL as us, but y is averaged. UR is same for y but x and 
		// z are averaged
		my_bb.ll = Point3( bbox.ll.x-box_error, AvgY-box_error, bbox.ll.z-box_error );
		my_bb.ur = Point3( AvgX+box_error, bbox.ur.y+box_error, AvgZ+box_error );
		break;
	case 3:
		// Sub node 4, LL.z is same as our LL but x and y are averaged, UR x and y are our UR but z is averaged
		my_bb.ll = Point3( AvgX-box_error, AvgY-box_error, bbox.ll.z-box_error );
		my_bb.ur = Point3( bbox.ur.x+box_error, bbox.ur.y+box_error, AvgZ+box_error );
		break;
	case 4:
		// Sub node 5, LL x and y is our LL, z is averaged, UR z is our UR y and z are averaged
		my_bb.ll = Point3( bbox.ll.x-box_error, bbox.ll.y-box_error, AvgZ-box_error );
		my_bb.ur = Point3( AvgX+box_error, AvgY+box_error, bbox.ur.z+box_error );
		break;
	case 5:
		// Sub node 6, LL x and z are averaged, y is our LL, UR, x and z are our UR and y is averaged
		my_bb.ll = Point3( AvgX-box_error, bbox.ll.y-box_error, AvgZ-box_error );
		my_bb.ur = Point3( bbox.ur.x+box_error, AvgY+box_error, bbox.ur.z+box_error );
		break;
	case 6:
		// Sub node 7, LL y and z are averaged, x is our LL, UR, y and z are our UR and x is averaged
		my_bb.ll = Point3( bbox.ll.x-box_error, AvgY-box_error, AvgZ-box_error );
		my_bb.ur = Point3( AvgX+box_error, bbox.ur.y+box_error, bbox.ur.z+box_error );
		break;
	case 7:
		// Sub node 8, LL is the center and UR is our UR
		my_bb.ll = Point3( ptBoxCenter.x-box_error, ptBoxCenter.y-box_error, ptBoxCenter.z-box_error );
		my_bb.ur = Point3( bbox.ur.x+box_error, bbox.ur.y+box_error, bbox.ur.z+box_error );
		break;
	};
}

const RISEPel& PointSetOctree::PointSetOctreeNode::AverageIrradiance(
				) const
{
	return irrad;
}

bool PointSetOctree::PointSetOctreeNode::AddElements( 
	const PointSet& points,
	const unsigned int maxElements,
	const BoundingBox& bbox,
	const char which_child,
	const unsigned char max_recursion_level
	)
{
	// We add the given elements to our section, 
	// First we see how many elements qualify for us, if that number is less than
	// or equal to the minimum defined, we don't create any children, and we simply keep
	// the polygons

	// If children must be created we subdivide evenly into 8 children passing
	// the element list
	static unsigned int tree_level = 0;
	tree_level++;

	BoundingBox my_bb;
	MyBBFromParent( bbox, which_child, my_bb );
	
	PointSet elements_list;
	PointSet::const_iterator i, e;
	for( i=points.begin(), e=points.end(); i!=e; i++ ) {
		if( GeometricUtilities::IsPointInsideBox( i->ptPosition, my_bb.ll, my_bb.ur ) ) {
			elements_list.push_back( *i );
		}
	}

	if( elements_list.size() < 1 ) {
		tree_level--;
		return false;
	}

	// If we have reached the maximum recursion level, then stop and don't try to create any more children
	if( tree_level > max_recursion_level || 
		elements_list.size() <= maxElements )
	{
		pElements = new PointSet( elements_list.size() );
		GlobalLog()->PrintNew( pElements, __FILE__, __LINE__, "Elements list" );
		std::copy( elements_list.begin(), elements_list.end(), pElements->begin() );

		// Compute the average irradiance
		PointSet::const_iterator i, e;
		for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
			irrad = irrad + i->irrad;
		}
		irrad = irrad * (1.0/Scalar(pElements->size()) );

		tree_level--;
		return true;
	}
	else
	{
		unsigned char	numRejects = 0;		// keeps track of how many children of this
											// node are crap, if they are all crap
											// then this node doesn't need to exist at all!

		// Subdivision required
		// Make eight children
		pChildren = new PointSetOctreeNode*[8];
		GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "point set octree children" );

		for( unsigned char x=0; x<8; x++ )
		{
			pChildren[x] = new PointSetOctreeNode( );
			GlobalLog()->PrintNew( pChildren[x], __FILE__, __LINE__, "ChildNode" );
			if( !pChildren[x]->AddElements( elements_list, maxElements, my_bb, x, max_recursion_level ) ) {
				GlobalLog()->PrintDelete( pChildren[x], __FILE__, __LINE__ );
				delete pChildren[x];
				pChildren[x] = 0;
				numRejects++;
			} else {
				irrad = irrad + pChildren[x]->AverageIrradiance();
			}
		}

		// If all of our children have been rejected, then there's no reason for this node itself
		// to even exist!
		if( numRejects == 8 ) {
			GlobalLog()->Print( eLog_Error, "PointSetOctreeNode: I have elements but none of my children do!  Should never happen" );
			GlobalLog()->PrintDelete( pChildren, __FILE__, __LINE__ );
			delete pChildren;
			pChildren = 0;
			tree_level--;
			return false;
		}

		irrad = irrad * (1.0/Scalar(8-numRejects) );
	}

	tree_level--;
	return true;
}

void PointSetOctree::PointSetOctreeNode::Evaluate(
	RISEPel& c,
	const BoundingBox& bbox,
	const char which_child,
	const Point3& point, 
	const ISubSurfaceExtinctionFunction& pFunc,
	const Scalar maxDistance,
	const IBSDF* pBSDF,
	const RayIntersectionGeometric& rig
	) const
{
	if( pChildren ) {
		BoundingBox my_bb;
		MyBBFromParent( bbox, which_child, my_bb );

		for( int i=0; i<8; i++ ) {
			// See if we should bother evaluating a particular child node
			if( pChildren[i] ) {
				Vector3 vdir;
				const Scalar dist = HowFarIsPointFromYou( vdir, point, my_bb, i );
				if( dist < maxDistance ) {
					pChildren[i]->Evaluate( c, my_bb, i, point, pFunc, maxDistance, pBSDF, rig );
				} else {
					// Use the node's average irradiance as an estimate
					if( pBSDF ) {
						c = c + pFunc.ComputeTotalExtinction( dist ) * pChildren[i]->AverageIrradiance() * pBSDF->value( vdir, rig ) ;
					} else {
						c = c + pFunc.ComputeTotalExtinction( dist ) * pChildren[i]->AverageIrradiance();
					}
				}
			}
		}
	}

	if( pElements ) {
		// Process the elements
		PointSet::const_iterator i, e;
		for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
			const Vector3& vdir = Vector3Ops::mkVector3( i->ptPosition, point );
			const Scalar dist = Vector3Ops::Magnitude( vdir );
			if( pBSDF ) {
				c = c + pFunc.ComputeTotalExtinction( dist ) * i->irrad * pBSDF->value( vdir, rig ) ;
			} else {
				c = c + pFunc.ComputeTotalExtinction( dist ) * i->irrad;
			}
		}
	}
}


/////////////////////////////////////
// PointSetOctree implementation
/////////////////////////////////////



