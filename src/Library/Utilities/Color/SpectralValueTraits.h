//////////////////////////////////////////////////////////////////////
//
//  SpectralValueTraits.h - Compile-time dispatch for path-transport
//    code that needs to work uniformly on RGB (RISEPel) or single
//    wavelength (Scalar + nm) values.
//
//    The traits layer sits above IBSDF / ISPF / IMaterial, whose
//    value() / valueNM() dual-virtual pattern is preserved.  This
//    header exists so integrator bodies can be written once as
//    function templates parameterized on a tag, and expand to the
//    same machine code as hand-written Pel or NM specializations.
//
//    Design choices:
//      * Tags are value types, not class-template parameters, so
//        they carry the runtime wavelength context (nm) the NM
//        calls need without forcing callers to pass a separate
//        parameter.
//      * HWSS (hero wavelength spectral sampling) is NOT a separate
//        tag.  HWSS companion wavelengths are driven by a
//        rasterizer-level loop that invokes the NM path per
//        wavelength and then composites.  A separate HWSS tag
//        would duplicate that orchestration without simplifying
//        the integrator core.
//      * All methods are inline.  The expectation is that tag
//        dispatch compiles to the same instruction stream as direct
//        Pel / NM calls.  Phase 0 ships a unit test
//        (PathValueOpsTest) that verifies bit-identical output.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: April 17, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef SPECTRAL_VALUE_TRAITS_
#define SPECTRAL_VALUE_TRAITS_

#include "Color.h"
// Color.h transitively includes ColorMath.h; explicit include kept for
// clarity that this header depends on ColorMath::MaxValue.
#include "ColorMath.h"

namespace RISE
{
	namespace SpectralDispatch
	{
		/// Tag for RGB evaluation.  Carries no runtime state.
		struct PelTag
		{
		};

		/// Tag for single-wavelength evaluation.  Carries the
		/// wavelength in nanometers.
		struct NMTag
		{
			Scalar nm;

			explicit NMTag( const Scalar nm_ ) : nm( nm_ ) {}
		};

		/// Primary template.  Specialized per tag.  Exposes:
		///   using value_type;
		///   static constexpr bool is_pel;
		///   static constexpr bool is_nm;
		///   static constexpr bool supports_aov;
		///   static value_type zero();
		///   static bool is_zero( const value_type& v );
		///   static Scalar max_value( const value_type& v );
		///
		/// Deliberately does NOT provide a `luminance()` accessor:
		/// the semantically-correct luminance of a single-wavelength
		/// scalar is V(λ)·value, not the RGB Rec.709 weighting, and
		/// neither semantic is needed by the current integrators.
		/// Add an explicit `MagnitudeForGuiding<Tag>` free function
		/// at the call site where the correct semantic is known,
		/// rather than bolting a misleading `luminance` onto traits.
		template<class Tag>
		struct SpectralValueTraits
		{
			static_assert( sizeof( Tag ) == 0,
				"SpectralValueTraits must be specialized for this Tag. "
				"Add an explicit specialization for your new Tag type." );
		};

		//////////////////////////////////////////////////////////////
		// Pel specialization
		//////////////////////////////////////////////////////////////

		template<>
		struct SpectralValueTraits<PelTag>
		{
			using value_type = RISEPel;

			static constexpr bool is_pel        = true;
			static constexpr bool is_nm         = false;
			static constexpr bool supports_aov  = true;

			static inline value_type zero()
			{
				return RISEPel( 0, 0, 0 );
			}

			static inline bool is_zero( const value_type& v )
			{
				return ColorMath::MaxValue( v ) <= 0;
			}

			/// Max absolute channel.  Used by Russian-roulette survival
			/// probability computation and BDPT-style contribution tests.
			/// For NM this is fabs(v); for Pel this is the max channel
			/// value.  Naming preserves the existing integrator
			/// convention (ColorMath::MaxValue for RISEPel; fabs for
			/// throughputNM).
			static inline Scalar max_value( const value_type& v )
			{
				return ColorMath::MaxValue( v );
			}
		};

		//////////////////////////////////////////////////////////////
		// NM specialization
		//////////////////////////////////////////////////////////////

		template<>
		struct SpectralValueTraits<NMTag>
		{
			using value_type = Scalar;

			static constexpr bool is_pel        = false;
			static constexpr bool is_nm         = true;
			static constexpr bool supports_aov  = false;

			static inline value_type zero()
			{
				return Scalar( 0 );
			}

			static inline bool is_zero( const value_type& v )
			{
				return v == Scalar( 0 );
			}

			/// fabs to match the existing integrator convention:
			///   max_value( scalar ) = fabs( scalar )
			///   max_value( RISEPel ) = ColorMath::MaxValue( pel )
			/// This matches PathTracingIntegrator's GuidingEffectiveAlpha
			/// and the BDPT/VCM NM paths, which all use fabs() when
			/// comparing NM throughput against the RR threshold.
			static inline Scalar max_value( const value_type& v )
			{
				return std::fabs( v );
			}
		};
	}
}

#endif
