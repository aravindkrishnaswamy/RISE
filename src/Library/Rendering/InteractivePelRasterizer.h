//////////////////////////////////////////////////////////////////////
//
//  InteractivePelRasterizer.h - A pel-based rasterizer tuned for
//    live interactive preview.  Subclass of PixelBasedPelRasterizer
//    that hard-codes the "no GI, no path guiding, no adaptive
//    sampling, 1 SPP" configuration so platform code can't accidentally
//    inherit production-mode behaviour.  Optional OIDN denoising is
//    explicitly selected by the scene-edit controller for completed
//    idle / polish passes only; live drag stays raw.
//
//  Configuration knobs are documented at the InteractivePelRasterizer::
//  Config struct.  See docs/INTERACTIVE_EDITOR_PLAN.md §4.4.
//
//  This class is intended to live ALONGSIDE the production rasterizer,
//  never replace it.  The SceneEditController owns one of each: the
//  interactive instance for live preview, the production instance
//  (whatever the scene file declared) for "Render" dispatch.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_INTERACTIVEPELRASTERIZER_
#define RISE_INTERACTIVEPELRASTERIZER_

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "PixelBasedPelRasterizer.h"
#include "../Utilities/Color/Color.h"   // RISEPel for the Toolkit slice 3a objectmap palette

namespace RISE
{
	class ISampling2D;
	class IRayCaster;
	class IRasterizer;
	class IObject;

	namespace Implementation
	{
		//! Constructs the shared cross-platform interactive preview
		//! pipeline: material-preview shading, bounded preview AO, a
		//! live-drag caster, a richer pointer-up polish caster, and an
		//! InteractivePelRasterizer wired to swap between them.
		//!
		//! Returned pointers are refcounted ownership references for
		//! the caller to release.  The rasterizer also addrefs the
		//! casters internally, so platform bridges may keep and release
		//! their own caster handles exactly as they do for normal
		//! RISE_API_CreateRayCaster results.
		bool CreateInteractiveMaterialPreviewPipeline(
			IRasterizer** ppRasterizer,
			IRayCaster** ppPreviewCaster,
			IRayCaster** ppPolishCaster );

		//! Toolkit slice 3a (objectmap): the per-render object-identity
		//! palette + registry + pixel tally shared between the caller
		//! (AgentSession, which BUILDS it from the scene's ObjectManager
		//! BEFORE the render, on the render thread, and READS the tally
		//! AFTER) and the ObjectIdShader that
		//! CreateInteractiveObjectMapPipeline installs.  Built once and
		//! left immutable for the duration of one render EXCEPT for
		//! `counts`, which the shader bumps concurrently (the interactive
		//! rasterizer family renders multi-threaded via the block
		//! ThreadPool -- so the tally is std::atomic per entry).
		//!
		//! COLOR CONTRACT: `bytes[id]` is the FINAL 8-bit sRGB byte triple
		//! (== the PNG bytes == the legend colorHex).  `linearColors[id]`
		//! is the LINEAR pre-image RISEPel the shader emits, chosen so the
		//! forward sRGB encode (SRGBTransferFunction + truncating *255)
		//! lands on EXACTLY `bytes[id]` (half-LSB margin both sides).
		struct ObjectMapPalette
		{
			//! Scene IObject* (as seen on RayIntersection::pObject) ->
			//! dense id in [0, names.size()).  A primary-ray hit whose
			//! pObject is null or absent here decodes to the reserved
			//! UNKNOWN color and tallies into the last `counts` slot.
			std::unordered_map<const IObject*, std::uint32_t> registry;

			//! Per-id data (parallel; size == the registered-object count).
			std::vector<std::string>                     names;        //!< the object's manager name (legend label)
			std::vector<RISEPel>                         linearColors; //!< the linear pre-image the shader emits
			std::vector<std::array<unsigned char, 3> >   bytes;        //!< the final sRGB byte triple (legend colorHex)

			//! The reserved UNKNOWN color (pObject null / not registered).
			RISEPel                                      unknownLinear;
			std::array<unsigned char, 3>                 unknownBytes;

