//////////////////////////////////////////////////////////////////////
//
//  CSGObject.cpp - Implements the CSGObject class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: January 22, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "CSGObject.h"

using namespace RISE;
using namespace RISE::Implementation;

CSGObject::CSGObject( const CSG_OP& op_ ) :
  pObjectA( 0 ),
  pObjectB( 0 ),
  op( op_ )
{
}

CSGObject::~CSGObject( )
{
	if( pObjectA ) {
		pObjectA->SetWorldVisible( true );
	}

	if( pObjectB ) {
		pObjectB->SetWorldVisible( true );
	}

	safe_release( pObjectA );
	safe_release( pObjectB );
}

IObjectPriv* CSGObject::CloneFull()
{
	CSGObject*	pClone = new CSGObject( op );
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "clone" );

	pClone->AssignObjects( pObjectA, pObjectB );

	if( pMaterial ) {
		pClone->AssignMaterial( *pMaterial );
	}

	if( pModifier ) {
		pClone->AssignModifier( *pModifier );
	}

	if( pShader ) {
		pClone->AssignShader( *pShader );
	}

	if( pRadianceMap ) {
		pClone->AssignRadianceMap( *pRadianceMap );
	}

	return pClone;
}

IObjectPriv* CSGObject::CloneGeometric()
{
	CSGObject*	pClone = new CSGObject( op );
	GlobalLog()->PrintNew( pClone, __FILE__, __LINE__, "clone" );

	pClone->AssignObjects( pObjectA, pObjectB );
	return pClone;
}

bool CSGObject::AssignObjects( IObjectPriv* objA, IObjectPriv* objB )
{
	// Check the parameters
	if( !objA || !objB ) {
		GlobalLog()->PrintEx( eLog_Error, "CSGObject::AssignObjects:: bad objects" );
		return false;
	}

	if( pObjectA ) {
		pObjectA->SetWorldVisible( true );
	}

	if( pObjectB ) {
		pObjectB->SetWorldVisible( true );
	}

	safe_release( pObjectA );
	safe_release( pObjectB );

	pObjectA = objA;
	pObjectB = objB;

	pObjectA->addref();
	pObjectB->addref();

	pObjectA->SetWorldVisible( false );
	pObjectB->SetWorldVisible( false );
	
	return true;
}

