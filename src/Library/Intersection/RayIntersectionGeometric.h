//////////////////////////////////////////////////////////////////////
//
//  RayIntersectionGeometric.h - A class that describes the geometric
//  aspects of ray intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 17, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_INTERSECTION_GEOMETRIC_
#define RAY_INTERSECTION_GEOMETRIC_

#include "../Utilities/Ray.h"
#include "../Utilities/OrthonormalBasis3D.h"

namespace RISE
{
	//! Describes the current state of the rasterizer
	struct RasterizerState
	{
		unsigned int x;						// Which pixel on the row we are processing
		unsigned int y;						// Which row we are processing

		// ... more state elements will be added when they are deemed necessary
		bool operator==( const RasterizerState& other ) {
			return (x==other.x && y==other.y);
		}
	};

	static const RasterizerState nullRasterizerState = {0};

	class RayIntersectionGeometric
	{
	public:
		Ray							ray;			// the ray that is intersecting
		RasterizerState			rast;			// the rasterizer state
		bool						bHit;			// was there an intersection ? 
		Scalar						range;			// distance to the intersection point
		Scalar						range2;			// distance to the exit point
		Vector3						vNormal;		// normal at the point of intersection
		Vector3						vNormal2;		// normal at the point of exit
		Point2						ptCoord;		// texture mapping co-ordinates

		Point3						ptIntersection;	// the point in world co-ordinates of the intersection, only	
													// set if there was an intersection
		Point3						ptExit;			// point at which the ray exits the object

		Point3						ptObjIntersec;	// the point of intersection on object space
		Point3						ptObjExit;		// the point of exit in object space

		OrthonormalBasis3D			onb;			// the orthonormal basis at the point of intersection

		//! Some custom intersection data that an object would
		//! want to pass to a shader, we don't really care what it is
		//! as long as it is reference counted properly.
		//! NOTE: this is a huge hack to get 3DS MAX shaders to 
		//! work with our shaders.  The correct thing to do here is
		//! to refactor the entire idea of RayIntersection and RayIntersectionGeometric
		//! into a ShaderContext, BSDFContext, PainterContext, so that they
		//! can ask for whatever information they want.
		IReference*					pCustom;		

		RayIntersectionGeometric( const Ray& ray_, const RasterizerState& rast_ ) : 
		  ray( ray_ ),
		  rast( rast_ ),
		  bHit( false ),
		  range( INFINITY ),
		  range2( INFINITY ),
		  pCustom( 0 )
		{}

		~RayIntersectionGeometric( )
		{
			safe_release( pCustom );
		}

		RayIntersectionGeometric( const RayIntersectionGeometric& r ) :
		  ray( r.ray ),
		  rast( r.rast ),
		  bHit( r.bHit ),
		  range( r.range ),
		  range2( r.range2 ),
		  vNormal( r.vNormal ),
		  vNormal2( r.vNormal ),
		  ptCoord( r.ptCoord ),
		  ptIntersection( r.ptIntersection ),
		  ptExit( r.ptExit ),
		  ptObjIntersec( r.ptObjIntersec ),
		  ptObjExit( r.ptObjExit ),
		  onb( r.onb ),
		  pCustom( r.pCustom )
		{
			if( pCustom ) {
				pCustom->addref();
			}
		}

		inline RayIntersectionGeometric& operator=( const RayIntersectionGeometric& r )
		{
			rast = r.rast;
			ray = r.ray;
			bHit = r.bHit;
			range = r.range;
			range2 = r.range2;
			vNormal = r.vNormal;
			vNormal2 = r.vNormal2;
			ptCoord = r.ptCoord;
			ptIntersection = r.ptIntersection;
			ptExit = r.ptExit;
			ptObjIntersec = r.ptObjIntersec;
			ptObjExit = r.ptObjExit;
			onb = r.onb;

			safe_release( pCustom );
			pCustom = r.pCustom;

			if( pCustom ) {
				pCustom->addref();
			}

			return *this;
		}
	};
}

#endif
