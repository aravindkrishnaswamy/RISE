//////////////////////////////////////////////////////////////////////
//
//  ColorUtils.h - Color utility functions
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: February 8, 2002
//  Tabs: 4
//  Comments: 
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef COLOR_UTILS_
#define COLOR_UTILS_

#include "Color.h"
#include "../../Interfaces/IFunction1D.h"
#include "../../Interfaces/IWriteBuffer.h"
#include "../../Interfaces/IReadBuffer.h"
#include "../FiniteMath.h"
#include <cmath>
#include <cstdint>
#include <type_traits>


namespace RISE
{
	namespace ColorUtils
	{
		// RGBE/RGBEA share an 8-bit exponent biased by 128. The largest
		// channel is (2^bits-1)*2^(255-128-bits). Clamp before frexp/casts;
		// retain the established low floor and truncate exact binary-scaled mantissas.
		template< unsigned int bits, class Pel >
		inline void PackSharedExponentRGB( const Pel& p, unsigned int (&samples)[3], unsigned char& exponent )
		{
			static_assert(bits==8 || bits==16,"shared-exponent mantissas must have 8 or 16 bits");
			constexpr unsigned int maxSample=(1u<<bits)-1u;
			const double ceiling=std::ldexp(double(maxSample),127-int(bits));
			const auto clip=[ceiling](double value) {
				if(!IsFiniteDouble(value))return IsPositiveInfinityDouble(value)?ceiling:0.0;
				if(value<=0)return 0.0;
				return value>ceiling?ceiling:value;
			};
			const double channels[]={clip(p.r),clip(p.g),clip(p.b)};
			double peak=channels[0];
			for(unsigned int i=1;i<3;++i)if(channels[i]>peak)peak=channels[i];
			samples[0]=samples[1]=samples[2]=0;exponent=0;
			if(peak<1e-32)return;
			int power=0;
			std::frexp(peak,&power);
			// A power-of-two scale avoids cancellation and ensures the peak
			// mantissa has its high bit set, disambiguating raw RGBE from RLE.
			const double scale=std::ldexp(1.0,int(bits)-power);
			for(unsigned int i=0;i<3;++i) {
				const double scaled=channels[i]*scale;
				samples[i]=scaled>=maxSample?maxSample:static_cast<unsigned int>(scaled);
			}
			exponent=static_cast<unsigned char>(power+128);
		}

		//! Applies the SRGB non-linearization transfer function
		/// \return SRGB result
		Scalar SRGBTransferFunction(
			const Scalar& x							///< [in] Value to transform
			);

		//! Applies the inverse of the SRGB non-linearization transfer function 
		/// \return Inverse SRGB result
		Scalar SRGBTransferFunctionInverse( 
			const Scalar& x							///< [in] Value to transform
			);

		//! Applies the ProPhotoRGB (ROMM RGB) non-linearization transfer function
		/// \return ProPhotoRGB (ROMM RGB) result
		Scalar ROMMRGBTransferFunction(
			const Scalar& x							///< [in] Value to transform
			);

		//! Applies the inverse of the ProPhotoRGB (ROMM RGB) non-linearization transfer function 
		/// \return Inverse ProPhotoRGB (ROMM RGB) result
		Scalar ROMMRGBTransferFunctionInverse( 
			const Scalar& x							///< [in] Value to transform
			);

		inline Scalar ApplySPDFunction( 
			const unsigned int& min_index,
			const unsigned int& max_index,
			const Scalar& weight,
			const Scalar* table
			)
		{
			const Scalar& min_value = table[min_index];
			const Scalar& max_value = table[max_index];

			const Scalar delta = max_value - min_value;

			return min_value + weight * delta;
		}

		//! Given a particular wavelength computes the indices to interpolate and the amount to
		//! interpolate for some arbritary spectral sensitivity table
		/// \return TRUE if the given wavelength is in the range for conversion, FALSE otherwise
		bool InterpCIE_SPDIndices( 
			Scalar nm,								///< [in] Wavelength to compute 
			unsigned int& min_index, 				///< [out] Index to interpolate from
			unsigned int& max_index,	 			///< [out] Index to interpolate to
			Scalar& weight							///< [out] Interpolation amount
			);

		//! Converts a particular wavelength of light into an CIE_XYZ value
		/// \return TRUE if the conversion was successful, FALSE otherwise
		bool XYZFromNM(
			XYZPel& p,								///< [out] Resultant CIE_XYZ value
			const Scalar nm							///< [in] Wavelength to convert
			);

