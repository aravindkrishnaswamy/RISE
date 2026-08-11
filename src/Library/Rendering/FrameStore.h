//////////////////////////////////////////////////////////////////////
//
//  FrameStore.h - Canonical HDR buffer for the FrameStore output
//  redesign.
//
//  Holds RISEPel-precision linear beauty + optional AOV channels
//  (albedo, normal, depth, etc.).  Tile-level shared-mutex concurrency
//  lets the rasterizer write and the UI / encoders read concurrently.
//  Render() is the polymorphic readback that
//  applies a ViewTransform and produces target-format bytes for
//  display surfaces and file encoders.
//
//  Reference convention: FrameStore inherits from
//  RISE::Implementation::Reference.  Construct with `new FrameStore(spec)`
//  (refcount starts at 1 — Reference's default — so the `new` call
//  hands the caller the initial ref; subsequent sharers `addref()`,
//  and `safe_release()` drops a ref + null-clears the pointer).
//  Verified by tests/FrameStoreTest.cpp:619.  This matches the rest
//  of the library (see RasterImage_Template, FilteredFilm, etc.).
//
//  Observer convention: observers attach to the FrameStore, NOT to
//  the rasterizer.  See IRenderObserver.h and
//  docs/FRAMESTORE_DESIGN.md §7.5.
//
//  See docs/FRAMESTORE_DESIGN.md for the full design rationale.
//
//  Author: design landing L1
//  License: see LICENSE.TXT
//
//////////////////////////////////////////////////////////////////////

#ifndef FRAMESTORE_
#define FRAMESTORE_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "../Interfaces/IRasterImage.h"
#include "../Utilities/Reference.h"
#include "../Utilities/Threads/Threads.h"

#include "Channel.h"
#include "AOVBuffers.h"
#include "FrameStoreColorSpace.h"
#include "TargetFormat.h"
#include "ViewTransform.h"

namespace RISE
{
	// Forward declaration; full definition in IRenderObserver.h.
	class IRenderObserver;

	namespace Implementation
	{
		class FrameStore;  // forward for the back-compat shim below
		class FrameStoreBulkBracket;
		class PixelBasedRasterizerHelper;
		class Rasterizer;
		class ViewportFrameStore;
	}

	namespace FrameStoreOutput
	{
		struct ActiveFireMedium
		{
			std::string mediaKind;             ///< "static_authored" in Phase A
			std::string managerName;
			std::string bindingKind;
			std::string bindingOwner;
			std::string authoredConfigDigest;  ///< canonical resolved authoring digest
			std::vector<std::string> opticalRecordIds;
		};

		//! Bookkeeping carried through the render pipeline.  Producers
		//! (rasterizers) populate fields they know about; consumers
		//! (encoders, UI) read what they need and ignore the rest.
		//! Empty strings + zero counters are the defaults; "missing"
		//! is conveyed by leaving the field at its default.
		struct Metadata
		{
			std::string sceneName;             ///< source .RISEscene file stem
			std::string cameraName;            ///< active camera (when known)
			std::string activeRasterizer;      ///< "pathtracing_pel_rasterizer" etc.
			uint64_t    sampleCount  = 0;      ///< samples-per-pixel converged so far
			double      cameraExposureEV = 0.0;///< from ICamera::GetExposureCompensationEV
			unsigned    frame        = 0;      ///< animation frame index
			bool        denoisedContent = false; ///< current beauty pixels passed through denoising
			std::string renderFidelityStatus;  ///< empty for non-fire; otherwise derived status
			std::vector<std::string> renderReasonCodes; ///< sorted unique schema-v1 fire reasons
			std::vector<std::string> activeFireOpticsRecordIds; ///< sorted unique exact-byte IDs
			std::vector<ActiveFireMedium> activeFireMedia; ///< binding-keyed tagged provenance
			std::vector<unsigned char> resolvedRenderConfigCoreV1; ///< canonical schema-v1 core
			std::vector<unsigned char> rendererBuildV1; ///< canonical renderer_build_v1 bytes
			std::string rendererBuildId; ///< SHA-256(rendererBuildV1)
			std::string primaryProvenanceId; ///< last finalized primary for derivative linkage
			std::string primaryArtifactSha256;
			std::string primaryArtifactFidelity;
		};

