//////////////////////////////////////////////////////////////////////
//
//  IFilm.h - Declaration of abstract Film class.
//
//  A Film describes the pixel grid the rasterizer produces — how the
//  continuous image formed by the camera's optics is discretized.  It
//  is the missing peer of ICamera in the camera/film/output split:
//
//    Camera (optics + sensor read-out)
//      → Film (pixel grid: width, height, pixelAR)
//        → FileRasterizerOutput (display transform, tone-map, file)
//
//  Pre-2026-05 RISE bundled width/height/pixelAR onto the camera
//  itself.  Splitting them onto a Film lets the same camera be
//  rendered at multiple resolutions (CLI override / interactive
//  preview) without re-authoring the scene.  See
//  docs/ARCHITECTURE.md "Camera / Film / Output separation".
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: May 10, 2026
//  Tabs: 4
//  Comments:
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef IFILM_
#define IFILM_

#include "IReference.h"
#include "../Utilities/Math3D/Math3D.h"

namespace RISE
{
	//! A Film describes the pixel grid that a render produces.
	//! It owns width, height and pixel aspect ratio — the
	//! discretization of the continuous image formed by the
	//! camera's optics.  A Scene has at most one active Film at a
	//! time; rasterizers query it (rather than the camera) for the
	//! grid dimensions.
	class IFilm :
		public virtual IReference
	{
	protected:
		IFilm( ){};
		virtual ~IFilm( ){};

	public:
		/// \return The image width in pixels
		virtual unsigned int GetWidth( ) const = 0;

		/// \return The image height in pixels
		virtual unsigned int GetHeight( ) const = 0;

		/// \return The pixel aspect ratio (width / height of a single
		/// pixel).  1.0 for square pixels (the common case); != 1.0
		/// for anamorphic / NTSC-style non-square pixel grids.
		virtual Scalar GetPixelAR( ) const = 0;

		//! In-place dim mutation.  Used by IScenePriv::ResizeFilm to
		//! avoid allocating a new IFilm per preview-scale frame in the
		//! interactive editor — the rest-Film and the scaled-Film are
		//! the SAME object whose dims toggle on entry/exit of a
		//! preview-scale pass.  Caller is responsible for re-syncing
		//! dependents (cameras, AcquireRenderImage's persistent
		//! buffer) — see Scene::ResizeFilm.
		virtual void Resize( unsigned int width, unsigned int height, Scalar pixelAR ) = 0;
	};
}

#endif