void CSGObject::IntersectRay( RayIntersection& ri, const Scalar dHowFar, const bool, const bool, const bool ) const
{
	// The hitting of front and back faces are IGNORED for CSG objects!

	if( !pObjectA || !pObjectB ) {
		GlobalLog()->PrintSourceWarning( "CSGObject::IntersectRay:: No subobjects for this CSG object, ignoring hit request", __FILE__, __LINE__ );
		return;
	}

	// Bring the ray into our frame, first tuck away the original ray value
	Ray		orig = ri.geometric.ray;

	ri.geometric.bHit = false;
	ri.geometric.ray.origin = Point3Ops::Transform( m_mxInvFinalTrans, orig.origin );
	ri.geometric.ray.dir = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, orig.dir ));

	RayIntersection		riObjA( ri.geometric.ray, ri.geometric.rast );
	RayIntersection		riObjB( ri.geometric.ray, ri.geometric.rast );

	pObjectA->IntersectRay( riObjA, dHowFar, true, true, true );
	pObjectB->IntersectRay( riObjB, dHowFar, true, true, true );

	/* Not necessary, should never happen!
	if( riObjA.geometric.bHit && riObjA.geometric.range2 == INFINITY ) {
		riObjA.geometric.range2 = 0;
		_asm int 3h;
	}

	if( riObjB.geometric.bHit && riObjB.geometric.range2 == INFINITY ) {
		riObjB.geometric.range2 = 0;
		_asm int 3h;
	}
	*/

	// Do different things depending on the type of CSG operation
	switch( op )
	{
	default:
	case CSG_UNION:
		// In the event of a union, we take the intersection that is closest
		// in range.
		if( riObjA.geometric.bHit && riObjB.geometric.bHit )
		{
			// If B begins after A ends, then take A
			if( riObjB.geometric.range > riObjA.geometric.range2 ) {
				ri = riObjA;
			}
			// If A begins after B ends, then take B
			else if( riObjA.geometric.range > riObjB.geometric.range2 ) {
				ri = riObjB;
			}
			// If a comes first, then take a's start and B's end
			else if( riObjA.geometric.range < riObjB.geometric.range ) {
				ri = riObjA;
				ri.geometric.range2 = riObjB.geometric.range2;
				ri.geometric.vNormal2 = riObjB.geometric.vNormal2;
			}
			// take b's start and A's end, because B came first
			else {
				ri = riObjB;
				ri.geometric.range2 = riObjA.geometric.range2;
				ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
			}
		} else if( riObjA.geometric.bHit && !riObjB.geometric.bHit ) {
			ri = riObjA;
		} else if( !riObjA.geometric.bHit && riObjB.geometric.bHit ) {
			ri = riObjB;
		}
		break;
	case CSG_INTERSECTION:
		// If both the objects don't intersect, then there can't be an 
		// intersection
		if( riObjA.geometric.bHit && riObjB.geometric.bHit )
		{
			if( 
				(riObjA.geometric.range <= riObjB.geometric.range) &&
				(riObjB.geometric.range <= riObjA.geometric.range2) )
			{
				ri = riObjA;
				ri.geometric.range = riObjA.geometric.range;
				ri.geometric.range2 = riObjB.geometric.range2;
				ri.geometric.vNormal = riObjB.geometric.vNormal;
				ri.geometric.vNormal2 = riObjB.geometric.vNormal2;
			}
			else if( 
				(riObjB.geometric.range <= riObjA.geometric.range) &&
				(riObjA.geometric.range <= riObjB.geometric.range2) )
			{
				ri = riObjB;
				ri.geometric.range = riObjB.geometric.range;
				ri.geometric.range2 = riObjA.geometric.range2;
				ri.geometric.vNormal = riObjA.geometric.vNormal;
				ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
			}
		}
		break;
	case CSG_SUBTRACTION:
		// If we don't hit A, then no possible intersection
		if( riObjA.geometric.bHit )
		{
			if( riObjA.geometric.range2 == 0 && riObjB.geometric.range2 == 0 ) {
				// If we are inside both objects, then as long as we hit B before leaving A
				if( riObjB.geometric.range < riObjA.geometric.range ) {
					ri = riObjB;
					ri.geometric.range2 = riObjA.geometric.range;
					ri.geometric.vNormal = -riObjB.geometric.vNormal;
					ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
				}
			} else if( riObjA.geometric.range2 == 0 ) {
				// If we are inside A but not inside B
				// Then as long as we hit B before dHowFar then its all good
				// should always be true
				if( riObjB.geometric.bHit && riObjB.geometric.range < riObjA.geometric.range ) {
					ri = riObjB;
					ri.geometric.vNormal = -riObjB.geometric.vNormal;
				} else {
					ri = riObjA;
				}
				ri.geometric.range2 = 0;
			} else if( riObjB.geometric.range2 == 0 ) {
				// If we are inside B but not inside A, then as long as we hit A before exiting B
				if( riObjA.geometric.range < riObjB.geometric.range ) {
					ri = riObjB;
					ri.geometric.range2 = riObjA.geometric.range2;
					ri.geometric.vNormal = -riObjB.geometric.vNormal;
					ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
				}
			} else {
				// If we never hit B, or if B begins after A ends, or if B ends before A can begin
				// then we only have A
				if( !riObjB.geometric.bHit ) {
					ri = riObjA;
				} else {
					// We hit bit as well
					if( riObjA.geometric.range < riObjB.geometric.range )
					{
						ri = riObjA;
						ri.geometric.range2 = riObjB.geometric.range;
						ri.geometric.vNormal2 = -riObjB.geometric.vNormal;
					}
					else if( riObjB.geometric.range2 < riObjA.geometric.range2 )
					{
						ri = riObjB;
						ri.geometric.range = riObjB.geometric.range2;
						ri.geometric.range2 = riObjA.geometric.range2;
						// Reverse the normal
						ri.geometric.vNormal = -riObjB.geometric.vNormal2;
						ri.geometric.vNormal2 = riObjA.geometric.vNormal2;
					}
					else if( (riObjB.geometric.range >= riObjA.geometric.range2) ||
							 (riObjB.geometric.range2 <= riObjA.geometric.range) )
					{
						ri = riObjA;
					}
				}
			}
		}
		break;
	};

	if( ri.geometric.bHit )
	{
		// Transform the normal back
		ri.geometric.vNormal = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal ));
		ri.geometric.vNormal2 = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvTranspose, ri.geometric.vNormal2 ));

		ri.geometric.onb.CreateFromW( ri.geometric.vNormal );

		// Compute the intersection in world space
		ri.geometric.ptIntersection = Point3Ops::Transform( m_mxFinalTrans,	ri.geometric.ray.PointAtLength( ri.geometric.range - 1e-4 ) );
		ri.geometric.ptExit = Point3Ops::Transform( m_mxFinalTrans,	ri.geometric.ray.PointAtLength( ri.geometric.range2 + 1e-4 ) );

		// Re-compute the ranges in world space
		ri.geometric.range = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptIntersection, orig.origin ) );

		if( ri.geometric.range2 != 0 ) {
			ri.geometric.range2 = Vector3Ops::Magnitude( Vector3Ops::mkVector3( ri.geometric.ptExit, orig.origin ) );
		}

		if( pMaterial ) {
			ri.pMaterial = pMaterial;
		}

		if( pModifier ) {
			ri.pModifier = pModifier;
		}

		if( pShader ) {
			ri.pShader = pShader;
		}

		if( pRadianceMap ) {
			ri.pRadianceMap = pRadianceMap;
		}
	}

	// Restore the old ray
	ri.geometric.ray = orig;
}

