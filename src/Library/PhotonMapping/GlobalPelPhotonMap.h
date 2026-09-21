//////////////////////////////////////////////////////////////////////
//
//  GlobalPelPhotonMap.h - Definition of the global pel photon map class.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: June 14, 2003
//  Tabs: 4
//  Comments:  The code here is an implementation from Henrik Wann
//             Jensen's book Realistic Image Synthesis Using 
//             Photon Mapping.  Much of the code is influeced or
//             taken from the sample code in the back of his book.
//			   I have however used STD data structures rather than
//			   reinventing the wheel as he does.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef GLOBAL_PEL_PHOTON_MAP_
#define GLOBAL_PEL_PHOTON_MAP_

#include "PhotonMap.h"

namespace RISE
{
	namespace Implementation
	{
		class GlobalPelPhotonMap : 
			public PhotonMapDirectionalPelHelper<IrradPhoton>
		{		
		protected:
			struct CacheAnchor
			{
				Point3 position;
				Vector3 geometricNormal;
				unsigned char plane;
			};
			std::vector<CacheAnchor> anchors;
			unsigned int anchorSpacing;
			bool hasGeometricNormals;

			void BalanceAnchors( int from, int to, BoundingBox bounds );
			void FindAnchor( const Point3& point, const Vector3& normal,
				int from, int to, Scalar& distance, const CacheAnchor*& nearest ) const;
			const CacheAnchor* FindAnchor( const Point3& point, const Vector3& normal ) const;
			void RadianceAtAnchor( RISEPel& rad, const CacheAnchor& anchor,
				const RayIntersectionGeometric& query, const IBSDF& bsdf ) const;

		public:
			GlobalPelPhotonMap( 
				const unsigned int max_photons,
				const IPhotonTracer* tracer
				);
			virtual ~GlobalPelPhotonMap( );	

			void PrecomputeIrradiance( 
				const unsigned int apart,						// How far apart should precomputed irradiances be?
				IProgressCallback* pFunc						// Progress callback
				);

			void RadianceEstimate( 
				RISEPel&						rad,					// returned radiance
				const RayIntersectionGeometric&	ri,						// ray-surface intersection information
				const IBSDF&					brdf					// BRDF of the surface to estimate irradiance from
				) const;

			bool Store( const RISEPel& power, const Point3& pos, const Vector3& N, const Vector3& dir );

			void SetGatherParams( const Scalar radius, const Scalar ellipse_ratio,
				const unsigned int nminphotons, const unsigned int nmaxphotons,
				IProgressCallback* pFunc );

			// Loading is transactional. Legacy full directional maps can be
			// gathered directly; legacy scalar caches cannot be reconstructed.
			bool DeserializeChecked( IReadBuffer& buffer );

			void Serialize( 
				IWriteBuffer&			buffer					///< [in] Buffer to serialize to
				) const;

			void Deserialize(
				IReadBuffer&			buffer					///< [in] Buffer to deserialize from
				);
		};
	}
}

#endif