		//! Fixed schema-v1 fire render reason-code membership.
		bool IsAllowedFireRenderReasonCode( const std::string& reason );

		//! Validates the complete semantic fire metadata envelope before render
		//! authorization or artifact publication.  Empty renderFidelityStatus is
		//! non-fire metadata and is intentionally rejected by this function.
		bool ValidateFireOutputMetadata(
			const Metadata& metadata,
			std::string& error
			);

		//! Construction parameters for FrameStore.  Pass to the
		//! FrameStore constructor.
		struct FrameStoreSpec
		{
			size_t width  = 0;
			size_t height = 0;

			//! Tile edge length in pixels.  Should match the
			//! rasterizer's tile size to align the lock with
			//! the actual write granularity.  32 is the RISE default.
			size_t tileEdge = 32;

			//! Optional AOV channels to allocate beyond Beauty + Alpha.
			//! Listing a ChannelId here causes the FrameStore to allocate
			//! storage for that channel; omitting it leaves the channel
			//! absent (HasChannel returns false; GetChannel returns nullptr).
			//! Beauty and Alpha are always allocated.
			std::vector<ChannelId> aovChannels;

			Metadata meta;
		};
	}

	namespace Implementation
	{
		//! Canonical HDR frame buffer.  Multiple readers (UI repaint,
		//! file encoders, network mirror) and one writer (the
		//! rasterizer or its FrameSink adapter) communicate through
		//! the tile-level shared-mutex protocol.
		//!
		//! Lifetime: managed via Reference / addref / release.
		//! Construction examples in tests/FrameStoreTest.cpp.
		class FrameStore : public virtual Reference
		{
		public:
			using Spec        = FrameStoreOutput::FrameStoreSpec;
			using Metadata    = FrameStoreOutput::Metadata;
			using ChannelId   = FrameStoreOutput::ChannelId;
			using TargetFormat = FrameStoreOutput::TargetFormat;
			using ViewTransform = FrameStoreOutput::ViewTransform;

			struct Snapshot
			{
				Metadata metadata;
				std::vector<RISEPel> beauty;
				std::vector<Chel> alpha;
				std::vector<RISEPel> albedo;
				std::vector<Vector3> normal;
				std::vector<float> depth;
				std::vector<uint32_t> objectId;
				std::vector<uint32_t> primitiveId;
			};

			explicit FrameStore( const Spec& spec );

			// ── geometry ──────────────────────────────────────────

			size_t Width()       const { return width_; }
			size_t Height()      const { return height_; }
			size_t TileEdge()    const { return tileEdge_; }
			size_t TileCountX()  const { return tileCountX_; }
			size_t TileCountY()  const { return tileCountY_; }

			// ── channel presence ──────────────────────────────────

			//! Runtime channel-presence query.  True iff storage is
			//! allocated for `id` (Beauty + Alpha are always true;
			//! AOVs depend on Spec.aovChannels).
			bool HasChannel( ChannelId id ) const
			{
				return id < ChannelId::COUNT
				    && presence_[ static_cast<uint32_t>( id ) ];
			}

			//! Compile-time channel access.  Returns nullptr if the
			//! channel was not allocated (e.g. unrequested AOV).
			//! Because the type is determined at compile time by
			//! ChannelTraits, callers can't request a channel as
			//! the wrong element type.
			template <FrameStoreOutput::ChannelId C>
			FrameStoreOutput::Channel<FrameStoreOutput::ChannelType<C>>* GetChannel();

			template <FrameStoreOutput::ChannelId C>
			const FrameStoreOutput::Channel<FrameStoreOutput::ChannelType<C>>* GetChannel() const;

			// ── write-side API (rasterizer / FrameSink) ───────────

			//! Begin a tile write.  Takes the tile's exclusive lock;
			//! readers block until the matching EndTile.
			//! Tile coordinates are in tile units (not pixel units).
			void BeginTile( size_t tileX, size_t tileY );

			//! End a tile write.  Releases the exclusive lock and bumps
			//! the global generation counter; observers'
			//! OnTileComplete fires here.
			void EndTile( size_t tileX, size_t tileY );

