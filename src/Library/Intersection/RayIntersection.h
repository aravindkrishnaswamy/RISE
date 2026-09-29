//////////////////////////////////////////////////////////////////////
//
//  RayIntersection.h - A class that describes an intersection of
//	a ray with some object, it incorporates the geometric aspects
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_INTERSECTION_
#define RAY_INTERSECTION_

#include "RayIntersectionGeometric.h"
#include <vector>

namespace RISE
{
	class IMaterial;
	class IRayIntersectionModifier;
	class IObject;
	class IShader;
	class IRadianceMap;

	class RayIntersection
	{
	public:
		RayIntersectionGeometric		geometric;		// geometric elements of ray intersection 

		const IMaterial*				pMaterial;		// the material at the intersection
		const IShader*					pShader;		// the shader at the intersection
		const IRayIntersectionModifier*	pModifier;		// something that modifies the intersection
														// this should be a list of somesort... eventually
		const IObject*					pObject;		// the object that was hit
		const IRadianceMap*				pRadianceMap;	// the radiance map at the intersection

        Scalar acceptedAlphaCoverage = 1; // Exact acceptance probability, never re-evaluated for deposits.

		RayIntersection( const Ray& ray, const RasterizerState& rast ) : 
		  geometric( ray, rast ),
		  pMaterial( 0 ),
		  pShader( 0 ),
		  pModifier( 0 ),
		  pObject( 0 ),
		  pRadianceMap( 0 )
		{}

		RayIntersection( const RayIntersectionGeometric& rig ) : 
		  geometric( rig ),
		  pMaterial( 0 ),
		  pShader( 0 ),
		  pModifier( 0 ),
		  pObject( 0 ),
		  pRadianceMap( 0 )
		{}

		RayIntersection( const RayIntersection& r ) :   
		  geometric( r.geometric ),
		  pMaterial( r.pMaterial ),
		  pShader( r.pShader ),
		  pModifier( r.pModifier ),
		  pObject( r.pObject ),
		  pRadianceMap( r.pRadianceMap )
		{ acceptedAlphaCoverage = r.acceptedAlphaCoverage; }
	};
	// Accepted medium boundaries from one sampled visibility segment.
	// Each record retains its world point and true geometric side; no replay.
	typedef std::vector<RayIntersection> MediumBoundaryHits;
}

#include "../Interfaces/IMaterial.h"
#include "../Interfaces/IRayIntersectionModifier.h"
#include "../Interfaces/IObject.h"
#include "../Interfaces/IShader.h"
#include "../Interfaces/IRadianceMap.h"

#endif
