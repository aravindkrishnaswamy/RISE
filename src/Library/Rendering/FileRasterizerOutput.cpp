//////////////////////////////////////////////////////////////////////
//
//  FileRasterizerOutput.cpp - Implementation of a file rasterizer output
//  object
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 2, 2001
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FileRasterizerOutput.h"
#include "DisplayTransformWriter.h"
#include "FrameStore.h"
#include "FrameSink.h"
#include "FileEncoderObserver.h"
#include "FrameEncoders.h"
#include "../Interfaces/IOptions.h"
#include "../RISE_API.h"
#include "../Painters/CheckerPainter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/DiskFileWriteBuffer.h"
#include "../Utilities/RasterSanityScan.h"

#include <cassert>
#include <string.h>
#include <stdio.h>

using namespace RISE;
using namespace RISE::Implementation;

FileRasterizerOutput::FileRasterizerOutput(
	const char* szPattern_,
	const bool bMultiple_,
	const FRO_TYPE type_,
	const unsigned char bpp_,
	const COLOR_SPACE color_space_,
	const Scalar exposureEV_,
	const DISPLAY_TRANSFORM display_transform_,
	const EXR_COMPRESSION exr_compression_,
	const bool exr_with_alpha_
	) :
  bMultiple( bMultiple_ ),
  type( type_ ),
  bpp( bpp_ ),
  color_space( color_space_ ),
  exposureEV( exposureEV_ ),
  display_transform( display_transform_ ),
  cameraExposureEV( Scalar( 0 ) ),	// Landing 5: defaults to 0 = no camera-side EV; rasterizer overrides per-frame.
  rawCameraExposureEV( Scalar( 0 ) ),	// L8-r2: raw (un-HDR-zeroed) value for shared Meta writes.
  exr_compression( exr_compression_ ),
  exr_with_alpha( exr_with_alpha_ )
{
	// Check the global options file to figuring out where to stick the rendered files
	RISE::IOptions& options = GlobalOptions();

	// First check to see if we should just stick stuff using a rendered subfolder from the media location
	const bool bUseMediaFolder = options.ReadBool( "rendered_output_in_rise_media_folder", false );

	if( bUseMediaFolder ) {
		// Do the concatenation
		const char* szmediapath = getenv( "RISE_MEDIA_PATH" );
		if( szmediapath ) {
			strcpy( szPattern, szmediapath );
			strcat( szPattern, szPattern_ );
		} else {
			GlobalLog()->PrintEasyWarning( "FileRenderedOutput: Asked to use media path for rendered files, but media path is not set!" );
			strcpy( szPattern, szPattern_ );
		}
	} else {
		// Look for an option that gives us the folder
		RISE::String strOutputFolder = options.ReadString( "rendered_output_folder", "" );
		strcpy( szPattern, strOutputFolder.c_str() );
		strcat( szPattern, szPattern_ );
	}

	if( color_space==eColorSpace_Rec709RGB_Linear || color_space==eColorSpace_ROMMRGB_Linear ) {
		// Spit out a warning if a linear color space is selected but only 8bits is selected for precision
		if( bpp == 8 && type != HDR && type != RGBEA && type != EXR ) {
			GlobalLog()->PrintEasyWarning( "FileRasterizerOutput:: Linear colorspace chosen but bpp is 8-bit" );
		}
	}

	if( color_space == eColorSpace_sRGB || color_space==eColorSpace_ProPhotoRGB ) {
		// Spit out a warning if a nonlinear color space is selected for an HDR format
		if( type == HDR || type == RGBEA || type == EXR ) {
			GlobalLog()->PrintEasyWarning( "FileRasterizerOutput:: Nonlinear colorspace chosen for a high dynamic range format, this is bad idea!" );
		}
	}

	// New (Landing 1) — sanity-check the display-pipeline knobs against the format.
	// Tone mapping and exposure are LDR-only concepts; applying them
	// to an HDR archival output would corrupt the radiometric ground
	// truth.  Force them off and warn the user if they were set
	// non-default on an HDR format.
	if( IsHDRFormat( type ) ) {
		if( display_transform != eDisplayTransform_None ) {
			GlobalLog()->PrintEasyWarning(
				"FileRasterizerOutput:: display_transform is ignored for HDR formats "
				"(EXR / HDR / RGBEA) — those write linear radiance verbatim.  "
				"If you want a tone-mapped preview, declare a separate PNG output." );
			display_transform = eDisplayTransform_None;
		}
		if( exposureEV != Scalar( 0 ) ) {
			GlobalLog()->PrintEasyWarning(
				"FileRasterizerOutput:: exposure is ignored for HDR formats "
				"(EXR / HDR / RGBEA) — those write linear radiance verbatim.  "
				"Apply exposure on a separate LDR output, or post-process the EXR." );
			exposureEV = Scalar( 0 );
		}
	}

	// EXR-specific knobs warn (but we keep the values for future use)
	// when set on a non-EXR type.  The chunk parser defaults them to
	// the v1 sane values so the warning only fires on intentional
	// misuse.
	if( type != EXR ) {
		if( exr_compression != eExrCompression_Piz || !exr_with_alpha ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"FileRasterizerOutput:: exr_compression / exr_with_alpha are EXR-only "
				"and have no effect on type=%s", extensions[type] );
		}
	}
}

