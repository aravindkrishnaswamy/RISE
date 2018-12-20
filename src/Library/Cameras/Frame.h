//////////////////////////////////////////////////////////////////////
//
//  Frame.h - Declaration of a frame.  A frame is made up of 
//  and orthonormal basis and a point of fixation
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 31, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAME_
#define FRAME_

#include "../Utilities/OrthonormalBasis3D.h"

namespace RISE
{
	//
	// Think of a frame as a picture frame, in a 3D world where the 
	// image gets projected onto.
	//
	class Frame
	{
	protected:
		OrthonormalBasis3D		basis;				/// The orthnormal basis describing basis vectors
		Point3					origin;				/// Origin of the frame in world co-ordinates
		unsigned int			width;				/// Width of the frame
		unsigned int			height;				/// Height of the frame

	public:
		// Canonical frame by default
		Frame( ){}

		// Frame set by given parameters
		Frame( 
			const OrthonormalBasis3D& b,
			const Point3& p, 
			const unsigned int w,
			const unsigned int h ) : 
		basis( b ),
		origin( p ),
		width( w ),
		height( h )
		{}

		inline void SetOrigin( const Point3& p )
		{
			origin = p;
		};

		inline void SetBasis( const OrthonormalBasis3D& b )
		{
			basis = b;
		};

		inline void SetDimensions( const unsigned int w, const unsigned int h )
		{
			width = w;
			height = h;
		};

		inline OrthonormalBasis3D GetBasis( ) const
		{
			return basis;
		};

		inline Point3 GetOrigin( ) const
		{
			return origin;
		};

		inline unsigned int GetWidth( ) const
		{
			return width;
		};

		inline unsigned int GetHeight( ) const
		{
			return height;
		};

		// Returns a transformation matrix to this frame from the canonical one
		inline Matrix4	GetTransformationMatrix( ) const
		{
			const Matrix4	t = Matrix4Ops::Translation( Vector3( origin.x, origin.y, origin.z ) );
			const Matrix4	m = t * basis.GetBasisToCanonicalMatrix( );
			return m;
		}
	};
}

#endif
