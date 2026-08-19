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
		const double* pointWidths;	//!< OPTIONAL per-control-point width (x/binormal axis) multipliers, one per path point (count <= numPathPoints; missing padded with 1.0).  Catmull-Rom interpolated onto the path samples and composed MULTIPLICATIVELY with the linear end_scale_x taper.  NULL/0 = uniform width (byte-identical to the linear-taper-only sweep)
		unsigned int  numPointWidths;	//!< number of pointWidths entries (0 = per-station width OFF)
		const double* pointScales;	//!< OPTIONAL per-control-point UNIFORM (both profile axes) scale multipliers, one per path point (count <= numPathPoints; missing padded with 1.0).  Catmull-Rom interpolated onto the path samples with the SAME sampler as pointWidths, and composed MULTIPLICATIVELY with pointWidths (x only) and the linear end_scale taper -- use pointScales for a ROUND varying radius (a tapered tentacle/tendril); pointWidths remains the deliberate-flattening (x-only) control.  NULL/0 = uniform scale (byte-identical to a sweep without it)
		unsigned int  numPointScales;	//!< number of pointScales entries (0 = per-station scale OFF)
		bool   pathClosed;				//!< sweep a CLOSED loop: periodic Catmull-Rom path sampling (segments wrap point[n-1]->point[0]), a periodic rotation-minimizing frame with a holonomy correction removing the seam twist, and cyclic ring stitching (no end caps).  Requires numPathPoints >= 3 and the first/last point NOT authored coincident.  end_scale_x/end_scale_y must stay 1.0 (point_scale IS allowed -- it samples periodically like point_width).  Default FALSE = the existing open-path behaviour, byte-identical

		SweepDescriptor() :
			profilePoints( 0 ), numProfilePoints( 0 ),
			pathPoints( 0 ), numPathPoints( 0 ),
			nLen( 64 ),
			endScaleX( 1.0 ), endScaleY( 1.0 ),
			capStart( true ), capEnd( true ),
			frameHintX( 0.0 ), frameHintY( 0.0 ), frameHintZ( 0.0 ),
			pointWidths( 0 ), numPointWidths( 0 ),
			pointScales( 0 ), numPointScales( 0 ),
			pathClosed( false )
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

	//! Surface-of-revolution (lathe) parameters: an OPEN 2D profile
	//! polyline authored in the half-plane (r = radius from the axis,
	//! h = height along it) revolved about a world axis.  Vases, bottles,
	//! turned table/chair legs, lamp bases, pedestals, knobs, goblets --
	//! the furniture-and-vessel vocabulary.  A silhouette is the single
	//! most reliable thing to author by hand (or to generate): only the
	//! outline is described, and the revolution supplies the solid.
	//!
	//! Distinct from SweepDescriptor: a sweep moves a CLOSED profile
	//! polygon along an arbitrary 3D path; a lathe spins an OPEN profile
	//! polyline about a fixed straight axis.  A profile point with r == 0
	//! sits ON the axis, so its ring collapses to a single POLE vertex
	//! (a triangle fan, no degenerate quad band) -- which is what makes a
	//! vase closed at both ends watertight with no caps at all.  An
	//! INTERIOR pole (a waisted profile pinched to r == 0 mid-way) gets one
	//! pole vertex per adjacent band, since the two sides of a pinch demand
	//! opposite axial normals.  The baked mesh is DOUBLE-SIDED, so an
	//! emissive material on a lathe radiates from both faces.
	//!
	//! CONSEQUENCE of that split, worth knowing before reaching for a fix:
	//! displaced_geometry over a PINCHED lathe TEARS at the waist.  The two
	//! pinch poles are coincident but carry exactly opposite normals, so
	//! displacement pushes them apart by 2 * disp_scale and opens a crack.
	//! This is inherent to split normals -- sweep_geometry's duplicate-a-
	//! profile-point hard-edge idiom has it too -- and sharing one vertex
	//! instead would trade the crack for a shading normal in the wrong
	//! half-space on one of the two bands, which is the very defect the
	//! split exists to prevent.  Displace an UNPINCHED profile, or keep
	//! disp_scale small relative to the waist, rather than "fixing" it.
	struct LatheDescriptor
	{
		const double* profilePoints;	//!< r0 h0 r1 h1 ... (pairs; OPEN polyline, >= 2, every r >= 0, not ALL r == 0)
		unsigned int  numProfilePoints;	//!< number of (r, h) PAIRS
		int    axis;					//!< revolution axis: 0 = world X, 1 = world Y (the default, matching cylinder_geometry and the SDF roundcone/capsule local-Y convention), 2 = world Z
		double sweepDegrees;			//!< angular extent of the revolution, in (0, 360].  360 closes the surface on itself (the last radial column IS the first -- no duplicated seam); anything less is a section/cutaway and gets two flat radial caps
		int    nRadial;					//!< radial SEGMENTS around the axis (clamped 3..2048).  A full 360 sweep emits nRadial columns (wrapping); a partial sweep emits nRadial + 1
		bool   smooth;					//!< TRUE: one row per profile point carrying the AVERAGE of its two adjacent segment normals (a turned surface reads smooth; duplicate a profile point to harden one edge).  FALSE: two rows per profile segment carrying that segment's flat normal (fully faceted)

		LatheDescriptor() :
			profilePoints( 0 ), numProfilePoints( 0 ),
			axis( 1 ), sweepDegrees( 360.0 ), nRadial( 48 ), smooth( true )
		{
		}
	};
}

#endif