FileRasterizerOutput::~FileRasterizerOutput()
{
	// Caller contract: an IRasterizerOutput is destroyed AFTER all
	// Output* calls on it have returned.  In RISE, this is enforced
	// by the rasterizer / Job lifetime — Job keeps the rasterizer
	// alive while RasterizeScene runs, and only after that returns
	// does the IRasterizerOutput get released.  See L3 adversarial
	// review M1.
	//
	// `RemoveObserver` waits for in-flight observer DISPATCHES (the
	// L1-round-2 P2 fix), but NOT for in-flight `framesink_->OutputImage`
	// calls — those can be mid-CopyTileFromRasterImage when the
	// dtor runs.  That is acceptable only because the contract above
	// guarantees no concurrent Output* call is in flight when the
	// dtor runs.  IRasterizerOutput.h does not formally state this
	// (filed as a documentation follow-up); the assumption is
	// pervasive throughout the codebase.
	//
	// L8 — TeardownChain_ handles BOTH bound and internal mode
	// (releases through `boundCanonical_` in bound mode, through
	// `framestore_` in internal mode).  In BOUND mode there's no
	// `framesink_` to worry about (null) — the only in-flight
	// concern is `MarkFrameComplete` driving our observer's
	// `OnFrameComplete`.  `FrameStore::RemoveObserver` blocks until
	// any in-flight dispatch on this observer returns (per the L1
	// round-2 fix), so the dtor is safe even if the rasterizer
	// fires `MarkFrameComplete` immediately before our destruction.
	// The wait is bounded by the encoder's `Encode` runtime (one
	// file write).  See impl below.
	TeardownChain_();
}

// L8 — Tear down whichever chain is currently active.  Used by both
// the dtor and `OnRasterizerFrameStoreChanged` when transitioning
// modes.  Refcount bookkeeping splits on `boundToCanonical_`:
//   * Bound mode: `framestore_` is an alias for `boundCanonical_`;
//     the addref lives on `boundCanonical_`.  Release through there.
//     `framesink_` is null in bound mode (no internal sink needed).
//   * Internal mode: `framestore_` owns its addref (allocated in
//     EnsureChain via `new FrameStore`); release through it.
//     `framesink_` was allocated alongside.
void FileRasterizerOutput::TeardownChain_()
{
	if ( framestore_ && encoderObserver_ ) {
		framestore_->RemoveObserver( encoderObserver_ );
	}
	safe_release( encoderObserver_ );
	encoderObserver_ = nullptr;

	safe_release( framesink_ );
	framesink_ = nullptr;

	if ( boundToCanonical_ ) {
		// framestore_ aliases boundCanonical_; clear the alias
		// (don't release) and release through boundCanonical_.
		framestore_       = nullptr;
		safe_release( boundCanonical_ );
		boundCanonical_   = nullptr;
		boundToCanonical_ = false;
	} else {
		// Internal mode — framestore_ owns its addref.
		safe_release( framestore_ );
		framestore_ = nullptr;
	}
}

void FileRasterizerOutput::OutputIntermediateImage( const IRasterImage&, const Rect* )
{
	// File outputs don't support intermediate rasterizations dammit!
}