			//! Bulk copy the beauty channel from a tile-aligned
			//! sub-rectangle of an IRasterImage.  Used by the Phase 1
			//! FrameSink to ingest data from the existing
			//! IRasterizerOutput push API.  Asserts srcRect aligns
			//! with the FrameStore's tile grid; the tile lock is
			//! managed internally (BeginTile/EndTile bracketing
			//! happen inside).
			void CopyTileFromRasterImage( size_t tileX, size_t tileY,
			                              const IRasterImage& src,
			                              const Rect& srcRect );

			//! Mark the current frame complete.  Bumps the global
			//! generation counter and fires observers'
			//! OnFrameComplete.  Should be called by the rasterizer
			//! (or its FrameSink) at end of pass.
			void MarkFrameComplete( unsigned frame );

			//! Hooks for the OIDN denoiser pre/post pass.  See
			//! IRenderObserver.h for semantics.
			void MarkPreDenoiseComplete( unsigned frame );
			void MarkDenoiseComplete( unsigned frame );

			// ── observer registration ─────────────────────────────

			//! Attach a render observer.  The observer must outlive
			//! the FrameStore, OR call RemoveObserver before
			//! self-destruction.  Caller retains ownership; the
			//! FrameStore stores a non-owning pointer (consistent
			//! with IRasterizer::AddRasterizerOutput).
			void AddObserver( IRenderObserver* observer );

			//! Detach a render observer.  Safe to call if the
			//! observer was never attached (silent no-op).
			void RemoveObserver( IRenderObserver* observer );

			// ── read-side API (UI, encoders) ──────────────────────

			//! Monotonically-increasing frame-store generation.
			//! Bumped on every tile commit and on frame complete.
			//! UIs gate "needs repaint" on `Generation() != lastSeen`.
			uint64_t Generation() const
			{
				return globalGeneration_.load( std::memory_order_acquire );
			}

			//! Polymorphic readback.  Walks `roi` of the frame,
			//! applies the ViewTransform pipeline (exposure, white
			//! balance, primaries, tone curve), then encodes in the
			//! target format's pixel layout into `dst`.
			//!
			//! `dst` points to the top-left of the OUTPUT buffer for
			//! `roi`; rows are `dstStride` BYTES apart (not pixels).
			//! `roi` may extend up to (Width(), Height()).
			//!
			//! Thread-safe: per-tile shared_mutex means concurrent reads
			//! and writes don't tear.  If a tile is being written at
			//! the moment of read, this function BLOCKS until the
			//! writer releases the tile's exclusive lock.
			//!
			//! Tone curve is applied iff GetTargetFormatInfo(fmt).isLDRFixed
			//! AND xform.toneCurve != None.  HDR float targets always
			//! skip the tone curve (the display compositor handles it).
			//!
			//! L8 round 14 — `nonBlocking` parameter.
			//!   * `false` (default): take each tile's `shared_lock`
			//!     in blocking mode.  Concurrent writers cause this
			//!     call to wait for the entire writer's exclusive
			//!     hold window — fine for end-of-render encoders
			//!     (file outputs, Save-As) that need a complete
			//!     coherent snapshot.
			//!   * `true`: use `try_lock_shared` per tile.  If a
			//!     writer holds exclusive, SKIP the tile — its
			//!     pixels in `dst` remain at whatever value the
			//!     caller initialised them to (typically the
			//!     previous emit's bytes).  Used by the platform
			//!     bridges' 30 Hz progressive-update poll so a slow
			//!     per-pixel block can't beachball the bridge: the
			//!     poll returns fast with whatever was readable.
			//!     Subsequent polls catch contended tiles when
			//!     they're free (between worker flushes — see
			//!     `PixelBasedRasterizerHelper`'s 100 ms intra-block
			//!     flush, round 11).
			void Render( void*                 dst,
			             size_t                dstStride,
			             const Rect&           roi,
			             FrameStoreOutput::TargetFormat fmt,
			             const FrameStoreOutput::ViewTransform& xform,
			             bool                  nonBlocking = false ) const;

			// ── metadata ──────────────────────────────────────────

			Metadata Meta() const
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				Metadata snapshot = meta_;
				snapshot.frame = completedFrame_.load(std::memory_order_relaxed);
				return snapshot;
			}

