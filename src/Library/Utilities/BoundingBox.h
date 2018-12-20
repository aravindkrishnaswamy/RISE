//////////////////////////////////////////////////////////////////////
//
//  BoundingBox.h - Definition of an axis aligned bounding boc
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 27, 2003
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef BOUNDING_BOX_
#define BOUNDING_BOX_

#include "Math3D/Math3D.h"
#include "../Interfaces/ISerializable.h"

namespace RISE
{
	class BoundingBox : public ISerializable
	{
	public:
		Point3	ll;				// The lower left of the bounding box
		Point3	ur;				// The upper right of the bounding box

	public:
		BoundingBox()
		{
			ll.x = ll.y = ll.z = -INFINITY;
			ur.x = ur.y = ur.z = INFINITY;
		}

		BoundingBox( const Point3& ll_, const Point3& ur_ ) : 
		ll( ll_ ), ur( ur_ )
		{
		}

		Vector3 GetExtents() const
		{
			return Vector3Ops::mkVector3( ur, ll );
		}

		Point3 GetCenter() const
		{
			return Point3((ll.x+ur.x)*0.5, (ll.y+ur.y)*0.5, (ll.z+ur.z)*0.5 );
		}

		bool DoIntersect( const BoundingBox& other ) const
		{
			const Vector3& T = Vector3Ops::mkVector3( GetCenter(), other.GetCenter() );

			const Vector3 Ea = GetExtents() * 0.5;
			const Vector3 Eb = other.GetExtents() * 0.5;

			return (
				fabs(T.x) <= (Ea.x + Eb.x) &&
				fabs(T.y) <= (Ea.y + Eb.y) &&
				fabs(T.z) <= (Ea.z + Eb.z)
				);
		
			/*
			if( (ll.x <= other.ur.x-1e-6 &&
				ur.x >= other.ll.x+1e-6) && 
				(ll.y <= other.ur.y-1e-6 &&
				ur.y >= other.ll.y+1e-6) && 
				(ll.z <= other.ur.z-1e-6 &&		
				ur.z >= other.ll.z+1e-6) ) {
					return true;
				}
			return false;
			*/
		}

		void Include( const Point3& p )
		{
			if( p.x < ll.x ) {
				ll.x = p.x;
			}
			if( p.x > ur.x ) {
				ur.x = p.x;
			}

			if( p.y < ll.y ) {
				ll.y = p.y;
			}
			if( p.y > ur.y ) {
				ur.y = p.y;
			}

			if( p.z < ll.z ) {
				ll.z = p.z;
			}
			if( p.z > ur.z ) {
				ur.z = p.z;
			}
		}

		void Include( const BoundingBox& other )
		{
			if( other.ll.x < ll.x ) {
				ll.x = other.ll.x;
			}

			if( other.ll.y < ll.y ) {
				ll.y = other.ll.y;
			}

			if( other.ll.z < ll.z ) {
				ll.z = other.ll.z;
			}

			if( other.ur.x > ur.x ) {
				ur.x = other.ur.x;
			}

			if( other.ur.y > ur.y ) {
				ur.y = other.ur.y;
			}

			if( other.ur.z > ur.z ) {
				ur.z = other.ur.z;
			}
		}

		void SanityCheck()
		{
			if( ll.x > ur.x ) {
				Scalar t = ur.x;
				ur.x = ll.x;
				ll.x = t;
			}

			if( ll.y > ur.y ) {
				Scalar t = ur.y;
				ur.y = ll.y;
				ll.y = t;
			}

			if( ll.z > ur.z ) {
				Scalar t = ur.z;
				ur.z = ll.z;
				ll.z = t;
			}
		}

		void EnsureBoxHasVolume()
		{
			if( ll.x == ur.x ) {
				ll.x -= NEARZERO;
				ur.x += NEARZERO;
			}

			if( ll.y == ur.y ) {
				ll.y -= NEARZERO;
				ur.y += NEARZERO;
			}

			if( ll.z == ur.z ) {
				ll.z -= NEARZERO;
				ur.z += NEARZERO;
			}
		}

		inline void Grow( const Scalar s )
		{
			ll = Point3Ops::mkPoint3( ll, Vector3Ops::Normalize(Vector3Ops::mkVector3(ll,GetCenter()))*s );
			ur = Point3Ops::mkPoint3( ur, Vector3Ops::Normalize(Vector3Ops::mkVector3(ur,GetCenter()))*s );
		}

   		void Serialize( 
			IWriteBuffer&			buffer					///< [in] Buffer to serialize to
			) const
		{
			Point3Ops::Serialize( ll, buffer );
			Point3Ops::Serialize( ur, buffer );
		}

		void Deserialize(
			IReadBuffer&			buffer					///< [in] Buffer to deserialize from
			)
		{
			Point3Ops::Deserialize( ll, buffer );
			Point3Ops::Deserialize( ur, buffer );
		}
	};
}

#endif
