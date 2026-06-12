//////////////////////////////////////////////////////////////////////
//
//  ProceduralDescriptors.h - Plain public parameter blocks for the
//  procedural construction factories (guilloché disk / oxide painter,
//  profile sweep, along-path instances).  Lives in Interfaces so IJob
//  and RISE_API can share
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
	class IGeometry;

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

	//! Every knob of the guilloché field + the disk bake (mesh_n, disp).
	//! Scene-chunk parameter names match the Python flags (snake_case).
	struct GuillocheDiskDescriptor
	{
		int    pattern;					//!< GuillochePatternKind
		double radius;					//!< disk radius (world units)
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

		GuillocheDiskDescriptor() :
			pattern( eGuillochePatternUniform ),
			radius( 20.6 ),
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

	//! General profile-sweep parameters: an arbitrary CLOSED 2D profile
	//! polygon swept along an arbitrary 3D Catmull-Rom path with
	//! rotation-minimizing frames.  Tubes, rails, mouldings, bands, cables.
	//! The scene chunk supplies the profile as repeatable
	//! `profile_point <x> <h>` lines (x = local binormal/width axis,
	//! h = local normal/height axis) and the path as repeatable
	//! `point <x> <y> <z>` lines.
	struct SweepDescriptor
	{
		const double* profilePoints;	//!< x0 h0 x1 h1 ... (pairs; CLOSED polygon, >= 3)
		unsigned int  numProfilePoints;	//!< number of (x, h) PAIRS
		const double* pathPoints;		//!< x0 y0 z0 x1 y1 z1 ... (triples)
		unsigned int  numPathPoints;	//!< number of (x, y, z) TRIPLES (>= 2)
		int    nLen;					//!< samples along the path
		double endScaleX;				//!< profile x scale at the path end (linear taper from 1)
		double endScaleY;				//!< profile h scale at the path end (linear taper from 1)
		bool   capStart;				//!< triangulate the start cross-section closed
		bool   capEnd;					//!< triangulate the end cross-section closed
		double frameHintX, frameHintY, frameHintZ;	//!< initial binormal hint (0,0,0 = auto: world axis most perpendicular to the start tangent)

		SweepDescriptor() :
			profilePoints( 0 ), numProfilePoints( 0 ),
			pathPoints( 0 ), numPathPoints( 0 ),
			nLen( 64 ),
			endScaleX( 1.0 ), endScaleY( 1.0 ),
			capStart( true ), capEnd( true ),
			frameHintX( 0.0 ), frameHintY( 0.0 ), frameHintZ( 0.0 )
		{
		}
	};

	//! General along-path instancing: a named TEMPLATE geometry (tessellated
	//! once through the universal TessellateToMesh contract) stamped along a
	//! 3D Catmull-Rom path at arc-length pitch.  Fence posts, rivets, beads,
	//! stitches, chain links.  The template's local +Y aligns with the path
	//! tangent (rotated by slant about the frame normal), +Z with the frame
	//! normal, +X with the binormal.
	struct PathInstancesDescriptor
	{
		const IGeometry* pGeometry;		//!< resolved template geometry (NOT addref'd here; consumed synchronously)
		const double* pathPoints;		//!< x0 y0 z0 ... (triples, >= 2)
		unsigned int  numPathPoints;	//!< number of (x, y, z) TRIPLES
		int    nLen;					//!< arc-length walk resolution (path samples)
		double pitch;					//!< arc-length distance between instances (> 0)
		double phase;					//!< arc-length distance before the first instance (< 0 = pitch/2, the centred default)
		double slantDeg;				//!< rotation of the template about the frame normal (degrees)
		double scale;					//!< uniform template scale
		unsigned int detail;			//!< TessellateToMesh detail for the template (clamped 3..256)
		double frameHintX, frameHintY, frameHintZ;	//!< initial binormal hint (0,0,0 = auto), as in SweepDescriptor

		PathInstancesDescriptor() :
			pGeometry( 0 ),
			pathPoints( 0 ), numPathPoints( 0 ),
			nLen( 256 ),
			pitch( 1.0 ), phase( -1.0 ), slantDeg( 0.0 ), scale( 1.0 ),
			detail( 16 ),
			frameHintX( 0.0 ), frameHintY( 0.0 ), frameHintZ( 0.0 )
		{
		}
	};
}

#endif
