//////////////////////////////////////////////////////////////////////
//
//  ProceduralDescriptors.h - Plain public parameter blocks for the
//  procedural construction factories (guilloché dial / oxide painter,
//  swept band).  Lives in Interfaces so IJob and RISE_API can share
//  the types without touching Implementation headers.
//
//  These mirror the retired Python bakers' argparse surfaces; the
//  defaults are the bakers' defaults, and the per-pattern overrides
//  (gen_dials.sh's blessed parameter sets) live in the scene chunks.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef PROCEDURAL_DESCRIPTORS_
#define PROCEDURAL_DESCRIPTORS_

namespace RISE
{
	//! Guilloché pattern selector (string names in the scene chunk:
	//! uniform | lightning | radial | iris | swirl | varwidth).
	enum GuillochePatternKind
	{
		eGuillochePatternUniform   = 0,
		eGuillochePatternLightning = 1,
		eGuillochePatternRadial    = 2,
		eGuillochePatternIris      = 3,
		eGuillochePatternSwirl     = 4,
		eGuillochePatternVarwidth  = 5
	};

	//! Every knob of the guilloché field + the dial bake (mesh_n, disp).
	//! Scene-chunk parameter names match the Python flags (snake_case).
	struct GuillocheDialDescriptor
	{
		int    pattern;					//!< GuillochePatternKind
		double radius;					//!< dial radius (world units)
		int    meshN;					//!< grid samples across the diameter (bake density)
		double disp;					//!< relief amplitude (world units)
		int    numArms;
		double swirl;					//!< angular lean centre->rim (rad)
		double seamJag;
		double seamJagFreq;
		double cell;					//!< woven cell world size (land-to-land)
		double gridAmp;
		double petalAmp;
		double gridE0, gridE1;
		double petalE0, petalE1;
		double base;
		double landLevel;
		double reliefDepth;
		double centerRadius;
		int    cellMode;				//!< 0 freqblend | 1 select
		double lightningCellScale;
		double lightningLo, lightningHi;
		double lightningRelief;
		double zigzagAmp;
		double zigzagFreq;
		double fieldCell;
		int    fieldFrame;				//!< 0 global | 1 radial
		int    boltStyle;				//!< 0 rung | 1 cube | 2 woven
		double rungLen;
		double rungWidth;
		double irisAperture;
		double irisSwirl;
		double irisEdge;
		double swirlTurns;

		GuillocheDialDescriptor() :
			pattern( eGuillochePatternUniform ),
			radius( 20.6 ), meshN( 560 ), disp( 0.42 ),
			numArms( 12 ), swirl( 0.0 ),
			seamJag( 0.16 ), seamJagFreq( 3.0 ),
			cell( 0.9 ), gridAmp( 0.85 ), petalAmp( 0.30 ),
			gridE0( 0.12 ), gridE1( 0.5 ),
			petalE0( 0.0 ), petalE1( 0.82 ),
			base( 0.15 ), landLevel( 0.45 ), reliefDepth( 0.85 ),
			centerRadius( 0.03 ),
			cellMode( 0 ), lightningCellScale( 0.6 ),
			lightningLo( 0.30 ), lightningHi( 0.72 ), lightningRelief( 0.0 ),
			zigzagAmp( 0.16 ), zigzagFreq( 3.0 ),
			fieldCell( 0.45 ), fieldFrame( 0 ), boltStyle( 0 ),
			rungLen( 1.2 ), rungWidth( 1.5 ),
			irisAperture( 0.13 ), irisSwirl( 0.5 ), irisEdge( 0.6 ),
			swirlTurns( 6.0 )
		{
		}
	};

	//! Swept-band (watch-strap) parameters -- strap_mesh_gen.py's surface.
	//! The path is a Catmull-Rom spline through (y, z) control points in the
	//! band's local frame (x = width axis); the scene chunk supplies them as
	//! repeatable `point <y> <z>` lines.
	struct SweptBandDescriptor
	{
		const double* pathPoints;		//!< y0 z0 y1 z1 ... (pairs)
		unsigned int  numPathPoints;	//!< number of (y, z) PAIRS
		int    nLen;					//!< samples along the path
		int    nWid;					//!< samples across the width
		double width;					//!< band width at the start (lug)
		double endWidth;				//!< band width at the free end (taper)
		double thickness;
		double crown;					//!< extra centre doming of the top surface
		double edgePow;					//!< superellipse edge exponent
		double groove;					//!< stitch channel depth in the top surface
		double stitchInset;				//!< stitch row inset from each edge (fraction of width)
		double stitchPitch;				//!< distance between stitches along the band
		double stitchLen;				//!< visible thread length per stitch
		double stitchR;					//!< thread radius
		double stitchAngleDeg;			//!< saddle slant (mirrored per row)
		bool   emitStitches;			//!< false = the band mesh, true = the thread mesh

		SweptBandDescriptor() :
			pathPoints( 0 ), numPathPoints( 0 ),
			nLen( 140 ), nWid( 14 ),
			width( 25.26 ), endWidth( 20.2 ),
			thickness( 3.0 ), crown( 0.55 ), edgePow( 8.0 ), groove( 0.16 ),
			stitchInset( 0.085 ), stitchPitch( 2.4 ), stitchLen( 1.35 ),
			stitchR( 0.14 ), stitchAngleDeg( 16.0 ),
			emitStitches( false )
		{
		}
	};
}

#endif