bool CSGObject::IntersectRay_IntersectionOnly( const Ray& ray, const Scalar dHowFar, const bool , const bool ) const
{
	if( !pObjectA || !pObjectB ) {
		GlobalLog()->PrintSourceWarning( "CSGObject::IntersectRay_IntersectionOnly:: No subobjects for this CSG object, ignoring hit request", __FILE__, __LINE__ );
		return false;
	}

	// Bring the ray into our frame, but use our own copy
	Ray		orig = ray;

	orig.origin = Point3Ops::Transform( m_mxInvFinalTrans, ray.origin );
	orig.dir = Vector3Ops::Normalize( Vector3Ops::Transform( m_mxInvFinalTrans, ray.dir ));

	const Scalar factor = Vector3Ops::Magnitude( Vector3Ops::Transform( m_mxInvFinalTrans, Vector3(1,0,0) ) );
	Scalar dHowFar2 = INFINITY; 

	// We can't go farther than infinity, so in this case only reduce thelength, never extend
	if( (dHowFar != INFINITY) || (factor < 1.0) ) {
		dHowFar2 = factor*dHowFar;
	}

	RayIntersection		riObjA( orig, nullRasterizerState );
	RayIntersection		riObjB( orig, nullRasterizerState );

	// For CSG objects, we still need the ranges, even if it is for intersection only!

	pObjectA->IntersectRay( riObjA, dHowFar, true, true, false );
	pObjectB->IntersectRay( riObjB, dHowFar, true, true, false );

	// Do different things depending on the type of CSG operation
	switch( op )
	{
	default:
	case CSG_UNION:
		if( riObjA.geometric.bHit || riObjB.geometric.bHit ) {
			return ((riObjA.geometric.range < dHowFar2) || (riObjB.geometric.range < dHowFar2));
		}
		break;
	case CSG_INTERSECTION:
		// If both the objects don't intersect, then there can't be an 
		// intersection
		if( riObjA.geometric.bHit && riObjB.geometric.bHit ) {
			if( (riObjA.geometric.range <= riObjB.geometric.range) && (riObjB.geometric.range <= riObjA.geometric.range2) ) {
				return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
			}
			else if( (riObjB.geometric.range <= riObjA.geometric.range) && (riObjA.geometric.range <= riObjB.geometric.range2) ) {
				return (riObjB.geometric.range < dHowFar2 && riObjB.geometric.range > 0);
			}
		}
		break;
	case CSG_SUBTRACTION:
		// If we don't hit A, then no possible intersection
		if( riObjA.geometric.bHit )
		{
			if( riObjA.geometric.range2 == 0 && riObjB.geometric.range2 == 0 ) {
				// If we are inside both objects, then as long as we hit A before leaving B
				// B before we exit A we are ok
				return (riObjB.geometric.range < riObjA.geometric.range && riObjB.geometric.range < dHowFar2 );
			} else if( riObjA.geometric.range2 == 0 ) {
				// If we are inside A but not inside B
				// Then as long as we hit B before dHowFar2 then its all good
				// should always be true
				return true;
//				return (riObjB.geometric.range < dHowFar2 );
			} else if( riObjB.geometric.range2 == 0 ) {
				// If we are inside B but not inside A, then as long as we exit B before dHowFar2 its all good
				return (riObjB.geometric.range < dHowFar2 );
			} else {
				// If we never hit B, or if B begins after A ends, or if B ends before A can begin
				// then we only have A
				if( !riObjB.geometric.bHit ||
					(riObjB.geometric.range >= riObjA.geometric.range2) || 
					(riObjB.geometric.range2 <= riObjA.geometric.range) ) {
					return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
				} else {
					if( riObjA.geometric.range < riObjB.geometric.range && riObjA.geometric.range2 != 0 ) {
						return (riObjA.geometric.range < dHowFar2 && riObjA.geometric.range > 0);
					}
					else if( riObjB.geometric.range2 < riObjA.geometric.range2 ) {
						return (riObjB.geometric.range2 < dHowFar2 && riObjB.geometric.range2 > 0);
					}
				}
			}
		}
		break;
	};

	return false;
}

void CSGObject::ResetRuntimeData() const
{
	Object::ResetRuntimeData();
	if( pObjectA ) {
		pObjectA->ResetRuntimeData();
	}
	if( pObjectB ) {
		pObjectB->ResetRuntimeData();
	}
}

