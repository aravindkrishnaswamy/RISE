//////////////////////////////////////////////////////////////////////
//
//  PointSetOctree.cpp - Hierarchical irradiance sample octree
//
//  See PointSetOctree.h for algorithm overview.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 21, 2005
//  Tabs: 4
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
		delete[] pChildren;
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
    my_bb=bbox;
    if(which_child==99) return;
    const Point3 center=bbox.GetCenter();
    // Exact sibling boxes. Membership below assigns the split plane to
    // the upper child; outer endpoints remain included at the root.
    my_bb.ll=Point3((which_child&1)?center.x:bbox.ll.x,
        (which_child&2)?center.y:bbox.ll.y,(which_child&4)?center.z:bbox.ll.z);
    my_bb.ur=Point3((which_child&1)?bbox.ur.x:center.x,
        (which_child&2)?bbox.ur.y:center.y,(which_child&4)?bbox.ur.z:center.z);

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
	const unsigned char max_recursion_level,
    unsigned tree_level
	)
{
	// We add the given elements to our section, 
	// First we see how many elements qualify for us, if that number is less than
	// or equal to the minimum defined, we don't create any children, and we simply keep
	// the polygons

	// If children must be created we subdivide evenly into 8 children passing
	// the element list
	tree_level++;

	BoundingBox my_bb;
	MyBBFromParent( bbox, which_child, my_bb );
	
	PointSet elements_list;
	PointSet::const_iterator i, e;
	for( i=points.begin(), e=points.end(); i!=e; i++ ) {
        const Point3& p=i->ptPosition;
        if(which_child==99) {
            if(p.x>=bbox.ll.x && p.x<=bbox.ur.x && p.y>=bbox.ll.y && p.y<=bbox.ur.y
                && p.z>=bbox.ll.z && p.z<=bbox.ur.z) elements_list.push_back(*i);
        } else {
            const Point3 center=bbox.GetCenter();
            const unsigned child=(p.x>=center.x?1u:0u)|(p.y>=center.y?2u:0u)|(p.z>=center.z?4u:0u);
            if(child==unsigned(which_child)) elements_list.push_back(*i);
        }
	}

	if( elements_list.size() < 1 ) {
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
            smsReferenceRadiance = smsReferenceRadiance || i->smsReferenceRadiance;
		}
		sampleCount = pElements->size();
		irrad = irrad * (1.0/Scalar(sampleCount) );

		return true;
	}
	else
	{
		unsigned char	numRejects = 0;		// keeps track of how many children of this
											// node are crap, if they are all crap
											// then this node doesn't need to exist at all!

		// Subdivision required
		// Make eight children
		pChildren = new PointSetOctreeNode*[8]();
		GlobalLog()->PrintNew( pChildren, __FILE__, __LINE__, "point set octree children" );

		for( unsigned char x=0; x<8; x++ )
		{
			pChildren[x] = new PointSetOctreeNode( );
			GlobalLog()->PrintNew( pChildren[x], __FILE__, __LINE__, "ChildNode" );
			if( !pChildren[x]->AddElements( elements_list, maxElements, my_bb, x, max_recursion_level, tree_level ) ) {
				GlobalLog()->PrintDelete( pChildren[x], __FILE__, __LINE__ );
				delete pChildren[x];
				pChildren[x] = 0;
				numRejects++;
			} else {
				sampleCount += pChildren[x]->sampleCount;
				irrad = irrad + pChildren[x]->AverageIrradiance() * Scalar(pChildren[x]->sampleCount);
                smsReferenceRadiance = smsReferenceRadiance || pChildren[x]->smsReferenceRadiance;
			}
		}

		// If all of our children have been rejected, then there's no reason for this node itself
		// to even exist!
		if( numRejects == 8 ) {
			GlobalLog()->Print( eLog_Error, "PointSetOctreeNode: I have elements but none of my children do!  Should never happen" );
			GlobalLog()->PrintDelete( pChildren, __FILE__, __LINE__ );
			delete[] pChildren;
			pChildren = 0;
				return false;
		}

		irrad = irrad * (1.0/Scalar(sampleCount) );
	}

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
	const RayIntersectionGeometric& rig,
	const IORStack* pIorStack,
	const Scalar exteriorIOR,
    bool* referenceRadiance
	) const
{
    // A cached sample/node is an opaque mixed return. Propagate its tag
    // only when its weighted value actually contributes to this evaluation.
    const auto accumulate=[&](const RISEPel& term,bool tagged) {
        c=c+term;
        if(referenceRadiance && tagged && (term[0]!=0 || term[1]!=0 || term[2]!=0)) *referenceRadiance=true;
    };

	if( pChildren ) {
		BoundingBox my_bb;
		MyBBFromParent( bbox, which_child, my_bb );

		for( int i=0; i<8; i++ ) {
			// See if we should bother evaluating a particular child node
			if( pChildren[i] ) {
				Vector3 vdir;
				const Scalar dist = HowFarIsPointFromYou( vdir, point, my_bb, i );
				if( dist < maxDistance ) {
					// DL-291: forward the live IOR stack (DL-223 dropped it here, so
					// every node below the root priced a stateful BSDF stacklessly)
					// and the exterior index to every depth.
					pChildren[i]->Evaluate( c, my_bb, i, point, pFunc, maxDistance, pBSDF, rig, pIorStack, exteriorIOR, referenceRadiance );
				} else {
					// Use the node's average irradiance as an estimate.
					// DL-291: air (every shipped scene) keeps the original
					// expressions verbatim; any other exterior prices the
					// profile against it.
					if( exteriorIOR == 1.0 ) {
						if( pBSDF ) {
							accumulate( pFunc.ComputeTotalExtinction( dist ) * (pChildren[i]->AverageIrradiance() * Scalar(pChildren[i]->sampleCount)) * pBSDF->valueStateful( vdir, rig, pIorStack ), pChildren[i]->smsReferenceRadiance );
						} else {
							accumulate( pFunc.ComputeTotalExtinction( dist ) * (pChildren[i]->AverageIrradiance() * Scalar(pChildren[i]->sampleCount)), pChildren[i]->smsReferenceRadiance );
						}
					} else {
						const RISEPel ext = pFunc.ComputeTotalExtinctionForExterior( dist, exteriorIOR );
						if( pBSDF ) {
							accumulate( ext * (pChildren[i]->AverageIrradiance() * Scalar(pChildren[i]->sampleCount)) * pBSDF->valueStateful( vdir, rig, pIorStack ), pChildren[i]->smsReferenceRadiance );
						} else {
							accumulate( ext * (pChildren[i]->AverageIrradiance() * Scalar(pChildren[i]->sampleCount)), pChildren[i]->smsReferenceRadiance );
						}
					}
				}
			}
		}
	}

	if( pElements ) {
		// Process the elements
		PointSet::const_iterator i, e;
		if( exteriorIOR == 1.0 ) {
			// Air: the original loop verbatim (DL-291 in-air identity).
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				const Vector3& vdir = Vector3Ops::mkVector3( i->ptPosition, point );
				const Scalar dist = Vector3Ops::Magnitude( vdir );
				if( pBSDF ) {
					accumulate( pFunc.ComputeTotalExtinction( dist ) * i->irrad * pBSDF->valueStateful( vdir, rig, pIorStack ), i->smsReferenceRadiance );
				} else {
					accumulate( pFunc.ComputeTotalExtinction( dist ) * i->irrad, i->smsReferenceRadiance );
				}
			}
		} else {
			for( i=pElements->begin(), e=pElements->end(); i!=e; i++ ) {
				const Vector3& vdir = Vector3Ops::mkVector3( i->ptPosition, point );
				const Scalar dist = Vector3Ops::Magnitude( vdir );
				const RISEPel ext = pFunc.ComputeTotalExtinctionForExterior( dist, exteriorIOR );
				if( pBSDF ) {
					accumulate( ext * i->irrad * pBSDF->valueStateful( vdir, rig, pIorStack ), i->smsReferenceRadiance );
				} else {
					accumulate( ext * i->irrad, i->smsReferenceRadiance );
				}
			}
		}
	}
}


/////////////////////////////////////
// PointSetOctree implementation
/////////////////////////////////////



