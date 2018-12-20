//////////////////////////////////////////////////////////////////////
//
//  VectorsOps.h - Contains operations on vectors
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef VECTORS_OPS_
#define VECTORS_OPS_

#include "../../Interfaces/IWriteBuffer.h"
#include "../../Interfaces/IReadBuffer.h"

namespace RISE
{
	namespace Vector2Ops
	{
		inline Vector2 mkVector2( const Point2& b, const Point2& a )
		{
			return Vector2( b.x-a.x, b.y-a.y );
		}

		inline bool AreEqual( const Vector2& a, const Vector2& b, const Scalar epsilon )
		{
			return( fabs(a.x-b.x) <= epsilon &&
					fabs(a.y-b.y) <= epsilon );
		}

		// Dot product 
		inline Scalar Dot( const Vector2& a, const Vector2& b )
		{
			return Scalar( a.x*b.x + a.y*b.y );
		}

		// Squared Modulus
		inline Scalar SquaredModulus( const Vector2& v )
		{
			return Scalar( Dot(v,v) );
		}
		
		// Magnitude
		inline Scalar Magnitude( const Vector2& v )
		{
			return Scalar( sqrt( SquaredModulus(v) ) );
		}

		// Normalize the input vector, return the magnitude
		inline Scalar Normalize( Vector2& v )
		{
			Scalar		mag = Magnitude(v);
			if( mag > 0.0 ) {
				Scalar		ovmag = 1.0 / mag;
				v.x *= ovmag;
				v.y *= ovmag;
			}

			return mag;
		}

		// Return the normalized input vector
		inline Vector2 Normalize( const Vector2& v )
		{
			Scalar		mag = Magnitude(v);
			if( mag > 0.0 ) {
				Scalar		ovmag = 1.0 / mag;
				return Vector2( v.x * ovmag, v.y * ovmag );
			}

			return v;
		}

		// Returns a vector perpendicular to the given one
		inline Vector2 Perpendicular( const Vector2& v )
		{
			return Vector2( v.y, -v.x );
		}

			
		// Matrix transform a vector
		inline Vector2 Transform( const Matrix3& m, const Vector2& v )
		{
			return Vector2(
					m._00 * v.x + m._10 * v.y,
					m._01 * v.x + m._11 * v.y
			);
		}

		inline Vector2 WeightedAverage2(
			const Vector2& p0,
			const Vector2& p1,
			const Scalar alpha,
			const Scalar beta )
		{
			return Vector2(	
				alpha*p0.x+ beta*p1.x,
				alpha*p0.y+ beta*p1.y );
		}

		inline Vector2 WeightedAverage3( 
			const Vector2& p0,
			const Vector2& p1,
			const Vector2& p2,
			const Scalar alpha,
			const Scalar beta,
			const Scalar gamma )
		{
			return Vector2(	
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y );
		}

		inline Vector2 WeightedAverage3(
			const Vector2& p0,
			const Vector2& p1,
			const Vector2& p2,
			const Scalar alpha,
			const Scalar beta )
		{
			const Scalar gamma = 1.0 - alpha - beta;
			return Vector2(	
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y );
		}

		inline void Serialize( 
			const Vector2& p, 
			IWriteBuffer& buffer )
		{
			buffer.ResizeForMore( sizeof( Scalar ) * 2 );
			buffer.setDouble( p.x );
			buffer.setDouble( p.y );
		}

		inline void Deserialize( 
			Vector2& v,
			IReadBuffer& buffer )
		{
			v.x = buffer.getDouble();
			v.y = buffer.getDouble();
		}
	}

	namespace Vector3Ops
	{
		inline Vector3 mkVector3( const Point3& b, const Point3& a )
		{
			return Vector3( b.x-a.x, b.y-a.y, b.z-a.z );
		}

		inline bool AreEqual( const Vector3& a, const Vector3& b, const Scalar epsilon )
		{
			return( fabs(a.x-b.x) <= epsilon &&
					fabs(a.y-b.y) <= epsilon && 
					fabs(a.z-b.z) <= epsilon );
		}

