//////////////////////////////////////////////////////////////////////
//
//  Vectors.h - Contains definition for a templated 2-D and 3D
//              Vector classes
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 13, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////
//
//  Descriptions of different types
//
//  Vector2		- Templeted 2D vector class
//  Vector2		- Templated 3D vector class
//
//////////////////////////////////////////////////////////////////////


#ifndef VECTORS_
#define VECTORS_

namespace RISE
{

	////////////////////////////////////
	//
	// 2-Dimension Vector class
	//
	////////////////////////////////////

	struct Vector2
	{
		//
		// Actual data
		//

		Scalar		x;
		Scalar		y;

		//
		// Constructors
		//
		inline		Vector2( ) : 
		x( 0 ),
		y( 0 )
		{};

		inline		Vector2( const Scalar val[3] ) : 
		x( val[0] ),
		y( val[1] )
		{};

		inline		Vector2( Scalar x_, Scalar y_ ) : 
		x( x_ ),
		y( y_ )
		{};

		
		// Copy Constructor
		inline		Vector2( const Vector2& v ) : 
		x( v.x ),
		y( v.y )
		{};

		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0 ? x : y;
		}

		// 
		// Operators
		//

		// Scalar multiplication 
		inline	friend	Vector2	operator*( const Vector2& v, const Scalar d )
		{
			return Vector2( v.x*d, v.y*d );
		}

		// Scalar multiplication
		inline	friend	Vector2	operator*( const Scalar d, const Vector2& v )
		{
			return Vector2( v.x*d, v.y*d );
		}

		// Scalar multiplication
		inline	Vector2	operator*=( const Scalar d )
		{
			x *= d;
			y *= d;

			return *this;
		}

		// Unary negation
		inline	friend	Vector2	operator-( const Vector2& v )
		{
			return Vector2( -v.x, -v.y );
		}

		// Addition
		inline friend	Vector2 operator+( const Vector2& a, const Vector2& b )
		{
			return Vector2( a.x+b.x, a.y+b.y );
		}

		// Subtraction
		inline friend	Vector2 operator-( const Vector2& a, const Vector2& b )
		{
			return Vector2( a.x-b.x, a.y-b.y );
		}
	};



	////////////////////////////////////
	//
	// 3-Dimension Vector class
	//
	////////////////////////////////////

	struct Vector3
	{
		//
		// Actual data
		//
		Scalar		x;
		Scalar		y;
		Scalar		z;

		//
		// Constructors
		//
		inline		Vector3( ) : 
		x( 0 ),
		y( 0 ),
		z( 0 )
		{};

		inline		Vector3( const Scalar val[3] ) : 
		x( val[0] ),
		y( val[1] ),
		z( val[2] )
		{};

		inline		Vector3( Scalar x_, Scalar y_, Scalar z_ ) : 
		x( x_ ),
		y( y_ ),
		z( z_ )
		{};

		
		// Copy Constructor
		inline		Vector3( const Vector3& v ) : 
		x( v.x ),
		y( v.y ),
		z( v.z )
		{};


		// Array style access
		inline		Scalar		operator[]( const unsigned int i ) const
		{
			return i==0?x : i==1? y : z;
		}

		// 
		// Operators
		//

		// Unary negation
		inline	friend	Vector3	operator-( const Vector3& v )
		{
			return Vector3( -v.x, -v.y, -v.z );
		}

		// Vector addition
		inline friend Vector3 operator+( const Vector3& a, const Vector3& b )
		{
			return Vector3( a.x+b.x, a.y+b.y, a.z+b.z );
		}

		// Subtraction
		inline friend	Vector3 operator-( const Vector3& a, const Vector3& b )
		{
			return Vector3( a.x-b.x, a.y-b.y, a.z-b.z );
		}

		// Scalar multiplication 
		inline friend	Vector3	operator*( const Vector3& v, const Scalar d )
		{
			return Vector3( v.x*d, v.y*d, v.z*d );
		}

		// Scalar multiplication
		inline friend	Vector3	operator*( const Scalar d, const Vector3& v )
		{
			return Vector3( v.x*d, v.y*d, v.z*d );
		}
	};
}

#endif
