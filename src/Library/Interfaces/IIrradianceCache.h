//////////////////////////////////////////////////////////////////////
//
//  IIrradianceCache.h - Interface to the irradiance cache
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: December 1, 2003
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IIRRADIANCE_CACHE_
#define IIRRADIANCE_CACHE_

#include "IReference.h"
#include "../Utilities/Color/Color.h"
#include <vector>

namespace RISE
{
	class IIrradianceCache : public virtual IReference
	{
	public:
		//
		// This presents a cache element. Every element in the cache has:
		//
		// position
		// normal
		// irradiant energy
		//
		class CacheElement
		{
		public:
			Point3		ptPosition;
			Vector3		vNormal;
			RISEPel		cIRad;
			Scalar		r0;
			Scalar		dWeight;
			RISEPel		rotationalGradient[3];
			RISEPel		translationalGradient[3];

			CacheElement( const Point3& pos, const Vector3& norm, const RISEPel& rad, const Scalar r0, const Scalar weight, const RISEPel* rot, const RISEPel* tran );
			CacheElement( const CacheElement& ce, const Scalar weight );
			virtual ~CacheElement( );

			Scalar ComputeWeight( const Point3& pos, const Vector3& norm ) const;
		};


		virtual void InsertElement(
			const Point3&		ptPosition,
			const Vector3&		vNormal,
			const RISEPel&		cIRad,
			const Scalar		invR0,
			const RISEPel*		rot,
			const RISEPel*		trans
			) = 0;

		virtual Scalar Query( const Point3& ptPosition, const Vector3& vNormal, std::vector<CacheElement>& results ) const = 0;
		virtual bool IsSampleNeeded( const Point3& ptPosition, const Vector3& vNormal ) const = 0;
		virtual Scalar GetTolerance() = 0;
		virtual void Clear() = 0;
		virtual bool Precomputed() = 0;
		virtual void FinishedPrecomputation() = 0;
	};
}

#endif