		// Returns index of smallest component
		inline int IndexOfMinAbsComponent( const Vector3& v )
		{
			const Scalar x = fabs(v.x);
			const Scalar y = fabs(v.y);
			const Scalar z = fabs(v.z);
			if( x < y && x < z )
				return 0;
			else if( y < z )
				return 1;
			else
				return 2;
		}

		// Returns a vector perpendicular to this one
		inline Vector3 Perpendicular( const Vector3& v )
		{
			int axis = IndexOfMinAbsComponent(v);
			if( axis == 0 )
			return Vector3(0.0, v.z, -v.y);
			else if( axis == 1 )
			return Vector3(v.z, 0.0, -v.x);
			else
			return Vector3(v.y, -v.x, 0.0);

		}

		// Dot product
		inline Scalar Dot( const Vector3& a, const Vector3& b )
		{
			return Scalar( a.x*b.x + a.y*b.y + a.z*b.z );
		}

		// Cross product
		inline Vector3 Cross( const Vector3& a, const Vector3& b )
		{
			return Vector3( a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x );
		}

		// Squared Modulus
		inline Scalar SquaredModulus( const Vector3& v )
		{
			return Scalar( Dot(v,v) );
		}
		
		// Magnitude
		inline Scalar Magnitude( const Vector3& v )
		{
			return Scalar( sqrt( SquaredModulus(v) ) );
		}

		// Normalize the input vector, return the magnitude
		inline Scalar NormalizeMag( Vector3& v )
		{
			Scalar		mag = Magnitude(v);
			if( mag > 0.0 ) {
				Scalar		ovmag = 1.0 / mag;
				v.x *= ovmag;
				v.y *= ovmag;
				v.z *= ovmag;
			}

			return mag;
		}

		// Return the normalized input vector
		inline Vector3 Normalize( const Vector3& v )
		{
			Scalar		mag = Magnitude(v);
			if( mag > 0.0 ) {
				Scalar		ovmag = 1.0 / mag;
				return Vector3( v.x * ovmag, v.y * ovmag, v.z * ovmag );
			}

			return v;
		}

		// Matrix transforms a vector
		inline Vector3 Transform( const Matrix4& m, const Vector3& v )
		{
			return Vector3(
				m._00 * v.x + m._10 * v.y + m._20 * v.z,
				m._01 * v.x + m._11 * v.y + m._21 * v.z,
				m._02 * v.x + m._12 * v.y + m._22 * v.z
			);
		}

		inline Vector3 WeightedAverage2(
			const Vector3& p0,
			const Vector3& p1,
			const Scalar alpha,
			const Scalar beta )
		{
			return Vector3(	
				alpha*p0.x+ beta*p1.x,
				alpha*p0.y+ beta*p1.y,
				alpha*p0.z+ beta*p1.z );
		}

		inline Vector3 WeightedAverage3( 
			const Vector3& p0,
			const Vector3& p1,
			const Vector3& p2,
			const Scalar alpha,
			const Scalar beta,
			const Scalar gamma )
		{
			return Vector3(	
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y,
				alpha*p0.z+ beta*p1.z+ gamma*p2.z );
		}

		inline Vector3 WeightedAverage3(
			const Vector3& p0,
			const Vector3& p1,
			const Vector3& p2,
			const Scalar alpha,
			const Scalar beta )
		{
			const Scalar gamma = 1.0 - alpha - beta;
			return Vector3(	
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y,
				alpha*p0.z+ beta*p1.z+ gamma*p2.z );
		}

		inline void Serialize( 
			const Vector3& p, 
			IWriteBuffer& buffer )
		{
			buffer.ResizeForMore( sizeof( Scalar ) * 3 );
			buffer.setDouble( p.x );
			buffer.setDouble( p.y );
			buffer.setDouble( p.z );
		}

		inline void Deserialize( 
			Vector3& v,
			IReadBuffer& buffer )
		{
			v.x = buffer.getDouble();
			v.y = buffer.getDouble();
			v.z = buffer.getDouble();
		}
	}
}

#endif
