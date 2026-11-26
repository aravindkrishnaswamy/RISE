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

        // Unbacked geometric boundary parameters in the CURRENT caller ray's
        // distance units. Object captures these before its shading backoff;
        // CSG selects entry/exit metadata alongside its selected surface and
        // converts units once per nesting transform. Unknown producers leave
        // availability false. These never replace published shading ranges.
        bool hasBoundaryRange = false, hasBoundaryRange2 = false;
        bool bMaterialFromMix = false;       // Resolved from a MixMaterial -> never an SMS specular caster (packed next to boundary bools)
        Scalar boundaryRange = 0, boundaryRange2 = 0;
        Point3 BoundaryPoint() const {
            return hasBoundaryRange ? geometric.ray.PointAtLength(boundaryRange) : geometric.ptIntersection;
        }
        Scalar acceptedAlphaCoverage = 1; // Exact acceptance probability, never re-evaluated for deposits.
        //! MixMaterial resolution metadata (MIX §I.1-I.3 & Round 3 P1-1/P1-2/P1-6).
        const IMaterial* pUnresolvedMix = 0; // Original MixMaterial for expected-value denoiser guides
        const IRayIntersectionModifier* pPreMixModifier = 0; // Object/CSG modifier prior to leaf override
        Scalar mixCoverage = 1;              // Mix pass-through coverage (1 - w_clear) * ownAlpha
        Scalar mixConnectible = 1;           // q = P(connectible leaf | mix accepted) for VCM deposit
        //! DL-408: set only on a CONTACT hit -- one found by
        //! `BSSRDFSampling::IntersectContact`, a cast restricted to the
        //! body known to share the launch point's face.  It names the body
        //! the ray LEFT (on the arrival side of this hit, at zero
        //! distance).  A continuation from this hit back toward the arrival
        //! side meets that body at the same point and must itself be a
        //! contact launch restricted to it.  Null on every ordinary hit.
        const IObject* pContactPartner = 0;

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
		{ acceptedAlphaCoverage = r.acceptedAlphaCoverage;
          pUnresolvedMix = r.pUnresolvedMix;
          pPreMixModifier = r.pPreMixModifier;
          mixCoverage = r.mixCoverage;
          mixConnectible = r.mixConnectible;
          bMaterialFromMix = r.bMaterialFromMix;
          hasBoundaryRange = r.hasBoundaryRange; hasBoundaryRange2 = r.hasBoundaryRange2;
          boundaryRange = r.boundaryRange; boundaryRange2 = r.boundaryRange2;
          pContactPartner = r.pContactPartner; }

		/// Round 3 P1-2: unified surface coverage composing mix pass-through
		/// coverage with the resolved leaf material's own alpha coverage.
		inline Scalar HitCoverage() const;
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

namespace RISE
{
	inline Scalar RayIntersection::HitCoverage() const
	{
		return mixCoverage * ( pMaterial ? pMaterial->AlphaCoverage( geometric ) : Scalar( 1 ) );
	}
}

#endif
