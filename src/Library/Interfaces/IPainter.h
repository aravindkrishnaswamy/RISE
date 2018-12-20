//////////////////////////////////////////////////////////////////////
//
//  IPainter.h - Declaration of the abstract interface IPainter
//  which is what all painters must implement
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: November 19, 2001
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IPAINTER_
#define IPAINTER_

#include "../Utilities/Color/Color.h"
#include "IFunction2D.h"
#include "IKeyframable.h"

namespace RISE
{
	class RayIntersectionGeometric;

	//! A painter determines the spectral properties of a material.  It takes
	//! intersection information and from that derives the spectral properties
	//! either as an RISEPel or as the amplitude of a specific wavelength
	class IPainter : 
		public virtual IFunction2D,
		public virtual IKeyframable
	{
	protected:
		IPainter() {};
		virtual ~IPainter(){};

	public:
		//!
		//! This is what a painter essentially does.  It takes
		//! some parameters as to the current RayEngineState, and returns
		//! some color, how this is done is left to the individual
		//! painters
		//!
		/// \return The computed color as an RISEPel
		virtual RISEPel GetColor(
			const RayIntersectionGeometric& ri					///< [in] Geometric intersection details 
			) const = 0;

		//! 
		//! This function is almost the same as the above one, however it returns
		//! the response of the painter to a particular wavelength of light.  Note
		//! that currently only the spectral painter class can do anything of use
		//! with this.  The default implementation returns 0
		//!
		/// \return The intensity for the particular wavelength
		virtual Scalar GetColorNM( 
			const RayIntersectionGeometric& ri,					///< [in] Geometric intersection details 
			const Scalar nm										///< [in] Wavelength to process
			) const = 0;

		//!
		//! This function is also similar to the above ones, however it returns the entire spectrum
		//! rather than just the value at the particular wavelength
		//!
		/// \return The computed color as a spectral packet
		virtual SpectralPacket GetSpectrum( 
			const RayIntersectionGeometric& ri 					///< [in] Geometric intersection details 
			) const = 0;

	};
}

#include "../Intersection/RayIntersectionGeometric.h"

#endif
