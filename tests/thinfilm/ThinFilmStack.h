//////////////////////////////////////////////////////////////////////
//
//  ThinFilmStack.h - Description of an air / film(s) / substrate
//    optical stack, plus small builders.  This is the shared input to
//    both the characteristic-matrix TMM reference (TmmReference.h) and
//    the single-film Airy closed form (AiryReference.h).
//
//    This is a STANDALONE optics reference (the ground-truth oracle for
//    the thin-film feature, see docs/THIN_FILM_INTERFERENCE.md).  It is
//    header-only and lives under tests/ so it carries NO dependency on
//    the renderer.  All optics are done in std::complex<double> for
//    clarity over speed -- this is the oracle, not the production path.
//
//    Conventions (must match TmmReference.h and AiryReference.h):
//      * Complex index  N = n + i*k,  k >= 0 for an absorbing medium.
//      * Time convention chosen so absorbing media DECAY: the cosTheta
//        branch is picked with Im(N*cosTheta) >= 0 (a decaying, not a
//        growing, evanescent wave).  See TmmReference.h / AiryReference.h.
//      * Wavelength and film thickness are both in nanometres (nm); only
//        their ratio enters the phase, so any consistent length unit
//        works, but nm is the documented choice.
//      * Media order: medium 0 is the ambient (incidence side), then an
//        ordered list of films, then a semi-infinite substrate.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THINFILM_THINFILMSTACK_H
#define THINFILM_THINFILMSTACK_H

#include <complex>
#include <vector>

namespace RISE
{
	namespace ThinFilmReference
	{
		typedef std::complex<double> Complex;

		//! A single film layer: its complex index and physical thickness.
		struct Layer
		{
			Complex	index;			//!< N = n + i*k  (k >= 0 absorbing)
			double	thickness_nm;	//!< physical thickness, nm

			Layer()
				: index( 1.0, 0.0 )
				, thickness_nm( 0.0 )
			{}

			Layer( const Complex& n, double d_nm )
				: index( n )
				, thickness_nm( d_nm )
			{}
		};

		//! An air / film(s) / substrate stack.  The ambient and substrate
		//! are semi-infinite; the films are an ordered list from the
		//! ambient side toward the substrate.
		struct Stack
		{
			Complex				ambientIndex;	//!< medium 0 (incidence side); air = 1
			std::vector<Layer>	films;			//!< ordered ambient -> substrate
			Complex				substrateIndex;	//!< semi-infinite bottom medium

			Stack()
				: ambientIndex( 1.0, 0.0 )
				, substrateIndex( 1.0, 0.0 )
			{}
		};

		//! Builds a complex index from real n and extinction k.  k is
		//! forced non-negative (the absorbing convention).
		inline Complex MakeIndex( double n, double k = 0.0 )
		{
			return Complex( n, k >= 0.0 ? k : -k );
		}

		//! Builds a bare-substrate stack (no film) with the given ambient
		//! and substrate indices.
		inline Stack MakeBareStack( const Complex& ambient, const Complex& substrate )
		{
			Stack s;
			s.ambientIndex = ambient;
			s.substrateIndex = substrate;
			s.films.clear();
			return s;
		}

		//! Builds a single-film stack: ambient / one film / substrate.
		inline Stack MakeSingleFilmStack(
			const Complex& ambient,
			const Complex& filmIndex, double filmThickness_nm,
			const Complex& substrate )
		{
			Stack s;
			s.ambientIndex = ambient;
			s.substrateIndex = substrate;
			s.films.clear();
			s.films.push_back( Layer( filmIndex, filmThickness_nm ) );
			return s;
		}

		//! Convenience: air ambient (N = 1).
		inline Complex Air()
		{
			return Complex( 1.0, 0.0 );
		}
	}
}

#endif