		//! Returns ∫Ȳ(λ)dλ over [lambda_begin, lambda_end] using the
		//! CIE 1931 2° piecewise-linear table, clipped to [380,780] nm.
		//! Empty/reversed intervals or nonfinite endpoints return zero.
		//! Used by spectral rasterizers
		//! to normalize the MC luminance estimator so a perfect-white
		//! reflector under flat illuminant integrates to Y = 1
		//! (matching the RGB rasterizer's white = 1 convention).
		//!
		//! Math: the MC estimator (1/N)·Σ X̄(λᵢ)·V(λᵢ) for uniformly
		//! sampled λᵢ ∈ [a, b] approximates the AVERAGE of the
		//! integrand, not the integral.  Scale by (b-a) to get the
		//! integral, then divide by k_y = ∫Ȳdλ to normalize.  Net
		//! per-sample factor: (b-a)/k_y, applied as a uniform scale
		//! that preserves chromaticity.
		Scalar CIE_Y_Integral(
			const Scalar lambda_begin,					///< [in] Start wavelength (nm)
			const Scalar lambda_end						///< [in] End wavelength (nm)
			);

		//! Moves the given XYZPel into the Rec 709 RGB gamut
		void MoveXYZIntoRec709RGBGamut( 
			XYZPel& p								///< [in/out] Color to move, resultant CIE_XYZ value
			);

		//! Moves the given XYZPel into the ROMM RGB gamut
		void MoveXYZIntoROMMRGBGamut( 
			XYZPel& p								///< [in/out] Color to move, resultant CIE_XYZ value
			);

		//! Saves the given color to the buffer
		template< class T >
		void SerializeRGBPel( 
			const T& rgb,							///< [in] Pel to serialize
			IWriteBuffer& buffer					///< [in] Buffer to serialize into
			)
		{
			buffer.ResizeForMore(sizeof(Chel)*3);
			buffer.setDouble( rgb.r );
			buffer.setDouble( rgb.g );
			buffer.setDouble( rgb.b );
		}

		//! Saves the given color to the buffer
		void SerializeXYZPel( 
			const XYZPel& xyz,						///< [in] Pel to serialize
			IWriteBuffer& buffer					///< [in] Buffer to serialize into
			);

		//! Gets a Pel from a buffer
		template< class T >
		void DeserializeRGBPel(
			T& rgb,									///< [out] The deserialized pel
			IReadBuffer& buffer						///< [in] Buffer to read from
			)
		{
			rgb.r = buffer.getDouble();
			rgb.g = buffer.getDouble();
			rgb.b = buffer.getDouble();
		}

		//! Gets a Pel from a buffer
		void DeserializeXYZPel( 
			XYZPel& xyz,							///< [out] The deserialized pel
			IReadBuffer& buffer						///< [in] Buffer to read from
			);

		//! Premultiplies RGB Pel
		//! Integral RGB/alpha inputs must be in [0,max_val]. Floating RGB
		//! must be finite and alpha in [0,max_val]; RGB may retain HDR range.
		//! Alpha is preserved. The positive normalization maximum is explicit.
		template< class T, int max_val >
		T PremultiplyAlphaRGB( const T& rgba )
		{
			static_assert(max_val>0,"alpha normalization maximum must be positive");
			using Channel=decltype(rgba.r);
			T ret;
			if constexpr(std::is_integral<Channel>::value) {
				const auto multiply=[&rgba](Channel value) {
					return static_cast<Channel>((static_cast<std::uint64_t>(value)*static_cast<std::uint64_t>(rgba.a))/
						static_cast<std::uint64_t>(max_val));
				};
				ret.r=multiply(rgba.r);ret.g=multiply(rgba.g);ret.b=multiply(rgba.b);
			} else {
				// Normalize bounded mantissas, then restore the combined exponent.
				// Forming alpha/max first can erase a tiny alpha; multiplying RGB
				// by alpha first can overflow despite a representable final result.
				int alphaPower=0;
				const long double alphaMantissa=std::frexp(static_cast<long double>(rgba.a),&alphaPower);
				const long double normalizedAlpha=alphaMantissa/static_cast<long double>(max_val);
				const auto multiply=[normalizedAlpha,alphaPower](Channel value) {
					int rgbPower=0;
					const long double rgbMantissa=std::frexp(static_cast<long double>(value),&rgbPower);
					return static_cast<Channel>(std::ldexp(rgbMantissa*normalizedAlpha,rgbPower+alphaPower));
				};
				ret.r=multiply(rgba.r);ret.g=multiply(rgba.g);ret.b=multiply(rgba.b);
			}
			ret.a=rgba.a;
			return ret;
		}
	}
}

#endif