void FileRasterizerOutput::SetCameraExposureCompensationEV( Scalar ev )
{
	// Caller contract: the rasterizer calls this serially on the
	// same worker thread that drives Output*.  cameraExposureEV
	// is therefore not made atomic — both reads (in EnsureChain)
	// and writes (here) happen-before each other through the
	// rasterizer's own synchronization.  See L3 adversarial review L1.
	//
	// L8 review round 2 — Single source of truth is
	// `framestore_->Meta().cameraExposureEV`.  We always write the
	// RAW `ev` to it (no HDR-zeroing at the write side).  Encoder
	// gates per-format reading via `IsHDRFormat()` (FrameEncoders.cpp:75)
	// — HDR encoders skip the camera-EV apply block entirely; LDR
	// encoders apply it.
	//
	// Pre-round-2: HDR FROs force-zeroed `cameraExposureEV` then
	// wrote 0 to shared Meta.  In bound mode multiple FROs share
	// one canonical store — an HDR FRO would clobber LDR FROs'
	// values to zero.  The HDR-zeroing was redundant given the
	// encoder-side IsHDRFormat gate; removing it lets all writers
	// (FROs + VFS) write the same raw `ev` to Meta consistently
	// without any clobber concern.
	//
	// Local `cameraExposureEV` field still preserves the
	// HDR-zeroed value for any FRO-internal logic that depends on
	// it (today there's none; vestige kept for ABI ease).  The
	// encoder reads `framestore_->Meta()` directly, NOT this field.
	cameraExposureEV    = IsHDRFormat( type ) ? Scalar( 0 ) : ev;
	rawCameraExposureEV = ev;

	if ( framestore_ ) {
		framestore_->MutableMeta().cameraExposureEV =
			static_cast<double>( rawCameraExposureEV );
	}
}

namespace
{
	// Map the legacy FRO_TYPE enum to the FrameEncoderRegistry's
	// FormatName string.  Order matches FRO_TYPE values:
	//   TGA=0, PPM=1, PNG=2, HDR=3, TIFF=4, RGBEA=5, EXR=6.
	const char* FormatNameForType( FileRasterizerOutput::FRO_TYPE t )
	{
		switch ( t ) {
			case FileRasterizerOutput::TGA:   return "TGA";
			case FileRasterizerOutput::PPM:   return "PPM";
			case FileRasterizerOutput::PNG:   return "PNG";
			case FileRasterizerOutput::HDR:   return "HDR";
			case FileRasterizerOutput::TIFF:  return "TIFF";
			case FileRasterizerOutput::RGBEA: return "RGBEA";
			case FileRasterizerOutput::EXR:   return "EXR";
		}
		// Match the legacy WriteImageToFile switch's `default: PPM`
		// fallback (FileRasterizerOutput.cpp pre-L3, lines 311-316):
		// out-of-range FRO_TYPE values produced a PPM file rather
		// than dropping the render.  Preserve that for byte-identity
		// of any (real or future) scene that hits an unknown enum.
		// See L3 adversarial review HIGH-1.
		return "PPM";
	}
}

// L8 — Build a FileEncoderObserver bound to `store` and register it.
// Caller has either just allocated `store` (legacy mode) or just
// received it from the rasterizer (bound mode).  Returns false on
// encoder lookup failure (registry doesn't have an encoder for this
// FRO_TYPE — unlikely but defensive).
bool FileRasterizerOutput::BuildAndAttachObserver_( FrameStore* store )
{
	if ( !store ) return false;

	IFrameEncoder* encoder =
		FrameEncoderRegistry::Get().ByFormatName( FormatNameForType( type ) );
	if ( !encoder ) {
		GlobalLog()->PrintEx( eLog_Error,
			"FileRasterizerOutput:: No IFrameEncoder registered for format '%s' "
			"(FRO_TYPE=%d) — output disabled", FormatNameForType( type ), (int)type );
		return false;
	}

	// Build the encoder options from the constructor parameters.
	// This is the legacy → new mapping that L2 byte-identical
	// regression validates: same color_space + bpp + EXR knobs +
	// (exposureEV, display_transform) → same EncodeOpts → same
	// bytes.  cameraExposureEV is consumed via FrameStore.Meta()
	// inside the encoder, NOT added here, so we don't double-count.
	EncodeOpts opts;
	opts.colorSpace     = color_space;
	opts.bpp            = bpp;
	opts.exrCompression = exr_compression;
	opts.exrWithAlpha   = exr_with_alpha;
	opts.viewTransform.exposureEV = static_cast<float>( exposureEV );
	opts.viewTransform.toneCurve  = display_transform;

	encoderObserver_ = new FileEncoderObserver(
		store, encoder, opts,
		std::string( szPattern ), bMultiple );
	store->AddObserver( encoderObserver_ );
	return true;
}