			//! Per-id pixel tally, size == names.size()+1 (the last slot
			//! is the UNKNOWN tally).  mutable + atomic: the shader holds a
			//! `const ObjectMapPalette*` and increments these from the
			//! parallel block workers.
			mutable std::vector<std::atomic<std::uint32_t> > counts;

			//! The smallest min-L1 colour separation the palette generator
			//! had to relax to (== the default 24 when it never degraded).
			//! Below the default it means the legend colours are still
			//! byte-UNIQUE but visually closer than usual -- the render
			//! result appends an honest "match by exact byte, not by eye"
			//! note (Toolkit slice 3a fix-round P2-1).
			unsigned int minColorDistance = 24;
		};

		//! Toolkit slice 3a (objectmap): construct an EPHEMERAL preview
		//! pipeline whose shader emits each hit object's flat identity
		//! colour (from `palette`) instead of shaded radiance -- a
		//! segmentation render.  Sibling of
		//! CreateInteractiveMaterialPreviewPipeline: the returned pointers
		//! are refcounted ownership references for the caller to release.
		//!
		//! `palette` is BORROWED -- the caller owns it and it MUST outlive
		//! the render (the installed shader captures its address).
		//!
		//! EXACTNESS INVARIANT: the returned rasterizer is left with NO
		//! sampling kernel and the caller MUST NOT install one nor call
		//! SetSampleCountOverride(>1) -- that is what routes every pixel
		//! through PixelBasedPelRasterizer::IntegratePixel's single-ray
		//! (no jitter, no filter) branch, the ONLY path that guarantees an
		//! exact, un-blended per-pixel identity byte.  A single ray caster
		//! suffices (there is no AO / polish pass for an id render), so
		//! unlike the material-preview factory this returns just the
		//! rasterizer + its one caster.
		bool CreateInteractiveObjectMapPipeline(
			IRasterizer** ppRasterizer,
			IRayCaster** ppCaster,
			const ObjectMapPalette& palette );

		class InteractivePelRasterizer : public PixelBasedPelRasterizer
		{
		public:
			//! Tile dispatch order.  CenterOut prioritizes the
			//! image center so the user sees the part of the scene
			//! they're pointing at first.
			enum TileOrder
			{
				TileOrder_CenterOut,
				TileOrder_Scanline,
				TileOrder_Random
			};

			//! Configuration. All defaults are "minimum-cost preview".
			struct Config
			{
				//! Samples per pass while the user is actively
				//! dragging.  1 is the right answer; the cancel-
				//! restart loop discards intermediate frames anyway.
				unsigned int liveSamplesPerPass;

				//! Total cap on idle-mode passes.  After the user
				//! releases the pointer, we keep refining up to this
				//! many additional passes.
				unsigned int idleMaxPasses;

				//! Tile dispatch order.
				TileOrder tileOrder;

				//! True to refine progressively when the user is
				//! idle (not dragging).
				bool progressiveOnIdle;

				Config()
				: liveSamplesPerPass( 1 )
				, idleMaxPasses( 16 )
				, tileOrder( TileOrder_CenterOut )
				, progressiveOnIdle( true )
				{}
			};

			InteractivePelRasterizer( IRayCaster* pCaster, const Config& cfg , RISE::Implementation::FrameStore* frameStore = nullptr);

			enum PreviewDenoiseMode
			{
				PreviewDenoise_Off,
				PreviewDenoise_Fast,
				PreviewDenoise_Balanced
			};

			//! Selects the OIDN policy for the next RasterizeScene call.
			//! Off during active manipulation, Fast for idle refinement,
			//! Balanced for pointer-up polish.
			void SetPreviewDenoiseMode( PreviewDenoiseMode mode );

			//! Switch between "live drag" and "idle progressive
			//! refinement" modes.  Called from the SceneEditController:
			//!   - false during pointer-move flurries (1 SPP, no progressive)
			//!   - true after pointer-up (multi-pass refinement)
			void SetIdleMode( bool idle ) const;

			//! True if currently in idle (refinement) mode.
			bool IsIdleMode() const { return mIdleMode; }