			void SetMetadata( const Metadata& metadata )
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				if( outputMetadataLeaseCount_ ) {
					throw std::runtime_error(
						"output_provenance_unavailable: output metadata is leased");
				}
				meta_ = metadata;
				fireRenderPublicationBlocked_ = false;
				completedFrame_.store(metadata.frame,std::memory_order_relaxed);
			}

			void SetCameraExposureEV( const double ev )
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				meta_.cameraExposureEV = ev;
			}

			void SetFireFidelityMetadata(
				const std::string& status,
				const std::vector<std::string>& reasons,
				const std::vector<std::string>& recordIds,
				const std::vector<FrameStoreOutput::ActiveFireMedium>& media =
					std::vector<FrameStoreOutput::ActiveFireMedium>(),
				const std::vector<unsigned char>& renderConfig =
					std::vector<unsigned char>(),
				const std::vector<unsigned char>& rendererBuild =
					std::vector<unsigned char>(),
				const std::string& rendererBuildId = std::string()
				)
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				if( outputMetadataLeaseCount_ ) {
					throw std::runtime_error(
						"output_provenance_unavailable: output metadata is leased");
				}
				meta_.renderFidelityStatus = status;
				meta_.renderReasonCodes = reasons;
				meta_.activeFireOpticsRecordIds = recordIds;
				meta_.activeFireMedia = media;
				meta_.resolvedRenderConfigCoreV1 = renderConfig;
				meta_.rendererBuildV1 = rendererBuild;
				meta_.rendererBuildId = rendererBuildId;
				meta_.primaryProvenanceId.clear();
				meta_.primaryArtifactSha256.clear();
				meta_.primaryArtifactFidelity.clear();
				fireRenderPublicationBlocked_ = false;
			}

			void SetPreparedFireFidelityMetadata(
				const std::string& status,
				const std::vector<std::string>& reasons,
				const std::vector<std::string>& recordIds,
				const std::vector<FrameStoreOutput::ActiveFireMedium>& media,
				const std::vector<unsigned char>& renderConfig,
				const std::vector<unsigned char>& rendererBuild,
				const std::string& rendererBuildId )
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				if( outputMetadataLeaseCount_ ) {
					throw std::runtime_error(
						"output_provenance_unavailable: output metadata is leased");
				}
				meta_.renderFidelityStatus = status;
				meta_.renderReasonCodes = reasons;
				meta_.activeFireOpticsRecordIds = recordIds;
				meta_.activeFireMedia = media;
				meta_.resolvedRenderConfigCoreV1 = renderConfig;
				meta_.rendererBuildV1 = rendererBuild;
				meta_.rendererBuildId = rendererBuildId;
				meta_.primaryProvenanceId.clear();
				meta_.primaryArtifactSha256.clear();
				meta_.primaryArtifactFidelity.clear();
				fireRenderPublicationBlocked_ = !status.empty();
			}

			void CompleteFireRenderPublication()
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				fireRenderPublicationBlocked_ = false;
			}

			bool AcquireExternalArtifactMetadataSnapshot(
				Metadata& snapshot,
				bool& metadataLease )
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				metadataLease = false;
				if( !meta_.renderFidelityStatus.empty() ) {
					if( fireRenderPublicationBlocked_ || outputMetadataLeaseCount_ ) return false;
				}
				Metadata prepared = meta_;
				prepared.frame = completedFrame_.load(std::memory_order_relaxed);
				++outputMetadataLeaseCount_;
				metadataLease = true;
				snapshot = std::move(prepared);
				return true;
			}

			void ReleaseExternalArtifactMetadataLease()
			{
				ReleaseFireMetadataLease();
			}

			void SetPrimaryFireArtifact(
				const std::string& provenanceId,
				const std::string& artifactSha256,
				const std::string& artifactFidelity )
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				meta_.primaryProvenanceId = provenanceId;
				meta_.primaryArtifactSha256 = artifactSha256;
				meta_.primaryArtifactFidelity = artifactFidelity;
			}

			// ── back-compat shim (Phase 1 only) ───────────────────

			//! Returns the Beauty channel as an IRasterImage view,
			//! for the existing OIDN denoiser path (which takes
			//! IRasterImage) and for the Phase 1 FrameSink that
			//! copies from existing rasterizer outputs.  Phase 2
			//! retires this shim — rasterizers will write directly
			//! into the typed Beauty channel.
			Snapshot CaptureSnapshot() const;
			bool RestoreSnapshot( const Snapshot& snapshot );

			IRasterImage&       AsBeautyRasterImage();
			const IRasterImage& AsBeautyRasterImage() const;

		protected:
			//! Destructor protected per the Reference convention;
			//! callers free via release().
			virtual ~FrameStore();

		private:
			friend class FrameStoreBulkBracket;
			friend class Rasterizer;
			friend class PixelBasedRasterizerHelper;
			friend class ViewportFrameStore;

			class ObserverMutationToken;

			//! Prepared observer mutations are private transaction machinery for
			//! ViewportFrameStore.  A prepared removal leaves the observer
			//! registered but blocks new callback claims until commit or token
			//! destruction.  A prepared registration reserves vector capacity
			//! without publishing the observer.  Token destruction rolls either
			//! preparation back while the owning FrameStore is retained by VFS.
			ObserverMutationToken PrepareObserverRemoval(
				IRenderObserver* observer );
			ObserverMutationToken PrepareObserverRegistration();
			static void LockPreparedObserverMutations(
				const std::vector<ObserverMutationToken*>& tokens,
				const std::function<void(size_t)>& afterLock );
			static void UnlockPreparedObserverMutations(
				const std::vector<ObserverMutationToken*>& tokens ) noexcept;
			void CommitPreparedObserverRemoval(
				ObserverMutationToken& token ) noexcept;
			void CommitPreparedObserverRegistration(
				ObserverMutationToken& token,
				IRenderObserver* observer ) noexcept;
			void CommitPreparedObserverReplacement(
				ObserverMutationToken& registration,
				ObserverMutationToken& removal,
				IRenderObserver* observer ) noexcept;
			//! Freezes the identity-bearing fire envelope while a rasterizer is
			//! executing an authorized fire render.  Exposure/sample/frame progress
			//! and finalized-primary linkage remain independently writable.
			Metadata AcquireFireMetadataLeaseAndSnapshot()
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				Metadata snapshot = meta_;
				snapshot.frame = completedFrame_.load(std::memory_order_relaxed);
				++outputMetadataLeaseCount_;
				return snapshot;
			}

			void ReleaseFireMetadataLease()
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				if( outputMetadataLeaseCount_ ) --outputMetadataLeaseCount_;
			}

			void UpdateAnimatedFireMetadata(
				const std::vector<unsigned char>& renderConfig )
			{
				std::lock_guard<std::mutex> lock(metadataMutex_);
				if( !outputMetadataLeaseCount_ || meta_.renderFidelityStatus.empty() ) {
					throw std::runtime_error(
						"output_provenance_unavailable: animated fire metadata update is unauthorized");
				}
				meta_.resolvedRenderConfigCoreV1 = renderConfig;
				meta_.primaryProvenanceId.clear();
				meta_.primaryArtifactSha256.clear();
				meta_.primaryArtifactFidelity.clear();
			}

			//! Per-tile reader/writer lock.  `std::shared_mutex`
			//! gives N readers / 1 writer semantics that match the
			//! design intent (one rasterizer writes a tile while
			//! many UI / encoder readers read concurrently from
			//! other tiles, or block briefly while reading the
			//! actively-written tile).
			//!
			//! We previously used an atomic-seqlock (writer bumps
			//! odd→even, reader retries on parity mismatch), but
			//! the C++ memory model treats concurrent non-atomic
			//! access to pixel storage as UB regardless of the
			//! seqlock's retry-on-tear semantics — TSan would flag
			//! every read.  std::shared_mutex provides the required
			//! happens-before edges and is C++-standard-clean.  See
			//! L1 adversarial review P1.
			//!
			//! Heap-allocated array (not std::vector) because
			//! shared_mutex is non-copyable / non-movable.
			struct TileLock
			{
				// `mutable` because Render() is conceptually a const
				// observation of the FrameStore — locking the tile
				// to read it doesn't change the externally-visible
				// state.  Per the const-correctness-over-escape-
				// hatches skill, the const here is honest:
				// observation requires synchronisation, and the
				// synchronisation primitive is not part of the
				// observable state.
				mutable std::shared_mutex mtx;
			};

			size_t width_;
			size_t height_;
			size_t tileEdge_;
			size_t tileCountX_;
			size_t tileCountY_;

			// Channel presence (indexed by ChannelId).  Set at
			// construction; immutable thereafter so HasChannel is
			// race-free.
			std::vector<bool> presence_;

			// Channel storage.  unique_ptr so absent channels carry
			// no storage cost; null pointer = HasChannel false.
			std::unique_ptr<FrameStoreOutput::Channel<RISEPel>>   beauty_;
			// Alpha as Chel (double) for byte-identical L2 parity
			// with Color_Template<RISEPel>::a.  See Channel.h.
			std::unique_ptr<FrameStoreOutput::Channel<Chel>>      alpha_;
			std::unique_ptr<FrameStoreOutput::Channel<RISEPel>>   albedo_;
			std::unique_ptr<FrameStoreOutput::Channel<Vector3>>   normal_;
			std::unique_ptr<FrameStoreOutput::Channel<float>>     depth_;
			std::unique_ptr<FrameStoreOutput::Channel<uint32_t>>  objectId_;
			std::unique_ptr<FrameStoreOutput::Channel<uint32_t>>  primitiveId_;

			// Per-tile reader/writer lock; one entry per (tileX, tileY).
			std::unique_ptr<TileLock[]> tileLocks_;

			// Global generation counter.  Bumped on every EndTile
			// and MarkFrameComplete.  Used by UI repaint loops to
			// gate "store has changed since last paint".
			std::atomic<uint64_t> globalGeneration_{ 0 };

			// Observer list.  Guarded by observerMutex_ for
			// add/remove operations; DispatchObservers takes a
			// snapshot under the mutex then releases it before
			// invoking callbacks (so observers can safely call
			// AddObserver/RemoveObserver from inside their own
			// callbacks).
			//
			// Per-observer callback counts let an external RemoveObserver
			// wait for precisely the removed observer. Callback invocation is
			// serialized per store. A callback-side removal that would need to
			// wait for another active callback fails closed, avoiding raw-pointer
			// lifetime cycles without globally serializing independent stores.
			std::vector<IRenderObserver*>     observers_;
			std::vector<IRenderObserver*>     observerRemovalsPrepared_;
			size_t                            observerRegistrationReservations_ = 0u;
			mutable std::mutex                observerMutex_;
			std::mutex                        observerCallbackDispatchMutex_;
			std::map<IRenderObserver*,unsigned int> observerCallbacksInFlight_;
			mutable std::condition_variable   observerDispatchDone_;

			mutable std::mutex metadataMutex_;
			unsigned int outputMetadataLeaseCount_ = 0u;
			bool fireRenderPublicationBlocked_ = false;
			std::atomic<unsigned int> completedFrame_;
			Metadata meta_;

			//! IRasterImage shim view onto beauty_.  Constructed
			//! lazily on first AsBeautyRasterImage() call.  Held by
			//! unique_ptr so the FrameStore owns the lifetime.
			class BeautyRasterImageView;
			mutable std::unique_ptr<BeautyRasterImageView> beautyView_;

			// Helpers
			TileLock& TileLockAt( size_t tx, size_t ty )
			{
				return tileLocks_[ ty * tileCountX_ + tx ];
			}
			const TileLock& TileLockAt( size_t tx, size_t ty ) const
			{
				return tileLocks_[ ty * tileCountX_ + tx ];
			}

			//! Encode a single pixel through the ViewTransform pipeline
			//! and the target-format quantisation, writing into `dst`.
			//! `dst` is `bytesPerPixel` bytes (per fmt).  Used by
			//! Render() in its inner loop.
			void EncodePixel(
				const RISEPel&   linearPel,
				double           alpha,
				void*            dst,
				FrameStoreOutput::TargetFormat fmt,
				const FrameStoreOutput::ViewTransform& xform ) const;

			//! Snapshot-and-iterate dispatch helper for observer
			//! callbacks.  Locks observerMutex_ briefly, copies the
			//! observer list, releases the mutex, then invokes fn
			//! on each snapshot entry.  Avoids deadlocks on
			//! observer self-detach + the writer-blocked-on-slow-
			//! observer case. Reentrant observer dispatch fails closed;
			//! callbacks may mutate registration but may not recursively
			//! publish another FrameStore event. Defined in FrameStore.cpp;
			//! only used inside that TU.
			template <typename Fn>
			void DispatchObservers( Fn&& fn );
			bool RemoveObserverImpl( IRenderObserver* observer );
			void NotifyTileComplete( size_t tileX, size_t tileY, uint64_t generation );

			// FrameStore is non-copyable (Reference rules).
			FrameStore( const FrameStore& )            = delete;
			FrameStore& operator=( const FrameStore& ) = delete;
		};

		class FrameStore::ObserverMutationToken
		{
		public:
			ObserverMutationToken( ObserverMutationToken&& other ) noexcept;
			~ObserverMutationToken() noexcept;

			ObserverMutationToken( const ObserverMutationToken& ) = delete;
			ObserverMutationToken& operator=( const ObserverMutationToken& ) = delete;
			ObserverMutationToken& operator=( ObserverMutationToken&& ) = delete;

			bool IsPrepared() const { return owner_ != nullptr; }

		private:
			friend class FrameStore;
			enum class Kind { Registration, Removal };

			ObserverMutationToken( FrameStore& owner, Kind kind,
				IRenderObserver* observer );
			void Reset() noexcept;

			FrameStore* owner_;
			Kind kind_;
			IRenderObserver* observer_;
			std::unique_lock<std::mutex> lock_;
		};

		// L6e-1.1 — RAII guard for bulk full-image FrameStore writes.
		//
		// Some stages of the render pipeline write to the WHOLE image at
		// once instead of per-block (Resolve, OIDN denoise, Clear,
		// SplatFilm composition).  Per-block bracketing in
		// `SPRasterizeSingleBlock` doesn't cover them.  This guard
		// acquires every FrameStore tile's exclusive lock at construction
		// and releases all of them at destruction (so concurrent UI/
		// encoder readers see a clean post-write state, never a torn
		// half-written one).  Exception-safe — if the bracketed
		// `Resolve` / `ApplyDenoise` throws (OIDN device error, OOM in
		// scratch buffers), the destructor still runs and releases every
		// tile lock.  Free-function Begin/End helpers would deadlock in
		// that scenario.
		//
		// The guard is a no-op (locks nothing, releases nothing) when:
		//   * `fs` is null (no FrameStore bound), OR
		//   * `&image != &fs->AsBeautyRasterImage()` (the image being
		//     written is NOT the FrameStore beauty view — e.g. a private
		//     `RISERasterImage` whose dims happen to match camera dims,
		//     such as BDPT's path-guiding training image).  Identity
		//     check (not dim-match) prevents spurious tile-bumps + false
		//     OnTileComplete observer fires on un-modified FrameStore
		//     tiles.
		//
		// Lock acquisition is row-major (ty outer, tx inner) — same order
		// as `FrameStore::Render`'s shared-lock walk, so no AB/BA
		// deadlock between concurrent bulk-bracketed writers and a
		// concurrent reader.  Per-block writers in
		// `SPRasterizeSingleBlock` use the same order over their own
		// rect, so a bulk-bracket holder waits cleanly behind any
		// in-progress per-block writers (or vice versa).
		//
		// After every tile lock has been released, the guard bumps the
		// global generation counter and emits one `OnTileComplete` per
		// FrameStore tile (same fan-out as a fully-rendered per-block
		// frame). Observer failures are contained so a noexcept scope guard
		// cannot terminate an otherwise recoverable render unwind.
		//
		// IMPORTANT: do NOT construct nested inside an active per-tile
		// bracket window — `std::shared_mutex` is non-recursive, so a
		// second `BeginTile` on a tile already locked by the same thread
		// would deadlock.
		// L7 — Copy AOVBuffers contents into FrameStore's Albedo,
		// Normal, and Depth channels.  No-op when:
		//   * `fs` is null.
		//   * `aov`'s dims are zero.
		//   * `fs` and `aov` dim-mismatch.
		//   * neither Albedo nor Normal channel was requested in
		//     `fs`'s Spec at construction time.
		//
		// Bracketed via `FrameStoreBulkBracket` for concurrent-reader
		// correctness on the canonical store.  Type narrowing
		// (float→double) is loss-free for finite values.
		//
		// Called from `PixelBasedRasterizerHelper::RasterizeScene`,
		// `RasterizeSceneAnimation`, `BDPTRasterizerBase::RasterizeScene`,
		// and `MLTRasterizer::RasterizeScene` /
		// `RasterizeSceneAnimation` after their respective
		// `CollectFirstHitAOVs` calls — see commit messages for
		// L7 + the L7 follow-up that wired MLT.
		void PropagateAOVsToFrameStore(
			FrameStore* fs,
			const AOVBuffers& aov,
			const Rect* region = 0
			);

		//! Builds the smallest float-sidecar plan needed by a FrameStore,
		//! optionally unioned with the albedo+normal pair required by OIDN.
		AOVBuffers::Plan MakeAOVPlan( const FrameStore* fs, bool denoiserAux );

		class FrameStoreBulkBracket
		{
		public:
			FrameStoreBulkBracket( FrameStore* fs, const IRasterImage& image );
			// Releases every tile before notifying observers. Observer
			// failures are contained because a scope guard must not terminate
			// an otherwise recoverable render unwind.
			~FrameStoreBulkBracket() noexcept;

			// Non-copyable, non-movable — strict scope semantics.
			FrameStoreBulkBracket( const FrameStoreBulkBracket& )            = delete;
			FrameStoreBulkBracket& operator=( const FrameStoreBulkBracket& ) = delete;
			FrameStoreBulkBracket( FrameStoreBulkBracket&& )                 = delete;
			FrameStoreBulkBracket& operator=( FrameStoreBulkBracket&& )      = delete;

		private:
			FrameStore* mFs;  ///< null when guard is a no-op
		};

	} // namespace Implementation

	// Convenient unqualified alias for the common case.  Mirrors
	// the way RasterImage_Template / RISERasterImage are used.
	using FrameStore = Implementation::FrameStore;
}