// L8 — Notification handler.  Called by `Rasterizer::SetFrameStore`
// (post-L6e-2b) when the rasterizer's canonical FrameStore is
// installed or swapped.  We tear down whatever chain is currently
// active (legacy internal OR previously-bound canonical) and
// re-bind to the new canonical.
//
// `framestore` may be null — caller is unbinding (e.g. rasterizer's
// FrameStore was cleared).  In that case we revert to legacy lazy
// mode; the next Output*Image call's EnsureChain will allocate a
// fresh internal store.
//
// Same-pointer idempotent: re-binding to the SAME canonical is a
// no-op (avoids tearing down + rebuilding the observer registration).
void FileRasterizerOutput::OnRasterizerFrameStoreChanged( FrameStore* framestore )
{
	if ( boundToCanonical_ && framestore == boundCanonical_ ) {
		return;  // idempotent same-pointer rebind
	}

	// Tear down whatever's currently active (legacy or bound).
	TeardownChain_();

	if ( !framestore ) {
		// Unbind — revert to legacy lazy-internal mode.  The next
		// Output*Image call (if any) re-allocates an internal store.
		return;
	}

	// Bind to the canonical.  Mirror EnsureChain's setup but skip
	// the FrameSink + internal-store allocation — observers receive
	// MarkFrameComplete from the rasterizer's FlushToOutputs path
	// (post-L6f) on the canonical store directly.
	framestore->addref();  // defensive ref; canonical is also held
	                       // by Job + the rasterizer.
	boundCanonical_   = framestore;
	framestore_       = framestore;  // alias for legacy code paths
	boundToCanonical_ = true;
	// framesink_ stays null in bound mode.

	// L8 review round 2 — Write our cached RAW camera EV into the
	// canonical's Meta.  All bound FROs (regardless of HDR/LDR)
	// write the same raw value here, so multi-FRO scenes don't
	// clobber.  HDR encoders skip Meta-EV at READ time via the
	// `IsHDRFormat()` gate in FrameEncoders.cpp:75; LDR encoders
	// apply it.  This restores Meta as the single source of truth
	// (the round-1 fix introducing per-observer EV double-applied
	// when VFS ALSO wrote to Meta — see ViewportFrameStore.cpp:670).
	framestore->MutableMeta().cameraExposureEV =
		static_cast<double>( rawCameraExposureEV );

	if ( !BuildAndAttachObserver_( framestore ) ) {
		// Encoder lookup failed.  Tear down to leave us in a clean
		// "no observer" state; Output* calls remain no-ops.
		TeardownChain_();
		return;
	}

	GlobalLog()->PrintEx( eLog_Event,
		"FileRasterizerOutput::OnRasterizerFrameStoreChanged: bound to "
		"canonical FrameStore %ux%u (format=%s)",
		static_cast<unsigned>( framestore->Width() ),
		static_cast<unsigned>( framestore->Height() ),
		FormatNameForType( type ) );
}

void FileRasterizerOutput::EnsureChain( unsigned int width, unsigned int height )
{
	// L8 — bound mode: the canonical FrameStore is already installed
	// via OnRasterizerFrameStoreChanged.  No-op here; the rasterizer
	// drives MarkFrameComplete on its canonical store and the
	// FileEncoderObserver fires from there directly.
	if ( boundToCanonical_ ) return;

	if ( framestore_ ) {
		// Already allocated.  In the typical case dims are fixed
		// across frames (the rasterizer's image size is fixed by
		// the camera resolution at scene-construction time).  But
		// scenes can reconfigure the camera resolution between
		// frames; if that happens we must reallocate the chain so
		// the FrameStore matches the new rasterizer image —
		// otherwise FrameSink::CopyImageIntoStore clamps to the
		// smaller dim and silently produces black-padded or
		// cropped output.  See L3 adversarial review M4.
		if ( framestore_->Width()  == width &&
		     framestore_->Height() == height ) {
			return;
		}

		GlobalLog()->PrintEx( eLog_Warning,
			"FileRasterizerOutput:: rasterizer image size changed "
			"(%ux%u -> %ux%u); reallocating FrameStore + sink + observer.",
			static_cast<unsigned>( framestore_->Width() ),
			static_cast<unsigned>( framestore_->Height() ),
			width, height );

		// Tear down the existing chain in dtor order.
		framestore_->RemoveObserver( encoderObserver_ );
		safe_release( encoderObserver_ );
		safe_release( framesink_ );
		safe_release( framestore_ );
		encoderObserver_ = nullptr;
		framesink_       = nullptr;
		framestore_      = nullptr;
		// Fall through to the allocate-fresh path below.
	}

	// Allocate FrameStore.  TileEdge of 32 matches the typical
	// rasterizer tile size; the L3 ingest path doesn't depend on
	// alignment between rasterizer tiles and FrameStore tiles
	// (CopyImageIntoStore walks the FrameStore tile grid and
	// reads from src by absolute pixel coordinates), so any
	// reasonable value works.
	FrameStore::Spec spec;
	spec.width    = width;
	spec.height   = height;
	spec.tileEdge = 32;
	framestore_ = new FrameStore( spec );  // refcount = 1

	// L8 review round 2 — Write the cached RAW camera EV into the
	// freshly-allocated internal FrameStore's Meta.  Encoder reads
	// from Meta + gates on IsHDRFormat at write time.  This is
	// LEGACY (internal-store) mode; bound mode handles the same
	// in OnRasterizerFrameStoreChanged.
	framestore_->MutableMeta().cameraExposureEV =
		static_cast<double>( rawCameraExposureEV );

	// L8 — extracted observer build.  In legacy mode we ALSO need
	// the FrameSink to copy pixels into our internal FrameStore on
	// each Output*Image call (the rasterizer doesn't write to it
	// directly).  Bound mode skips both because the rasterizer
	// writes to the canonical store directly.
	framesink_ = new FrameSink( framestore_ );  // addrefs framestore_
	if ( !BuildAndAttachObserver_( framestore_ ) ) {
		// Encoder lookup failed — leave framestore_ + framesink_
		// allocated but skip observer setup.  Output* calls run the
		// FrameSink copy (which is harmless without an observer to
		// trigger encoding) until teardown.  Matches pre-L8
		// fall-through behaviour.
		return;
	}
}