			const Config& GetConfig() const { return mCfg; }

			//! Configure samples-per-pixel for the next RasterizeScene
			//! call.  n=1 (default) clears any 2D sampling kernel so
			//! the rasterizer's per-pixel integration takes a single
			//! ray per pixel.  n>1 installs a multi-jittered kernel
			//! sized for the requested SPP and disables progressive
			//! mode so the next pass runs as a single multi-sampled
			//! render.  If a polish ray caster has been installed via
			//! SetPolishRayCaster, n>1 also swaps to that caster so
			//! the polish pass can use richer preview shading (for
			//! example, more AO probes) than the caster used during
			//! live drag.  Sticky until cleared with SetSampleCount(1).
			//!
			//! Used by SceneEditController to do a final 4-SPP polish
			//! pass at full resolution after the user releases the
			//! pointer (and after the 1-SPP scale=1 refinement pass
			//! completes).
			void SetSampleCount( unsigned int n );

			//! Install an optional secondary ray caster that
			//! SetSampleCount(>1) swaps in for the duration of a
			//! multi-SPP polish pass.  Refcounted; the rasterizer
			//! addrefs and releases on destruction.  Pass nullptr to
			//! clear.
			void SetPolishRayCaster( IRayCaster* polishCaster );

		protected:
			virtual ~InteractivePelRasterizer();

			// Skip both the random-pastel clear and the
			// OutputIntermediateImage notification.  The clear is the
			// debug visualization that produces "flashes of colour"
			// during cancel-restart in the interactive viewport;
			// OutputIntermediateImage is unused by the viewport sink
			// (which only listens to OutputImage at end-of-pass).
			//
			// (The persistent IRasterImage that lets cancelled passes
			// reuse previous content lives in PixelBasedRasterizerHelper
			// — InteractivePelRasterizer inherits it without further
			// configuration.)
			virtual void PrepareImageForNewRender( IRasterImage& img, const Rect* pRect ) const override;

			// Honour Config::tileOrder.  Default Config produces
			// centre-out so partial buffers from cancelled passes
			// show the middle of the image filled first instead of
			// the upper-left corner (Morton/Z-curve default).
			virtual IRasterizeSequence* CreateDefaultRasterSequence( unsigned int tileEdge ) const override;

			// Set RuntimeContext::bFastPreview = true so heavy shader
			// ops (SSS point-set build, future path-guiding lookups,
			// etc.) take their fast-preview fallback path during
			// interactive renders.  Production rasterizers leave the
			// default false; their RuntimeContexts carry full-
			// fidelity behaviour.  This is the principled extension
			// point for "interactive cares about latency, production
			// cares about correctness" — each shader op decides
			// what fast-preview means for its specific computation,
			// keyed on the same flag.
			virtual void PrepareRuntimeContext( RuntimeContext& rc ) const override;

#ifdef RISE_ENABLE_OIDN
			virtual bool ShouldDenoise() const override;
			virtual unsigned int GetDenoiseAOVSamplesPerPixel() const override;
#endif

			// L8 round 7 — `ShouldFireToggleObserverEvents()` is now
			// false by default on the base class (see header doc on
			// `PixelBasedRasterizerHelper`); no per-rasterizer
			// override needed.  The previous round-6 override here
			// was redundant once the base default flipped.

		private:
			Config                mCfg;
			mutable bool          mIdleMode;
			PreviewDenoiseMode    mPreviewDenoiseMode;

			// Lazy-initialized 2D sampling kernel for multi-SPP polish
			// passes.  Constructed on first SetSampleCount(>1); reused
			// thereafter.  Released in the destructor.
			ISampling2D*          mPolishKernel;

			// Optional secondary ray caster for the polish pass.
			// SetSampleCount(>1) swaps pCaster to this; SetSampleCount(1)
			// swaps back to mSavedPreviewCaster.  Both are refcounted by
			// us — addref on install, release on destruction.
			IRayCaster*           mPolishCaster;
			IRayCaster*           mSavedPreviewCaster;
		};
	}
}

#endif
