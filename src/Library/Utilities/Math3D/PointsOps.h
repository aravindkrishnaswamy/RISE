//////////////////////////////////////////////////////////////////////
//
//  PointsOps.h - Contains operations on points
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 6, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef POINTS_OPS_
#define POINTS_OPS_

#include "../../Interfaces/IWriteBuffer.h"
#include "../../Interfaces/IReadBuffer.h"

namespace RISE
{
	namespace Point2Ops
	{
		inline Point2 mkPoint2( const Point2& p, const Vector2& v )
		{
			return Point2( p.x+v.x, p.y+v.y );
		}

		inline bool AreEqual( const Point2& a, const Point2& b, const Scalar epsilon )
		{
			return( fabs(a.x-b.x) <= epsilon &&
					fabs(a.y-b.y) <= epsilon );
		}

		// Computes the distance between two points
		inline Scalar Distance( const Point2& a, const Point2& b )
		{
			return Vector2Ops::Magnitude( Vector2Ops::mkVector2( b, a ) );
		}

		// Matrix transforms a point
		inline Point2 Transform( const Matrix3& m, const Point2& p )
		{
			return Point2(
					m._00 * p.x + m._10 * p.y + m._20,
					m._01 * p.x + m._11 * p.y + m._21
			);
		}

		inline Point2 WeightedAverage2(
			const Point2& p0,
			const Point2& p1,
			const Scalar alpha )
		{
			const Scalar beta = 1.0 - alpha;
			return Point2(
				alpha*p0.x + beta*p1.x, 
				alpha*p0.y + beta*p1.y );
		}

		inline Point2 WeightedAverage3(
			const Point2& p0,
			const Point2& p1,
			const Point2& p2,
			const Scalar alpha,
			const Scalar beta )
		{
			const Scalar gamma = 1.0 - alpha - beta;
			return Point2(
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y );
		}

		inline Point2 WeightedAverage3(
			const Point2& p0,
			const Point2& p1,
			const Point2& p2,
			const Scalar alpha,
			const Scalar beta,
			const Scalar gamma )
		{
			return Point2(
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y );
		}

		inline void Serialize( 
			const Point2& p, 
			IWriteBuffer& buffer )
		{
			buffer.ResizeForMore( sizeof( Scalar ) * 2 );
			buffer.setDouble( p.x );
			buffer.setDouble( p.y );
		}

		inline void Deserialize( 
			Point2& p,
			IReadBuffer& buffer )
		{
			p.x = buffer.getDouble();
			p.y = buffer.getDouble();
		}
	}

	namespace Point3Ops
	{
		inline Point3 mkPoint3( const Point3& p, const Vector3& v )
		{
			return Point3( p.x+v.x, p.y+v.y, p.z+v.z );
		}

		inline bool AreEqual( const Point3& a, const Point3& b, const Scalar epsilon )
		{
			return( fabs(a.x-b.x) <= epsilon &&
					fabs(a.y-b.y) <= epsilon &&
					fabs(a.z-b.z) <= epsilon );
		}

		// Computes the distance between two points
		inline Scalar Distance( const Point3& a, const Point3& b )
		{
			return Vector3Ops::Magnitude( Vector3Ops::mkVector3( b, a ) );
		}

		// Matrix transforms a point
		inline Point3 Transform( const Matrix4& m, const Point3& p )
		{
			return Point3(
				m._00 * p.x + m._10 * p.y + m._20 * p.z + m._30,
				m._01 * p.x + m._11 * p.y + m._21 * p.z + m._31,
				m._02 * p.x + m._12 * p.y + m._22 * p.z + m._32
			);
		}

		inline Point3 WeightedAverage2(
			const Point3& p0,
			const Point3& p1,
			const Scalar alpha )
		{
			const Scalar beta = 1.0 - alpha;
			return Point3(
				alpha*p0.x + beta*p1.x, 
				alpha*p0.y + beta*p1.y, 
				alpha*p0.z + beta*p1.z );
		}

		inline Point3 WeightedAverage3( 
			const Point3& p0,
			const Point3& p1,
			const Point3& p2,
			Scalar alpha,
			Scalar beta )
		{
			const Scalar gamma = 1.0 - alpha - beta;
			return Point3(	
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y,
				alpha*p0.z+ beta*p1.z+ gamma*p2.z );
		}

		inline Point3 WeightedAverage3( 
			const Point3& p0,
			const Point3& p1,
			const Point3& p2,
			Scalar alpha,
			Scalar beta,
			Scalar gamma
			)
		{
			return Point3(	
				alpha*p0.x+ beta*p1.x+ gamma*p2.x,
				alpha*p0.y+ beta*p1.y+ gamma*p2.y,
				alpha*p0.z+ beta*p1.z+ gamma*p2.z );
		}


		inline void Serialize( 
			const Point3& p, 
			IWriteBuffer& buffer )
		{
			buffer.ResizeForMore( sizeof( Scalar ) * 3 );
			buffer.setDouble( p.x );
			buffer.setDouble( p.y );
			buffer.setDouble( p.z );
		}

		inline void Deserialize(
			Point3& p,
			IReadBuffer& buffer )
		{
			p.x = buffer.getDouble();
			p.y = buffer.getDouble();
			p.z = buffer.getDouble();
		}
	}
}

#endif


