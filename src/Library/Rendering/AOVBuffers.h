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
#include "../Interfaces/IRasterImage.h"
#include "../Utilities/Math3D/Math3D.h"
#include "../Utilities/Color/Color.h"
#include "../Utilities/OidnConfig.h"

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
		bool		primaryDepthCaptured;	///< Raw camera intersection (including a miss) was examined

		PixelAOV() : depth( 0 ), valid( false ), primaryDepthCaptured( false ) {}
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
			std::atomic<bool> bHasAlbedoData;
			std::atomic<bool> bHasNormalData;
			std::atomic<bool> bHasDepthData;
			Plan plan;
			std::vector<float> albedo;		///< width*height*3, RGB interleaved
			std::vector<float> normals;		///< width*height*3, XYZ interleaved
			std::vector<float> depths;		///< width*height, camera-ray hit distance
			std::vector<float> depthWeights;	///< width*height, weight of hit samples only

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

			/// Records that the primary camera intersection was examined and,
			/// for a valid hit, accumulates its weighted distance at (x,y).
			/// Misses (depth <= 0) deliberately contribute no depth weight: a
			/// silhouette pixel is the mean distance of its hit samples, not a
			/// hit distance diluted toward zero by background samples.
			void AccumulateDepth(
				unsigned int x,
				unsigned int y,
				Scalar depth,
				Scalar weight
				);

			/// Marks allocated albedo/normal planes as handled by an inline
			/// producer even when a camera sample misses or an Accurate trace
			/// finds no non-delta surface. This prevents a redundant whole-frame
			/// fallback on empty/all-delta scenes without inventing zero samples.
			void MarkGuidesExamined();

			/// Finalizes every allocated AOV at (x,y). Albedo and normal use
			/// the supplied inverse total sample weight; depth uses its own
			/// accumulated hit-only weight so background misses do not dilute
			/// silhouette distances.
			void Normalize(
				unsigned int x,
				unsigned int y,
				Scalar invWeight
				);

			/// Normalizes only the selected planes. Used by the bounded
			/// first-hit completion pass so already-normalized inline planes
			/// are never scaled a second time.
			void NormalizeSelected(
				unsigned int x,
				unsigned int y,
				Scalar invWeight,
				const Plan& selected
				);

			/// Drops the perception-only depth scratch after it has been copied
			/// into FrameStore. OIDN may deliberately retain albedo/normal for
			/// reuse, but must not pin depth capacity through later output work.
			void ReleaseDepthStorage();

			/// Per-plane readiness is deliberately independent: primary depth
			/// must not make absent OIDN albedo/normal guides look complete.
			bool HasAlbedoData() const { return bHasAlbedoData.load( std::memory_order_relaxed ); }
			bool HasNormalData() const { return bHasNormalData.load( std::memory_order_relaxed ); }
			bool HasDepthData() const { return bHasDepthData.load( std::memory_order_relaxed ); }
			bool HasData() const { return HasAlbedoData() || HasNormalData() || HasDepthData(); }
			Plan MissingPlan() const {
				return Plan( plan.albedo && !HasAlbedoData(),
					plan.normal && !HasNormalData(),
					plan.depth && !HasDepthData() );
			}
			bool NeedsFallback() const { return MissingPlan().Any(); }
			const Plan& GetPlan() const { return plan; }

			const float* GetAlbedoPtr() const { return albedo.empty() ? 0 : albedo.data(); }
			const float* GetNormalPtr() const { return normals.empty() ? 0 : normals.data(); }
			const float* GetDepthPtr() const { return depths.empty() ? 0 : depths.data(); }
			size_t StorageBytes() const {
				return ( albedo.size() + normals.size() + depths.size()
					+ depthWeights.size() ) * sizeof( float );
			}
			size_t ReservedBytes() const {
				return ( albedo.capacity() + normals.capacity() + depths.capacity()
					+ depthWeights.capacity() ) * sizeof( float );
			}
			unsigned int GetWidth() const { return width; }
			unsigned int GetHeight() const { return height; }
		};

		// Successful OIDN renders may deliberately keep their albedo/normal
		// buffers warm on the owning rasterizer. A failed render has no such
		// cache contract: arm this guard at render entry and dismiss it only
		// after the normal tail has retained or released the buffers.
		class AOVBufferUnwindGuard
		{
			AOVBuffers*& target;
			bool armed;

			AOVBufferUnwindGuard( const AOVBufferUnwindGuard& ) = delete;
			AOVBufferUnwindGuard& operator=( const AOVBufferUnwindGuard& ) = delete;

		public:
			explicit AOVBufferUnwindGuard( AOVBuffers*& target_ ) noexcept :
				target( target_ ), armed( true ) {}
			~AOVBufferUnwindGuard()
			{
				if( armed ) {
					delete target;
					target = 0;
				}
			}
			void Dismiss() noexcept { armed = false; }
		};

		/// Generic first-hit fallback for rasterizers that cannot attach an
		/// inline PixelAOV to their estimator (notably MLT and legacy shader
		/// pipelines). Accurate guide discovery relies on a path-tracing-capable
		/// shader chain; custom terminal chains may leave those guides invalid.
		/// This is deliberately independent of OIDN.
		void CollectFirstHitAOVs(
			const IScene& scene,
			IRayCaster& caster,
			AOVBuffers& aovBuffers,
			unsigned int samplesPerPixel = 1,
			OidnPrefilter prefilterMode = OidnPrefilter::Fast,
			const Rect* region = 0
			);

		/// Row-subset form used by interlaced animation fallback. `selected`
		/// is captured before either field is completed and deliberately does
		/// not re-query global plane readiness between parities.
		void CollectFirstHitAOVRows(
			const IScene& scene,
			IRayCaster& caster,
			AOVBuffers& aovBuffers,
			const AOVBuffers::Plan& selected,
			unsigned int firstRow,
			unsigned int rowStride,
			unsigned int samplesPerPixel = 1,
			OidnPrefilter prefilterMode = OidnPrefilter::Fast,
			const Rect* region = 0
			);
	}
}

#endif
