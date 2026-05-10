//////////////////////////////////////////////////////////////////////
//
//  Film.h - Concrete implementation of IFilm.
//
//  A Film is a small value-object: it carries the pixel grid
//  description (width, height, pixelAR) that the rasterizer reads
//  out of the scene at render time.  The internal accumulator
//  classes in this same directory (FilteredFilm, ProgressiveFilm,
//  SplatFilm) are *algorithmic* containers — they share the word
//  "film" with this class but are unrelated.  IFilm/Film is the
//  scene-level format descriptor; the *Film accumulators are
//  rasterizer-internal scratch storage.
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef FILM_
#define FILM_

#include "../Interfaces/IFilm.h"
#include "../Utilities/Reference.h"

namespace RISE
{
	// Default Film dimensions — qHD = 960 x 540 with square pixels.
	// Single source of truth referenced by both Job::InitializeContainers
	// (the unconditional default) AND FilmAsciiChunkParser (the per-param
	// fallback when a `film` chunk omits a value).  Keep these in sync —
	// a `film { }` chunk should produce the same Film as omitting the
	// chunk entirely.
	static constexpr unsigned int kDefaultFilmWidth   = 960;
	static constexpr unsigned int kDefaultFilmHeight  = 540;
	static constexpr double       kDefaultFilmPixelAR = 1.0;

	// Upper sanity bounds on Film dims.  Absurd values (e.g. 19200x10800
	// from a typo, or wrap-arounds from a negative authored value
	// funneling through unsigned conversion) are rejected by
	// Job::SetFilm rather than silently allocating gigabytes of frame
	// buffer.  32768 leaves headroom above 8K (7680x4320).
	static constexpr unsigned int kMaxFilmWidth  = 32768;
	static constexpr unsigned int kMaxFilmHeight = 32768;

	namespace Implementation
	{
		class Film :
			public virtual IFilm,
			public virtual Reference
		{
		protected:
			// Members are NON-const so Resize() can mutate them in
			// place — the SceneEditController preview-scale path uses
			// this to avoid allocating a fresh Film per interactive
			// frame.  Outside that path, Films are typically created
			// once (qHD default in InitializeContainers, or one per
			// scene-load via the `film` chunk) and never resized.
			unsigned int	width;
			unsigned int	height;
			Scalar			pixelAR;

			virtual ~Film(){};

		public:
			Film(
				const unsigned int width_,
				const unsigned int height_,
				const Scalar pixelAR_
				);

			unsigned int GetWidth( ) const override { return width; }
			unsigned int GetHeight( ) const override { return height; }
			Scalar GetPixelAR( ) const override { return pixelAR; }

			void Resize( unsigned int width_, unsigned int height_, Scalar pixelAR_ ) override
			{
				width   = width_;
				height  = height_;
				pixelAR = pixelAR_;
			}
		};
	}
}

#endif
