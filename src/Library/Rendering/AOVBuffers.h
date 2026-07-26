//////////////////////////////////////////////////////////////////////
//
//  AOVBuffers.h - Float-precision AOV (Arbitrary Output Variable)
//  buffers for denoiser and agent-perception input.  Storage is
//  channel-planned: callers pay only for the first-hit albedo,
//  world-space normal, and/or camera-distance planes they request.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: March 28, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef AOV_BUFFERS_H_
#define AOV_BUFFERS_H_

#include <atomic>
#include <vector>
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"

namespace RISE
{
	class IScene;
	class IRayCaster;

	/// First-hit AOV data extracted from a path sample.
	/// Used to populate denoiser and agent-perception auxiliary buffers.
	struct PixelAOV
	{
		RISEPel		albedo;
		Vector3		normal;
		Scalar		depth;
		bool		valid;		///< Albedo/normal surface is valid; depth is independent

		PixelAOV() : depth( 0 ), valid( false ) {}
	};

	namespace Implementation
	{
		/// Stores requested first-hit AOVs as float buffers.
		/// Thread-safe for concurrent writes when each pixel is
		/// written by exactly one thread (guaranteed by RISE's
		/// non-overlapping block dispatch).
		class AOVBuffers
		{
		public:
			struct Plan
			{
				bool albedo;
				bool normal;
				bool depth;

				Plan( bool albedo_ = true, bool normal_ = true, bool depth_ = false ) :
				  albedo( albedo_ ), normal( normal_ ), depth( depth_ ) {}

				bool Any() const { return albedo || normal || depth; }
			};

		private:
			unsigned int width;
			unsigned int height;
			std::atomic<bool> bHasData;		///< True once any sample has been accumulated
			Plan plan;
			std::vector<float> albedo;		///< width*height*3, RGB interleaved
			std::vector<float> normals;		///< width*height*3, XYZ interleaved
			std::vector<float> depths;		///< width*height, camera-ray hit distance

		public:
			AOVBuffers( unsigned int w, unsigned int h, const Plan& requested = Plan() );

			/// Clears existing contents for reuse. Requested planes preserve
			/// capacity when their dimensions are unchanged; disabled planes
			/// release capacity so a former consumer cannot pin their memory.
			void Reset( unsigned int w, unsigned int h, const Plan& requested = Plan() );

			/// Accumulates a weighted albedo sample at (x,y).
			/// The RISEPel channels (double) are narrowed to float.
			void AccumulateAlbedo(
				unsigned int x,
				unsigned int y,
				const RISEPel& c,
				Scalar weight
				);

			/// Accumulates a weighted normal sample at (x,y).
			/// The Vector3 components (double) are narrowed to float.
			void AccumulateNormal(
				unsigned int x,
				unsigned int y,
				const Vector3& n,
				Scalar weight
				);

			/// Accumulates a weighted camera-ray hit distance at (x,y).
			void AccumulateDepth(
				unsigned int x,
				unsigned int y,
				Scalar depth,
				Scalar weight
				);

			/// Divides every allocated AOV at (x,y) by the
			/// total weight to produce the final per-pixel average.
			void Normalize(
				unsigned int x,
				unsigned int y,
				Scalar invWeight
				);

			/// Drops the perception-only depth plane immediately after its
			/// consumer has compacted it.  OIDN may deliberately retain the
			/// albedo/normal planes for reuse, but must not pin depth capacity.
			void ReleaseDepthStorage();

			/// Returns true if any AOV data has been accumulated.
			bool HasData() const { return bHasData.load( std::memory_order_relaxed ); }
			const Plan& GetPlan() const { return plan; }

			const float* GetAlbedoPtr() const { return albedo.empty() ? 0 : albedo.data(); }
			const float* GetNormalPtr() const { return normals.empty() ? 0 : normals.data(); }
			const float* GetDepthPtr() const { return depths.empty() ? 0 : depths.data(); }
			size_t StorageBytes() const {
				return ( albedo.size() + normals.size() + depths.size() ) * sizeof( float );
			}
			size_t ReservedBytes() const {
				return ( albedo.capacity() + normals.capacity() + depths.capacity() ) * sizeof( float );
			}
			unsigned int GetWidth() const { return width; }
			unsigned int GetHeight() const { return height; }
		};

		/// Generic first-hit fallback for rasterizers that cannot attach an
		/// inline PixelAOV to their estimator (notably MLT and legacy shader
		/// pipelines).  This is deliberately independent of OIDN.
		void CollectFirstHitAOVs(
			const IScene& scene,
			IRayCaster& caster,
			AOVBuffers& aovBuffers,
			unsigned int samplesPerPixel = 1
			);
	}
}

#endif
