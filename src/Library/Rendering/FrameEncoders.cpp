//////////////////////////////////////////////////////////////////////
//
//  FrameEncoders.cpp - Implementation of the 7 concrete IFrameEncoders
//  + the FrameEncoderRegistry singleton.
//
//  See FrameEncoders.h for the design context.  Per-format byte
//  output is delegated to the existing IRasterImageWriter classes
//  (PNGWriter, EXRWriter, etc.) via the RISE_API_Create*Writer
//  factories — same writers FileRasterizerOutput uses, so byte-
//  identity holds modulo the DisplayTransformWriter wrap decision
//  (which we mirror exactly).
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "FrameEncoders.h"
#include "DisplayTransformWriter.h"
#include "FrameStore.h"

#include "../Interfaces/ILog.h"
#include "../RISE_API.h"

#include <algorithm>
#include <cassert>
#include <cctype>

using namespace RISE;
using namespace RISE::Implementation;
using namespace RISE::FrameStoreOutput;

namespace RISE
{
	namespace Implementation
	{

		// ─────────────────────────────────────────────────────────────
		// FrameEncoderBase::Encode — shared "construct, wrap, dump"
		// ─────────────────────────────────────────────────────────────
		void FrameEncoderBase::Encode(
			const FrameStore& store,
			IWriteBuffer&     dst,
			const EncodeOpts& opts )
		{
			IRasterImageWriter* pWriter = CreateWriter( dst, opts );
			if ( !pWriter ) {
				GlobalLog()->PrintEx( eLog_Error,
					"FrameEncoder[%s]: writer factory returned null",
					FormatName().c_str() );
				return;
			}

			// Decide whether to wrap in DisplayTransformWriter.
			// Mirrors the gate in FileRasterizerOutput.cpp:232 —
			// LDR-only AND non-default exposure or tone curve.
			//
			// Total EV is the sum of the caller-supplied static
			// exposure (from opts.viewTransform.exposureEV — UI
			// slider, scene-declared exposure_compensation, etc.)
			// and the per-frame camera-side EV (from
			// store.Meta().cameraExposureEV — set by the rasterizer
			// at frame start via SetCameraExposureCompensationEV).
			// This matches FileRasterizerOutput.cpp:231.  See L2
			// adversarial review HIGH-2.
			IRasterImageWriter* pEffective = pWriter;
			DisplayTransformWriter* pDtw = nullptr;
			if ( !IsHDRFormat() ) {
				const Scalar staticEV =
					static_cast<Scalar>( opts.viewTransform.exposureEV );
				const Scalar cameraEV =
					static_cast<Scalar>( store.Meta().cameraExposureEV );
				const Scalar totalEV = staticEV + cameraEV;
				const bool useDt =
					   ( opts.viewTransform.toneCurve != eDisplayTransform_None )
					|| ( totalEV != Scalar( 0 ) );
				if ( useDt ) {
					pDtw = new DisplayTransformWriter(
						*pWriter, totalEV, opts.viewTransform.toneCurve );
					GlobalLog()->PrintNew(
						pDtw, __FILE__, __LINE__, "DisplayTransformWriter" );
					pEffective = pDtw;
				}
			}

			// Drive the writer through the FrameStore's
			// IRasterImage shim.  BeautyRasterImageView::DumpImage
			// walks pixels in the same row-major order as
			// RasterImage_Template::DumpImage, so the bytes the
			// writer emits are byte-identical to the legacy path
			// for the same input.
			store.AsBeautyRasterImage().DumpImage( pEffective );

			// Release wrapper before inner writer (match
			// FileRasterizerOutput.cpp:268-277 ordering).
			if ( pDtw ) {
				GlobalLog()->PrintDelete( pDtw, __FILE__, __LINE__ );
				safe_release( pDtw );
			}
			GlobalLog()->PrintDelete( pWriter, __FILE__, __LINE__ );
			safe_release( pWriter );
		}

		// ─────────────────────────────────────────────────────────────
		// Per-format CreateWriter overrides
		// ─────────────────────────────────────────────────────────────
		IRasterImageWriter* PNGFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreatePNGWriter( &w, dst, opts.bpp, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* EXRFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateEXRWriter( &w, dst,
				opts.colorSpace, opts.exrCompression, opts.exrWithAlpha );
			return w;
		}

