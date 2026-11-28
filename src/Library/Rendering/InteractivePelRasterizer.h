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
	class IShader;

	namespace Implementation
	{
		//! Shared cross-platform interactive preview pipeline (macOS/Windows/Android
		//! bridges MUST route through here for viewport parity): material-preview
		//! shading, bounded AO, live-drag + polish casters, and InteractivePelRasterizer.
		//! Returned pointers are refcounted references for the caller to release.
		bool CreateInteractiveMaterialPreviewPipeline(
			IRasterizer** ppRasterizer,
			IRayCaster** ppPreviewCaster,
			IRayCaster** ppPolishCaster );

		//! Per-render object-identity palette + atomic pixel tally shared between
		//! caller and ObjectIdShader.  Immutable during render except `counts`.
		//! Color contract: `linearColors[id]` is the linear pre-image whose forward
		//! sRGB encode lands on exact 8-bit `bytes[id]`.
		struct ObjectMapPalette
		{
			std::unordered_map<const IObject*, std::uint32_t> registry;	//!< IObject* -> dense id in [0, names.size())
			std::vector<std::string>                     names;			//!< legend labels
			std::vector<RISEPel>                         linearColors;	//!< linear pre-images emitted by shader
			std::vector<std::array<unsigned char, 3> >   bytes;			//!< final 8-bit sRGB triples
			RISEPel                                      unknownLinear;
			std::array<unsigned char, 3>                 unknownBytes;
			//! Size == names.size() + 1 (last slot = UNKNOWN); bumped atomically by block workers.
			mutable std::vector<std::atomic<std::uint32_t> > counts;
			//! Minimum L1 separation achieved (< 24 means byte-unique but visually close).
			unsigned int minColorDistance = 24;
		};

		//! Ephemeral flat-ID segmentation pipeline (caller owns returned refs;
		//! `palette` is borrowed and must outlive the render).
		//! Exactness invariant: NO sampling kernel and NO SetSampleCountOverride(>1)
		//! so every pixel uses the un-blended single-ray path.
		bool CreateInteractiveObjectMapPipeline(
			IRasterizer** ppRasterizer,
			IRayCaster** ppCaster,
			const ObjectMapPalette& palette );

		//! Shared viewport render-mode registry across GUI, C-ABI, and agent tools
		//! (docs/gui/RENDER_MODES.md).  `name` is the wire format; enums are never serialized.
		enum class ViewportRenderMode
		{
			Preview,     //!< studio material preview
			ObjectMap,   //!< per-object identity segmentation
			Normals,     //!< world-space shading-normal false colour
			Depth,       //!< auto-windowed hit distance (near = bright)
			Facets,      //!< headlamp-shaded geometric normal
			Wireframe,   //!< first-hit triangle edges over dim facet shading
			DeepReflect, //!< BeautyVariant: low-res deep-bounce PT
			Direct,      //!< BeautyVariant: direct lighting only (maxBounces=1)
			Indirect,    //!< BeautyVariant: beauty minus camera-vertex direct (SetIndirectOnly)
			ClayLights   //!< BeautyVariant: neutral clay Lambertian override (SetClayOverride)
		};

		struct ViewportRenderModeInfo
		{
			ViewportRenderMode	mode;
			const char*			name;
			const char*			title;
			const char*			question;
			bool				wantsDenoise;
			bool				viewportSelectable;
			bool				casterFactory;
			//! BeautyVariant config (read only when IsBeautyVariantMode(mode) == true).
			unsigned int		variantScaleDivisor;
			unsigned int		variantMaxBounces;     //!< PathTracingIntegrator::SetMaxPathDepth cap
			unsigned int		variantSamplesPerPass; //!< > 0 identifies BeautyVariant rows
			bool			variantIndirectOnly;
			bool			variantClayOverride;
		};

		//! True iff the mode row has variantSamplesPerPass > 0.
		bool IsBeautyVariantMode( ViewportRenderMode mode );

		const ViewportRenderModeInfo* GetViewportRenderModes( unsigned int& outCount );
		const ViewportRenderModeInfo* FindViewportRenderModeByName( const char* name );
		const ViewportRenderModeInfo* FindViewportRenderModeInfo( ViewportRenderMode mode );

		//! Builds an ephemeral view-mode caster (Normals/Depth/Facets/Wireframe).
		//! `xray` enables RayCaster::SetXrayViewResolve(true) (straight-line
		//! see-through past transmissive surfaces, no refraction).  Caller releases ref.
		bool CreateInteractiveViewModeCaster(
			ViewportRenderMode mode,
			IRayCaster** ppCaster,
			bool xray = false );

		//! Wraps CreateInteractiveViewModeCaster in a 1-spp non-progressive
		//! InteractivePelRasterizer (caller releases refs).
		bool CreateInteractiveViewModePipeline(
			ViewportRenderMode mode,
			IRasterizer** ppRasterizer,
			IRayCaster** ppCaster,
			bool xray = false );

		//! Builds an ephemeral production-class PT pipeline for a BeautyVariant
		//! mode (docs/gui/RENDER_MODES.md §6): multijittered sampler, box filter,
		//! SMS off, OIDN on, plus SetMaxPathDepth / SetIndirectOnly / SetClayOverride
		//! (propagated via RuntimeContext to SSS/random-walk continuations).
		//! `pDefaultShader` is used by caster-dispatched BSSRDF/SSS continuations;
		//! nullptr installs a generic DefaultPathTracing chain.  Caller releases refs.
		bool CreateBeautyVariantPipeline(
			ViewportRenderMode mode,
			IRasterizer** ppRasterizer,
			IRayCaster** ppCaster,
			IShader* pDefaultShader = 0 );

		//! Ephemeral fixed-fidelity PT pipeline for isolated material inspection
		//! (not a ViewportRenderMode; caller installs/restores light rig and
		//! releases returned refs).
		bool CreateMaterialLookPipeline(
			IRasterizer** ppRasterizer,
			IRayCaster** ppCaster,
			IShader* pDefaultShader = 0,
			unsigned int* outSamplesPerPixel = 0,
			unsigned int* outMaxBounces = 0 );

		//! Configures a BeautyVariant pass: liveGesture uses 1 SPP + OIDN off;
		//! idle restores registry SPP + OIDN.  Returns false if not a BeautyVariant.
		bool ConfigureBeautyVariantPass(
			IRasterizer& rasterizer,
			ViewportRenderMode mode,
			bool liveGesture );

		//! Center-sampled nearest-neighbor derivative of `source` (caller owns *out).
		bool CreateNearestNeighborSubview(
			const IRasterImage& source,
			unsigned int width,
			unsigned int height,
			IRasterImage** out );

		//! Marks an in-flight BeautyVariant quantum obsolete so its OIDN tail skips atomically.
		bool SuppressBeautyVariantDenoise( IRasterizer& rasterizer );

		class InteractivePelRasterizer : public PixelBasedPelRasterizer
		{
		public:
			enum TileOrder
			{
				TileOrder_CenterOut,
				TileOrder_Scanline,
				TileOrder_Random
			};

			struct Config
			{
				unsigned int liveSamplesPerPass;
				unsigned int idleMaxPasses;
				TileOrder tileOrder;
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

			//! OIDN policy for next RasterizeScene: Off (drag), Fast (idle), Balanced (polish).
			void SetPreviewDenoiseMode( PreviewDenoiseMode mode );

			//! False during live drag (1 SPP, non-progressive); true after pointer-up.
			void SetIdleMode( bool idle ) const;
			bool IsIdleMode() const { return mIdleMode; }
			const Config& GetConfig() const { return mCfg; }

			//! Sticky SPP override: n=1 clears kernel (single ray); n>1 installs a
			//! multi-jittered kernel, disables progressive, and swaps to polish caster
			//! (suppressed while a view-mode caster is installed).
			void SetSampleCount( unsigned int n );

			//! Optional refcounted caster swapped in by SetSampleCount(>1) (nullptr clears).
			void SetPolishRayCaster( IRayCaster* polishCaster );

			//! Installs/clears a refcounted view-mode caster (docs/gui/RENDER_MODES.md §5).
			//! Unwinds any active polish swap before saving the true preview caster;
			//! while installed, suppresses polish swaps and gates ShouldDenoise via `allowsDenoise`.
			void SetViewModeCaster( IRayCaster* p, bool allowsDenoise = false );
			bool HasViewModeCaster() const { return mViewModeCasterInstalled; }

			//! Viewport-wide x-ray see-through toggle (docs/gui/RENDER_MODES.md);
			//! stamped onto active, polish, and saved RayCasters and on every swap.
			void SetXrayView( bool on );
			bool GetXrayView() const { return mXrayView; }

			//! Overrides base RasterizeScene to run at most one extra pass if an
			//! active Depth view-mode caster's auto-window is stale (not cancelled).
			virtual void RasterizeScene( const IScene& pScene, const Rect* pRect, IRasterizeSequence* pRasterSequence ) const override;

		protected:
			virtual ~InteractivePelRasterizer();

			// Skip random-pastel clear + intermediate output to avoid flashes on cancel-restart.
			virtual void PrepareImageForNewRender( IRasterImage& img, const Rect* pRect ) const override;
			virtual IRasterizeSequence* CreateDefaultRasterSequence( unsigned int tileEdge ) const override;
			// Sets RuntimeContext::bFastPreview = true for interactive preview latency.
			virtual void PrepareRuntimeContext( RuntimeContext& rc ) const override;

#ifdef RISE_ENABLE_OIDN
			virtual bool ShouldDenoise() const override;
			virtual unsigned int GetDenoiseAOVSamplesPerPixel() const override;
#endif

		private:
			Config                mCfg;
			mutable bool          mIdleMode;
			PreviewDenoiseMode    mPreviewDenoiseMode;

			ISampling2D*          mPolishKernel;
			IRayCaster*           mPolishCaster;
			IRayCaster*           mSavedPreviewCaster;

			// Separate from mSavedPreviewCaster so view-mode and polish swaps nest cleanly.
			bool                  mViewModeCasterInstalled;
			bool                  mViewModeCasterAllowsDenoise;
			IRayCaster*           mSavedPreviewCasterForViewMode;

			bool                  mXrayView;
			void ApplyXrayViewToCaster_( IRayCaster* c ) const;

		};
	}
}

#endif