// Template member-function implementations (compile-time channel
// dispatch).  In the header so callers can specialise without
// recompiling FrameStore.cpp.
namespace RISE
{
	namespace Implementation
	{
		template <FrameStoreOutput::ChannelId C>
		FrameStoreOutput::Channel<FrameStoreOutput::ChannelType<C>>* FrameStore::GetChannel()
		{
			using namespace FrameStoreOutput;
			if constexpr ( C == ChannelId::Beauty )      return beauty_.get();
			else if constexpr ( C == ChannelId::Alpha )  return alpha_.get();
			else if constexpr ( C == ChannelId::Albedo ) return albedo_.get();
			else if constexpr ( C == ChannelId::Normal ) return normal_.get();
			else if constexpr ( C == ChannelId::Depth )  return depth_.get();
			else if constexpr ( C == ChannelId::ObjectId )    return objectId_.get();
			else if constexpr ( C == ChannelId::PrimitiveId ) return primitiveId_.get();
			else {
				static_assert( C != C, "Unhandled ChannelId in GetChannel — "
				                       "did you add an enum value without "
				                       "wiring it through FrameStore?" );
				return nullptr;
			}
		}

		template <FrameStoreOutput::ChannelId C>
		const FrameStoreOutput::Channel<FrameStoreOutput::ChannelType<C>>*
		FrameStore::GetChannel() const
		{
			using namespace FrameStoreOutput;
			if constexpr ( C == ChannelId::Beauty )      return beauty_.get();
			else if constexpr ( C == ChannelId::Alpha )  return alpha_.get();
			else if constexpr ( C == ChannelId::Albedo ) return albedo_.get();
			else if constexpr ( C == ChannelId::Normal ) return normal_.get();
			else if constexpr ( C == ChannelId::Depth )  return depth_.get();
			else if constexpr ( C == ChannelId::ObjectId )    return objectId_.get();
			else if constexpr ( C == ChannelId::PrimitiveId ) return primitiveId_.get();
			else {
				static_assert( C != C, "Unhandled ChannelId in GetChannel" );
				return nullptr;
			}
		}
	}
}

#endif
