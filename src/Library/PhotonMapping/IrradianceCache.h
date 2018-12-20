//////////////////////////////////////////////////////////////////////
//
//  IrradianceCache.h - An irradiance cache is a cache that stores
//  the diffuse interreflection values with a coarse discretization
//  in a scene.
//
//  The cache uses an octree internally.  This code is based off
//  the paper by Gregory J. Ward et al.  titled
//  "A Ray Tracing Solution for Diffuse Interreflection"
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 28, 2002
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IRRADIANCE_CACHE_
#define IRRADIANCE_CACHE_

#include "../Interfaces/IIrradianceCache.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/BoundingBox.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/Threads/Threads.h"
#include <vector>

#ifdef _DEBUG
#include <assert.h>
#endif

namespace RISE
{
	namespace Implementation
	{
		//! Implementation of the irradiance cache
		class IrradianceCache :
			public virtual IIrradianceCache,
			public virtual Implementation::Reference
		{
		protected:
			//
			// This represents a cache node which is the internal octree
			// for the irradiance cache
			//
			class CacheNode
			{
			protected:
				Point3		ptCenter;					// The absolute center of the node in world space
				Scalar		dSize;						// The size of the node (from edge to an edge)
				Scalar		dHalfSize;					// The size of the node from center to edge

				std::vector<CacheElement>	values;		// The irradiance values for this node

				CacheNode*	pChildren[8];				// The eight children of this node

				// This function given a point will locate the child node that contains that point
				unsigned char WhichNode( const Point3& ptPosition );

			public:
				CacheNode( const Scalar size, const Point3& center );
				virtual ~CacheNode( );

				//
				// These are the useful cache node functions
				//

				//! Inserts a cache element into the tree starting at this node
				void InsertElement( const CacheElement& elem, const Scalar tolerance );

				//! Queries the cache node for all possible elements
				Scalar Query(
					const Point3& ptPosition,
					const Vector3& vNormal,
					std::vector<CacheElement>& results,
					const Scalar invTolerance
					) const;

				//! Asks the cache is a sample is need at the current position
				bool IsSampleNeeded(
					const Point3& ptPosition,
					const Vector3& vNormal,
					const Scalar invTolerance
					) const;

				void Clear();
			};

			CacheNode		root;
			Scalar			tolerance;
			Scalar			invTolerance;
			Scalar			min_spacing;
			Scalar			max_spacing;

			mutable RMutexReadWrite	mutex;				// Mutex to control access

			bool			bPreComputed;				// Has the cache been pre-computed?

			virtual ~IrradianceCache( );

		public:
			IrradianceCache( const Scalar size, const Scalar tol, const Scalar min, const Scalar max );

			void InsertElement(
				const Point3&		ptPosition,
				const Vector3&		vNormal,
				const RISEPel&		cIRad,
				const Scalar		r0_,
				const RISEPel*		rot,
				const RISEPel*		trans
				)
			{
				#ifdef _DEBUG
				assert( !bPreComputed );
				#endif

				Scalar r0 = r0_;

				if( r0 * tolerance < min_spacing ) {
					r0 = min_spacing/tolerance;
				}

				if( r0 * tolerance > max_spacing ) {
					r0 = max_spacing/tolerance;
				}

				mutex.write_lock();
				root.InsertElement( CacheElement(ptPosition, vNormal, cIRad, r0, 0, rot, trans), tolerance );
				mutex.write_unlock();
			}

			// Queries don't require the lock because we query only, there will be insertions (that is done in a prepass)
			Scalar Query( const Point3& ptPosition, const Vector3& vNormal, std::vector<CacheElement>& results ) const
			{
				#ifdef _DEBUG
				assert( bPreComputed );
				#endif
				return root.Query( ptPosition, vNormal, results, invTolerance );
			}

			bool IsSampleNeeded( const Point3& ptPosition, const Vector3& vNormal ) const
			{
				mutex.read_lock();
				bool bSampleNeeded = root.IsSampleNeeded( ptPosition, vNormal, invTolerance );
				mutex.read_unlock();

				return bSampleNeeded;
			}

			inline Scalar GetTolerance(){ return tolerance; };
			inline void Clear()
			{
				root.Clear();
				bPreComputed = false;
			};

			void FinishedPrecomputation()
			{
				#ifdef _DEBUG
				assert( !bPreComputed );
				#endif
				bPreComputed = true;
			}

			bool Precomputed()
			{
				return bPreComputed;
			}
		};

	}
}

#endif
