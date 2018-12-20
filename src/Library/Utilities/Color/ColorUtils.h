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


namespace RISE
{
	namespace ColorUtils
	{

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
		template< class T, int max_val >
		T PremultiplyAlphaRGB( const T& rgba )
		{
			T ret;
			ret.r = rgba.r * max_val / rgba.a;
			ret.g = rgba.g * max_val / rgba.a;
			ret.b = rgba.b * max_val / rgba.a;
			return ret;
		}
	}
}

#endif