// Shared post-write diagnostic.  Kept from the legacy
// implementation: scans the rasterizer's IRasterImage for
// negative-radiance pixels (typically reconstruction-filter
// side lobes) and emits a warning.  Purely advisory, doesn't
// affect the output bytes.
namespace
{
	void ScanForPathologicalPixels( const IRasterImage& pImage, const char* contextName )
	{
		const RasterSanityReport r = ScanRasterImageForPathologicalPixels( pImage );
		if ( r.negativeCount > 0 ) {
			GlobalLog()->PrintEx( eLog_Warning,
				"FileRasterizerOutput:: '%s' contains %u pixels with negative "
				"radiance (max magnitude %.4e).  Common cause: pixel reconstruction "
				"filter with negative side lobes (Mitchell / Lanczos) interacting "
				"with bright neighbors.  Switch to a non-negative filter "
				"(gaussian, triangle, box) if this is visually distracting, or "
				"accept minor clamping to 0 in LDR output formats.",
				contextName, r.negativeCount, (double)r.maxNegativeMagnitude );
		}
	}
}

// L3 shim implementations: each Output* call ensures the
// FrameStore + FrameSink + FileEncoderObserver chain is
// allocated, then forwards to the FrameSink.  The sink copies
// pixels into the FrameStore (per-tile shared_mutex held during
// the write) and calls the appropriate Mark* method, which
// fires the observer's OnFrameComplete /
// OnPreDenoiseComplete / OnDenoiseComplete — at which point
// the observer opens DiskFileWriteBuffer and runs the
// IFrameEncoder.  Bytes produced are L2-byte-identical to the
// legacy WriteImageToFile path (which has been removed; all
// regression coverage is in tests/FrameEncoderTest.cpp +
// tests/FileRasterizerOutputShimTest.cpp).

void FileRasterizerOutput::OutputImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame )
{
	EnsureChain( pImage.GetWidth(), pImage.GetHeight() );
	if ( framesink_ ) framesink_->OutputImage( pImage, pRegion, frame );
	ScanForPathologicalPixels( pImage, szPattern );
}

void FileRasterizerOutput::OutputPreDenoisedImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame )
{
	EnsureChain( pImage.GetWidth(), pImage.GetHeight() );
	if ( framesink_ ) framesink_->OutputPreDenoisedImage( pImage, pRegion, frame );
	ScanForPathologicalPixels( pImage, szPattern );
}

void FileRasterizerOutput::OutputDenoisedImage( const IRasterImage& pImage, const Rect* pRegion, const unsigned int frame )
{
	EnsureChain( pImage.GetWidth(), pImage.GetHeight() );
	if ( framesink_ ) framesink_->OutputDenoisedImage( pImage, pRegion, frame );
	ScanForPathologicalPixels( pImage, szPattern );
}
