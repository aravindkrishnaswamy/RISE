//////////////////////////////////////////////////////////////////////
//
//  ProceduralDescriptors.h - Plain public parameter blocks for the
//  procedural construction factories (profile sweep, along-path
//  instances).  Lives in Interfaces so IJob and RISE_API can share
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