		IRasterImageWriter* TIFFFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateTIFFWriter( &w, dst, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* HDRFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateHDRWriter( &w, dst, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* RGBEAFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& /*opts*/ ) const
		{
			// RGBEA factory does NOT take a color_space — see
			// RISE_API.h:1992.  RGBEA always writes Radiance-RGBE
			// linear radiance.
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateRGBEAWriter( &w, dst );
			return w;
		}

		IRasterImageWriter* TGAFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreateTGAWriter( &w, dst, opts.colorSpace );
			return w;
		}

		IRasterImageWriter* PPMFrameEncoder::CreateWriter(
			IWriteBuffer& dst, const EncodeOpts& opts ) const
		{
			IRasterImageWriter* w = nullptr;
			RISE_API_CreatePPMWriter( &w, dst, opts.colorSpace );
			return w;
		}

		// ─────────────────────────────────────────────────────────────
		// FrameEncoderRegistry
		// ─────────────────────────────────────────────────────────────

		// Case-insensitive ASCII compare (helper for lookup methods).
		static bool IEqualsASCII( const std::string& a, const std::string& b )
		{
			if ( a.size() != b.size() ) return false;
			for ( size_t i = 0; i < a.size(); ++i ) {
				const unsigned char ca = static_cast<unsigned char>( a[i] );
				const unsigned char cb = static_cast<unsigned char>( b[i] );
				if ( std::tolower( ca ) != std::tolower( cb ) ) return false;
			}
			return true;
		}

		// Strip a leading "." from an extension if present.
		static std::string StripDot( const std::string& s )
		{
			return ( !s.empty() && s[0] == '.' ) ? s.substr( 1 ) : s;
		}

		FrameEncoderRegistry& FrameEncoderRegistry::Get()
		{
			// Meyers singleton — thread-safe initialization
			// guaranteed by C++11 §6.7p4 (function-local statics).
			static FrameEncoderRegistry instance;
			return instance;
		}

		FrameEncoderRegistry::FrameEncoderRegistry()
		{
			RegisterBuiltins();
		}

		FrameEncoderRegistry::~FrameEncoderRegistry()
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			for ( IFrameEncoder* enc : encoders_ ) {
				safe_release( enc );
			}
			encoders_.clear();
		}

		void FrameEncoderRegistry::RegisterBuiltins()
		{
			// Register order is iteration order returned by All().
			Register( new PNGFrameEncoder() );
			Register( new EXRFrameEncoder() );
			Register( new TIFFFrameEncoder() );
			Register( new HDRFrameEncoder() );
			Register( new RGBEAFrameEncoder() );
			Register( new TGAFrameEncoder() );
			Register( new PPMFrameEncoder() );
		}

		void FrameEncoderRegistry::Register( IFrameEncoder* encoder )
		{
			if ( !encoder ) return;

			std::lock_guard<std::mutex> lock( mutex_ );

			// Replace existing entry with the same FormatName.
			for ( auto it = encoders_.begin(); it != encoders_.end(); ++it ) {
				if ( IEqualsASCII( ( *it )->FormatName(), encoder->FormatName() ) ) {
					safe_release( *it );
					*it = encoder;
					return;
				}
			}
			encoders_.push_back( encoder );
		}

		IFrameEncoder* FrameEncoderRegistry::ByFormatName( const std::string& name ) const
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			for ( IFrameEncoder* enc : encoders_ ) {
				if ( IEqualsASCII( enc->FormatName(), name ) ) {
					return enc;
				}
			}
			return nullptr;
		}

		IFrameEncoder* FrameEncoderRegistry::ByExtension( const std::string& ext ) const
		{
			const std::string needle = StripDot( ext );
			std::lock_guard<std::mutex> lock( mutex_ );
			for ( IFrameEncoder* enc : encoders_ ) {
				for ( const std::string& e : enc->Extensions() ) {
					if ( IEqualsASCII( e, needle ) ) {
						return enc;
					}
				}
			}
			return nullptr;
		}

		std::vector<IFrameEncoder*> FrameEncoderRegistry::All() const
		{
			std::lock_guard<std::mutex> lock( mutex_ );
			return encoders_;
		}

	} // namespace Implementation
} // namespace RISE
