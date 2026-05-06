//////////////////////////////////////////////////////////////////////
//
//  Ray.h - Definition of a 3D ray class
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: October 20, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_
#define RAY_

#include "../Utilities/Math3D/Math3D.h"
#include "RayDifferentials.h"

namespace RISE
{
	class Ray
	{
		// Direction is private — must use SetDir() to change it,
		// which automatically recomputes invDir and sign.
		Vector3		m_dir;

		// Precompute inverse direction for branchless slab ray-box tests.
		// Uses a large finite value instead of infinity for near-zero
		// direction components to remain correct under -ffast-math.
		void RecomputeInvDir()
		{
			static const Scalar SAFE_INV = Scalar(1e30);
			invDir.x = (m_dir.x > 1e-30 || m_dir.x < -1e-30) ? Scalar(1.0) / m_dir.x : (m_dir.x >= 0 ? SAFE_INV : -SAFE_INV);
			invDir.y = (m_dir.y > 1e-30 || m_dir.y < -1e-30) ? Scalar(1.0) / m_dir.y : (m_dir.y >= 0 ? SAFE_INV : -SAFE_INV);
			invDir.z = (m_dir.z > 1e-30 || m_dir.z < -1e-30) ? Scalar(1.0) / m_dir.z : (m_dir.z >= 0 ? SAFE_INV : -SAFE_INV);
			sign[0] = (invDir.x < 0) ? 1 : 0;
			sign[1] = (invDir.y < 0) ? 1 : 0;
			sign[2] = (invDir.z < 0) ? 1 : 0;
		}

	public:
		Point3		origin;
		Vector3		invDir;		// precomputed 1.0/dir per component
		int			sign[3];	// 0 if dir >= 0, 1 if dir < 0

		// Landing 2: per-ray differentials for texture LOD selection
		// (Igehy 1999).  Embedded in Ray (PBRT / Mitsuba / Arnold
		// convention) — the +96 bytes per Ray is real cache pressure
		// on BVH traversal, but the parallel-struct alternative was
		// rejected as permanent tech debt.  hasDifferentials gates
		// the consumers; left false on shadow / NEE / BSDF / photon
		// rays so those don't pay the (small) read overhead.
		// Origin / direction offsets are ABSOLUTE offsets from the
		// central ray's origin / m_dir, not absolute world positions.
		RayDifferentials	diffs;
		bool				hasDifferentials;

		Ray( ) : hasDifferentials( false ) {}

		Ray( const Point3& p, const Vector3& d ) :
		m_dir( d ), origin( p ), hasDifferentials( false )
		{
			RecomputeInvDir();
		}

		Ray( const Ray& r ) :
		m_dir( r.m_dir ), origin( r.origin ), invDir( r.invDir ),
		diffs( r.diffs ), hasDifferentials( r.hasDifferentials )
		{
			sign[0] = r.sign[0];
			sign[1] = r.sign[1];
			sign[2] = r.sign[2];
		}

		// Read-only access to direction
		const Vector3&	Dir() const { return m_dir; }

		// Set direction — automatically recomputes invDir and sign
		void SetDir( const Vector3& d )
		{
			m_dir = d;
			RecomputeInvDir();
		}

		Point3		PointAtLength( const Scalar s ) const
		{
			return( Point3Ops::mkPoint3(origin, (m_dir * s)) );
		}

		void		Advance( const Scalar s )
		{
			origin = Point3Ops::mkPoint3(origin, (m_dir * s));
		}

		void		Set( const Point3& p, const Vector3& d )
		{
			origin = p;
			m_dir = d;
			RecomputeInvDir();
			// Conservative: any explicit Set wipes stale differentials
			// from a prior use of this Ray slot.  Callers who want
			// differentials must populate diffs and set hasDifferentials
			// = true AFTER the Set call.
			hasDifferentials = false;
		}

		void		Set( const Vector3& d, const Point3& p )
		{
			origin = p;
			m_dir = d;
			RecomputeInvDir();
			hasDifferentials = false;
		}

		Ray& operator=( const Ray& r )
		{
			origin = r.origin;
			m_dir = r.m_dir;
			invDir = r.invDir;
			sign[0] = r.sign[0];
			sign[1] = r.sign[1];
			sign[2] = r.sign[2];
			diffs = r.diffs;
			hasDifferentials = r.hasDifferentials;

			return *this;
		}

		static inline bool AreEqual( const Ray& a, const Ray& b, const Scalar epsilon )
		{
			return Point3Ops::AreEqual(a.origin, b.origin, epsilon ) &&
				Vector3Ops::AreEqual(a.m_dir, b.m_dir, epsilon );
		}
	};
}

#endif
