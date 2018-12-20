//////////////////////////////////////////////////////////////////////
//
//  RayTriangleIntersectionWithDisplacement.cpp - Implements a ray-
//    triangle intersection where the triangle is displacement
//    mapped.
//  
//  This comes from the paper by Smits et al. titled:
//  "Direct Ray Tracing of Displacement Mapped Triangles"
//  available here: http://www.cs.utah.edu/~shirley/papers/disp.pdf
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 18, 2004
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RayPrimitiveIntersections.h"

namespace RISE
{

	void RayTriangleIntersectionWithDisplacement( 
		const Ray& ray,
		TRIANGLE_HIT& hit,
		const Point3 (&vertices)[3],					// Three points of the triangle
		const Vector3 (&normals)[3],					// Three normals of the triangle
		const IFunction2D& displace,					// Displacement function
		const Scalar N,									// Subdivision amount
		const Scalar m,									// Maximum displacement below
		const Scalar M									// Maximum displacement above
		)
	{
		hit.bHit = false;
		hit.dRange = INFINITY;

		// Initialization phase:
		// First check to see if the ray intersects the volume of the displaced triangle
		// To do this, we need to intersect the ray by the three bilinear patches and the two
		// end-cap triangles

		Point3 top[3];			// The top end-cap
		top[0] = Point3Ops::mkPoint3( vertices[0], normals[0] * M );
		top[1] = Point3Ops::mkPoint3( vertices[1], normals[1] * M );
		top[2] = Point3Ops::mkPoint3( vertices[2], normals[2] * M );

		Point3 bottom[3];		// The bottom end-cap
		bottom[0] = Point3Ops::mkPoint3( vertices[0], normals[0] * -m );
		bottom[1] = Point3Ops::mkPoint3( vertices[1], normals[1] * -m );
		bottom[2] = Point3Ops::mkPoint3( vertices[2], normals[2] * -m );

		// Then there are the three bilinear patches
		BilinearPatch p0;
		p0.pts[0] = top[1];
		p0.pts[1] = bottom[1];
		p0.pts[2] = top[0];
		p0.pts[3] = bottom[0];

		BilinearPatch p1;
		p0.pts[0] = top[2];
		p0.pts[1] = bottom[2];
		p0.pts[2] = top[1];
		p0.pts[3] = bottom[1];

		BilinearPatch p2;
		p0.pts[0] = top[0];
		p0.pts[1] = bottom[0];
		p0.pts[2] = top[2];
		p0.pts[3] = bottom[2];

		// Now we need to basically intersect against all of these elements to figure which 
		// side the ray enters and which side it leaves
		TRIANGLE_HIT topcap, bottomcap;

		RayTriangleIntersection( ray, topcap, top[0], 
			Vector3Ops::mkVector3(top[1],top[0]), Vector3Ops::mkVector3(top[2],top[0]) );

		RayTriangleIntersection( ray, bottomcap, bottom[0], 
			Vector3Ops::mkVector3(bottom[1],bottom[0]), Vector3Ops::mkVector3(bottom[2],bottom[0]) );

		BILINEAR_HIT bh0, bh1, bh2;

		RayBilinearPatchIntersection( ray, bh0, p0 );
		RayBilinearPatchIntersection( ray, bh1, p1 );
		RayBilinearPatchIntersection( ray, bh2, p2 );
		
		// Now we have to figure out which the closest hits were and which the farthest hits were
		// Fortunately we can do this seperately

		// See if either of the end-caps are an entry point,
		char in=-1, out=-1;							// describes which side the ray comes in and which side it goes out
		Scalar inDist=INFINITY, outDist=0;			// in and out distances

		if( topcap.bHit ) {
			in = 0; 
			out = 0;
			inDist = topcap.dRange;
			outDist = topcap.dRange;
		}

		if( bottomcap.bHit ) {
			if( bottomcap.dRange < inDist ) {
				inDist = bottomcap.dRange;
				in = 1;
			}

			if( bottomcap.dRange > outDist ) {
				outDist = bottomcap.dRange;
				out = 1;
			}
		}

		// Check the three bi-linear patches
	#define CHECK_BILIN_PATCHES( p, val )	\
		if( ##p.bHit ) {					\
			if( ##p.dRange < inDist ) {		\
				inDist = ##p.dRange;		\
				in = val;					\
			}								\
			if( ##p.dRange > outDist ) {	\
				outDist = ##p.dRange;		\
				out = val;					\
			}								\
		}

		CHECK_BILIN_PATCHES( bh0, 2 );
		CHECK_BILIN_PATCHES( bh1, 3 );
		CHECK_BILIN_PATCHES( bh2, 4 );

	#undef CHECK_BILIN_PATCHES

		// Quick sanity check
		if( in==-1 && out !=-1 ||
			in!=-1 && out == -1 )
		{
			// Something is terribly wrong!
			_asm int 3h;
		}

		// Now we check our bailing case
		if( in==-1 && out==-1 ) {
			return;	// the volume is never intersected
		}

		// Another sanity check
		if( in==out ) {
			// You can't enter and leave the volume on the same element
			// Well actually you can, but until I add support for checking twice intersected
			// bilinear patches, lets keep this here
			_asm int 3h;
		}

		// Now we compute i,j,k which is the start co-ordinates and
		// ie,je,ke which are the end co-ordinates based on in and out
		Scalar alpha, beta, gamma;

	#define FIND_BARY( x )												\
		switch( ##x )													\
		{																\
		case 0:															\
			/* top cap	*/												\
			alpha = topcap.alpha;										\
			beta = topcap.beta;											\
			gamma = 1.0 - (topcap.alpha+topcap.beta);					\
			/* we set one to zero so that we go right to the end */		\
			if( alpha < beta && alpha < gamma ) alpha = 0;				\
			else if( beta < gamma ) beta = 0;							\
			else gamma = 0;												\
			break;														\
		case 1:															\
			/* bottom cap	*/											\
			alpha = bottomcap.alpha;									\
			beta = bottomcap.beta;										\
			gamma = 1.0 - (bottomcap.alpha+bottomcap.beta);				\
			/* we set one to zero so that we go right to the end */		\
			if( alpha < beta && alpha < gamma ) alpha = 0;				\
			else if( beta < gamma ) beta = 0;							\
			else gamma = 0;												\
			break;														\
		case 2:															\
			/* bilinear 0 - gamma is 0	*/								\
			gamma = 0;													\
			alpha = bh0.u;												\
			beta = 1.0-bh0.u;											\
			break;														\
		case 3:															\
			/* bilinear 1 - alpha is 0	*/								\
			alpha = 0;													\
			beta = bh1.u;												\
			gamma = 1.0-bh0.u;											\
			break;														\
		case 4:															\
			/* bilinear 2 - beta is 0	*/								\
			beta = 0;													\
			gamma = bh2.u;												\
			alpha = 1.0-bh2.u;											\
			break;														\
		};

		FIND_BARY( out );

		int ie = int(floor(alpha*N));
		int je = int(floor(beta*N));
		int ke = int(floor(gamma*N));

		FIND_BARY( in );

		int i = int(floor(alpha*N));
		int j = int(floor(beta*N));
		int k = int(floor(gamma*N));

		// Now that we have the in and the out, we are ready to start the traversal
		const Scalar delta = 1.0 / Scalar(N);
		
		enum AdvanceType
		{
			iplus, jminus, kplus, iminus, jplus, kminus
		};

	//	AdvanceType change;

		

		Point3 a, b, c;				// Microtriangle vertices, ordered
		Vector3 cNormal;			// normal at vertex c

		for(;;)
		{	
			//@ TODO: I should actually finish implementing this someday.....
			//@       .... someday when I have some time...
		}
	}


}