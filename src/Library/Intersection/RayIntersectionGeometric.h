//////////////////////////////////////////////////////////////////////
//
//  RayIntersectionGeometric.h - A class that describes the geometric
//  aspects of ray intersection
//
//  Author: Aravind Krishnaswamy
//  Date of Birth: September 17, 2002
//  Tabs: 4
//  Comments:  
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RAY_INTERSECTION_GEOMETRIC_
#define RAY_INTERSECTION_GEOMETRIC_

#include "../Utilities/Ray.h"
#include "../Utilities/OrthonormalBasis3D.h"
#include "../Utilities/Color/Color.h"
#include "../Interfaces/ISurfaceSignalProvider.h"

namespace RISE
{
	class ICamera;

	//! Per-object / per-instance Cycles Object Info attributes (default all-zero).
	struct ObjectInfoData
	{
		float	color[3];
		float	alpha;
		float	objectIndex;
		float	random;
		float	particleRandom;

		ObjectInfoData() :
			alpha( 0.0f ),
			objectIndex( 0.0f ),
			random( 0.0f ),
			particleRandom( 0.0f )
		{
			color[0] = color[1] = color[2] = 0.0f;
		}
	};

	//! Hit-time surface derivatives populated by geometries that know local parameters.
	struct SurfaceDerivativesInfo
	{
		Vector3 dpdu;
		Vector3 dpdv;
		Vector3 dndu;
		Vector3 dndv;
		bool    valid;  // true if dpdu/dpdv/dndu/dndv are populated

		//! World characteristic length (bbox diagonal scaled by |det M|^(1/3), default 1.0)
		//! used to normalize mean curvature `curv`; populated when SurfaceCurvatureDemand::Any().
		Scalar  scaleHint;

		//! Direct world-space mean curvature H (positive = convex, 1/length; independent of `valid`).
		Scalar  curvature;
		bool    curvatureValid;

		//! 2x2 Jacobian mapping derivative params (u,v) to texture coord (s,t) in `ptCoord`;
		//! SolveFootprintUV withholds UV differentials unless `texChartValid` is true.
		Scalar  dsdu, dsdv, dtdu, dtdv;
		bool    texChartValid;

		SurfaceDerivativesInfo() :
		dpdu( Vector3(0,0,0) ), dpdv( Vector3(0,0,0) ),
		dndu( Vector3(0,0,0) ), dndv( Vector3(0,0,0) ),
		valid( false ),
		scaleHint( 1.0 ),
		curvature( 0 ), curvatureValid( false ),
		dsdu( 1 ), dsdv( 0 ), dtdu( 0 ), dtdv( 1 ), texChartValid( false )
		{
		}
	};

	//! Screen-space pixel footprint on the tangent plane and optional UV Jacobian.
	//! Invariant: `valid => widthValid`.
	//! - `dpdx`/`dpdy` are 1-pixel surface displacements (promoted linearly to world space
	//!   by Object/CSGObject); `worldWidth` is their mean magnitude in world units.
	//! - `objectWidth` is the pre-promotion mean width in `ptObjIntersec`'s frame.
	//! - `dudx`..`dvdy` are UV partials wrt screen pixel (x,y), valid only when `valid`.
	struct TextureFootprint
	{
		Scalar  dudx, dudy;
		Scalar  dvdx, dvdy;
		Vector3 dpdx, dpdy;
		Scalar  worldWidth;
		Scalar  objectWidth;	// pre-promotion width in ptObjIntersec's frame
		bool    valid;			// UV Jacobian is usable
		bool    widthValid;		// dpdx / dpdy / worldWidth / objectWidth are usable

		TextureFootprint() :
		dudx( 0 ), dudy( 0 ), dvdx( 0 ), dvdy( 0 ),
		dpdx( Vector3(0,0,0) ), dpdy( Vector3(0,0,0) ),
		worldWidth( 0 ), objectWidth( 0 ), valid( false ), widthValid( false )
		{
		}
	};

	//! Describes the current state of the rasterizer
	struct RasterizerState
	{
		unsigned int x;						// Which pixel on the row we are processing
		unsigned int y;						// Which row we are processing

		bool operator==( const RasterizerState& other ) {
			return (x==other.x && y==other.y);
		}
	};

	static const RasterizerState nullRasterizerState = {0};

	class RayIntersectionGeometric
	{
	public:
		Ray							ray;			// intersecting ray
		RasterizerState			rast;			// rasterizer state
		bool						bHit;			// true on intersection
		Scalar						range;			// distance to entry intersection
		Scalar						range2;			// distance to exit intersection
		Vector3						vNormal;		// shading normal (Phong/modifier-perturbed)
		Vector3						vNormal2;		// shading normal at exit
		//! Unperturbed geometric normals at entry/exit (flat face normal on meshes;
		//! identical to vNormal on smooth analytics). Use for physical sidedness tests.
		Vector3						vGeomNormal;
		Vector3						vGeomNormal2;

		//! True if the geometry flipped vGeomNormal to oppose the incoming ray
		//! (double-sided meshes, back-face Bezier/ClippedPlane, HairGeometry, CSG pass-through).
		//! Recover true winding normal via UnflippedGeomNormal() when !bGeomNormalRayDerived.
		bool						bGeomNormalOrientedToRay;

		//! True when vGeomNormal is fabricated from the ray direction (HairGeometry only).
		//! Consumers asking true-side questions must check HasTrueGeomSide() and skip.
		bool						bGeomNormalRayDerived;

		//! True when a double-sided/back-face flip occurred on an open/uncertified 2-D sheet
		//! (non-watertight double-sided mesh, ClippedPlane; not Bezier/Hair). Scoped to BSSRDFEntryFacing().
		bool						bOpenSheet;

		//! True only when the hit object provably encloses no volume (ClippedPlane, displaced
		//! convex quad, or certified open-sheet mesh). Always cleared by CSGObject::IntersectRay.
		bool						bProvablyNoInterior;

		//! Arrival facing for rebuilt path vertices whose `ray` is re-aimed per query:
		//! 0 = live hit (read from `ray`), -1 = vGeomNormal opposed arrival, +1 = aligned.
		//! Reset to 0 when spawning a new live ray from a copied record.
		signed char					arrivalGeomFacing;

		//! True when reported vGeomNormal opposed the arriving ray.
		inline bool GeomNormalOpposesArrival() const
		{
			return ( arrivalGeomFacing != 0 ) ? ( arrivalGeomFacing < 0 )
				: ( Vector3Ops::Dot( vGeomNormal, ray.Dir() ) < 0 );
		}

		//! Signed cosine in the shading hemisphere oriented to -ray.Dir().
		inline Scalar RayFacingShadingCosine( const Vector3& direction ) const
		{
			const Scalar c = Vector3Ops::Dot( vNormal, direction );
			return Vector3Ops::Dot( vNormal, ray.Dir() ) > Scalar(0) ? -c : c;
		}

		//! True winding-order geometric normal (undoes orient-to-ray flip unless ray-derived).
		//! Pair with HasTrueGeomSide() when hair hits are possible.
		inline Vector3 UnflippedGeomNormal() const
		{
			return ( bGeomNormalOrientedToRay && !bGeomNormalRayDerived )
				? -vGeomNormal : vGeomNormal;
		}

		//! Signed facing of UnflippedGeomNormal() against `d` (<0 = entry, >0 = exit).
		inline Scalar TrueGeomFacing( const Vector3& d ) const
		{
			return Vector3Ops::Dot( UnflippedGeomNormal(), d );
		}

		//! True when vGeomNormal represents a genuine two-sided surface (false for hair).
		inline bool HasTrueGeomSide() const
		{
			return !bGeomNormalRayDerived;
		}

		//! BSSRDF admission facing: ray-facing on open sheets (`bOpenSheet`), else TrueGeomFacing().
		inline Scalar BSSRDFEntryFacing( const Vector3& d ) const
		{
			return bOpenSheet
				? Vector3Ops::Dot( vGeomNormal, d )
				: TrueGeomFacing( d );
		}

		Point2						ptCoord;		// primary texture coordinates (TEXCOORD_0)

		//! Secondary texture coordinates (TEXCOORD_1); fall back to ptCoord when !bHasTexCoord1.
		Point2						ptCoord1;
		bool						bHasTexCoord1;

		//! True once an IUVGenerator overrides ptCoord (enforces CSG precedence:
		//! operand generator > composite generator > native UV).
		bool						bUVGeneratorApplied;

		Point3						ptIntersection;	// world-space entry point
		Point3						ptExit;			// world-space exit point

		Point3						ptObjIntersec;	// object-space entry point
		Point3						ptObjExit;		// object-space exit point

		//! Borrowed world->object matrix (`&Object::m_mxInvFinalTrans`) for `ptObjIntersec`'s
		//! frame (use linear part via Vector3Ops::Transform); null on CSG/synthetic hits.
		const Matrix4*				pmxWorldToObject;

		OrthonormalBasis3D			onb;			// orthonormal basis at intersection

		//! Reference-counted custom payload (legacy 3DS Max shader interop).
		IReference*					pCustom;

		Scalar						glossyFilterWidth;	///< Accumulated glossy filter blur from StabilityConfig (0 = off)

		//! Incident medium IOR stamped from IORStack::top() (default 1.0 = air/vacuum).
		Scalar						ambientIOR;

		//! Hit-time surface derivatives (fall back to IGeometry::ComputeSurfaceDerivatives if !valid).
		SurfaceDerivativesInfo		derivatives;

		//! Geometry- and scene-derived shading signal channel (object-space provider + world-space
		//! scene pointers; mirrored on BDPTVertex and probed on sampled emitters).
		SurfaceSignalInfo			signals;

		//! Pixel footprint for texture mip LOD and expression VM `fw`.
		TextureFootprint			txFootprint;

		//! Interpolated per-vertex color (valid iff bHasVertexColor).
		RISEPel						vColor;
		bool						bHasVertexColor;

		//! World-space vertex tangent (promoted forward; bitangent = sign * cross(vNormal, vTangent)).
		Vector3						vTangent;
		Scalar						bitangentSign;	// +1 or -1
		bool						bHasTangent;

		//! If true, Object::IntersectRay builds `onb` from `vShadingTangent` (if bHasShadingTangent)
		//! or world-X projection instead of arbitrary CreateFromW().
		bool						bShadingTangentFromGeometry;

		//! Separate shader socket direction (promoted forward, keeping raw normal component),
		//! unperturbed UV/global normal-map frame, and pre-modifier coat-decode frame.
        Vector3 vShaderDirection;
        bool bHasShaderDirection;
        OrthonormalBasis3D normalMapOnb;
        bool bHasNormalMapFrame;
        OrthonormalBasis3D coatDecodeOnb;
        bool bHasCoatDecodeOnb;
		//! Geometry-supplied shading tangent (object-space at hit, promoted forward in place
		//! to world space by Object/CSGObject; cleared if singular).
		Vector3						vShadingTangent;
		Scalar						shadingBitangentSign;	// +1 or -1 companion to vShadingTangent
		bool						bHasShadingTangent;

		//! Wireframe view-mode edge info: `bWantsWireEdgeInfo` (input) gates closest-edge
		//! output `ptWireNearestEdge` (world space after Object::IntersectRay) + `bHasWireEdgeInfo`.
		Point3						ptWireNearestEdge;
		bool						bHasWireEdgeInfo;
		bool						bWantsWireEdgeInfo;

		//! HOT-STRUCT LAYOUT INVARIANT: All fields below are declared at the tail to preserve
		//! earlier field offsets/vectorization bit-identity.
		//! Exact world-ray distances to geometric boundaries before surface-error backoff.
		Scalar						surfaceRange;
		Scalar						surfaceRange2;
		//! Per-cast input: if >= 0, rejects candidates with surfaceRange <= minimumSurfaceRange.
		Scalar						minimumSurfaceRange;
		//! Per-cast input (with minimumSurfaceRange >= 0): suppresses geometry self-hit bands
		//! when origin is an exact boundary-walk point.
		bool						bExactBoundaryOrigin;
		//! Per-cast input: true for primary camera rays (preserved across alpha/passthrough skips).
		//! Packed adjacent to bExactBoundaryOrigin to avoid tail padding.
		bool						bIsCameraRay;
		//! Object-space stencil offset stamped only by ReliefModifier's 4-tap loop (0 otherwise).
		Vector3						vReliefObjStep;
		//! Cold/optional tail pointers (preserved at tail for layout stability):
		//! - pmxObjectToWorld: `&Object::m_mxFinalTrans` (cleared by CSGObject)
		//! - pCamera: per-cast input active scene camera (carried by PropagateCastInputs)
		//! - pObjectInfo: borrowed hit object's ObjectInfoData
		const Matrix4*				pmxObjectToWorld;
		const ICamera*				pCamera;
		const ObjectInfoData*		pObjectInfo;

		//! Copies all per-cast input fields onto scratch candidate records; traversal and
		//! CSG composition MUST call this before dispatching so copy-back preserves inputs.
		inline void PropagateCastInputs( const RayIntersectionGeometric& src )
		{
			glossyFilterWidth = src.glossyFilterWidth;
			bWantsWireEdgeInfo = src.bWantsWireEdgeInfo;
			minimumSurfaceRange = src.minimumSurfaceRange;
			bExactBoundaryOrigin = src.bExactBoundaryOrigin;
			bIsCameraRay = src.bIsCameraRay;
			pCamera = src.pCamera;
		}

		RayIntersectionGeometric( const Ray& ray_, const RasterizerState& rast_ ) :
		  ray( ray_ ),
		  rast( rast_ ),
		  bHit( false ),
		  range( RISE_INFINITY ),
		  range2( RISE_INFINITY ),
		  bGeomNormalOrientedToRay( false ),
		  bGeomNormalRayDerived( false ),
		  bOpenSheet( false ),
		  bProvablyNoInterior( false ),
		  arrivalGeomFacing( 0 ),
		  bHasTexCoord1( false ),
		  bUVGeneratorApplied( false ),
		  pmxWorldToObject( 0 ),
		  pCustom( 0 ),
		  glossyFilterWidth( 0 ),
		  ambientIOR( 1.0 ),
		  bHasVertexColor( false ),
		  bitangentSign( 1.0 ),
		  bHasTangent( false ),
		  bShadingTangentFromGeometry( false ),
          bHasShaderDirection(false), bHasNormalMapFrame(false), bHasCoatDecodeOnb(false),
		  shadingBitangentSign( 1.0 ),
		  bHasShadingTangent( false ),
		  bHasWireEdgeInfo( false ),
		  bWantsWireEdgeInfo( false ),
		  surfaceRange( RISE_INFINITY ),
		  surfaceRange2( RISE_INFINITY ),
		  minimumSurfaceRange( -1.0 ),
		  bExactBoundaryOrigin( false ),
		  bIsCameraRay( false ),
		  vReliefObjStep( 0, 0, 0 ),
		  pmxObjectToWorld( 0 ),
		  pCamera( 0 ),
		  pObjectInfo( 0 )
		{}

		~RayIntersectionGeometric( )
		{
			safe_release( pCustom );
		}

		RayIntersectionGeometric( const RayIntersectionGeometric& r ) :
		  ray( r.ray ),
		  rast( r.rast ),
		  bHit( r.bHit ),
		  range( r.range ),
		  range2( r.range2 ),
		  vNormal( r.vNormal ),
		  vNormal2( r.vNormal2 ),
		  vGeomNormal( r.vGeomNormal ),
		  vGeomNormal2( r.vGeomNormal2 ),
		  bGeomNormalOrientedToRay( r.bGeomNormalOrientedToRay ),
		  bGeomNormalRayDerived( r.bGeomNormalRayDerived ),
		  bOpenSheet( r.bOpenSheet ),
		  bProvablyNoInterior( r.bProvablyNoInterior ),
		  arrivalGeomFacing( r.arrivalGeomFacing ),
		  ptCoord( r.ptCoord ),
		  ptCoord1( r.ptCoord1 ),
		  bHasTexCoord1( r.bHasTexCoord1 ),
		  bUVGeneratorApplied( r.bUVGeneratorApplied ),
		  ptIntersection( r.ptIntersection ),
		  ptExit( r.ptExit ),
		  ptObjIntersec( r.ptObjIntersec ),
		  ptObjExit( r.ptObjExit ),
		  pmxWorldToObject( r.pmxWorldToObject ),
		  onb( r.onb ),
		  pCustom( r.pCustom ),
		  glossyFilterWidth( r.glossyFilterWidth ),
		  ambientIOR( r.ambientIOR ),
		  derivatives( r.derivatives ),
		  signals( r.signals ),
		  txFootprint( r.txFootprint ),
		  vColor( r.vColor ),
		  bHasVertexColor( r.bHasVertexColor ),
		  vTangent( r.vTangent ),
		  bitangentSign( r.bitangentSign ),
		  bHasTangent( r.bHasTangent ),
		  bShadingTangentFromGeometry( r.bShadingTangentFromGeometry ),
          vShaderDirection(r.vShaderDirection), bHasShaderDirection(r.bHasShaderDirection),
          normalMapOnb(r.normalMapOnb), bHasNormalMapFrame(r.bHasNormalMapFrame),
          coatDecodeOnb(r.coatDecodeOnb), bHasCoatDecodeOnb(r.bHasCoatDecodeOnb),
		  vShadingTangent( r.vShadingTangent ),
		  shadingBitangentSign( r.shadingBitangentSign ),
		  bHasShadingTangent( r.bHasShadingTangent ),
		  ptWireNearestEdge( r.ptWireNearestEdge ),
		  bHasWireEdgeInfo( r.bHasWireEdgeInfo ),
		  bWantsWireEdgeInfo( r.bWantsWireEdgeInfo ),
		  surfaceRange( r.surfaceRange ),
		  surfaceRange2( r.surfaceRange2 ),
		  minimumSurfaceRange( r.minimumSurfaceRange ),
		  bExactBoundaryOrigin( r.bExactBoundaryOrigin ),
		  bIsCameraRay( r.bIsCameraRay ),
		  vReliefObjStep( r.vReliefObjStep ),
		  pmxObjectToWorld( r.pmxObjectToWorld ),
		  pCamera( r.pCamera ),
		  pObjectInfo( r.pObjectInfo )
		{
			if( pCustom ) {
				pCustom->addref();
			}
		}

		inline RayIntersectionGeometric& operator=( const RayIntersectionGeometric& r )
		{
			rast = r.rast;
			ray = r.ray;
			bHit = r.bHit;
			range = r.range;
			range2 = r.range2;
			surfaceRange = r.surfaceRange;
			surfaceRange2 = r.surfaceRange2;
			minimumSurfaceRange = r.minimumSurfaceRange;
			bExactBoundaryOrigin = r.bExactBoundaryOrigin;
			vReliefObjStep = r.vReliefObjStep;
			bIsCameraRay = r.bIsCameraRay;
			vNormal = r.vNormal;
			vNormal2 = r.vNormal2;
			vGeomNormal = r.vGeomNormal;
			vGeomNormal2 = r.vGeomNormal2;
			bGeomNormalOrientedToRay = r.bGeomNormalOrientedToRay;
			bGeomNormalRayDerived = r.bGeomNormalRayDerived;
			bOpenSheet = r.bOpenSheet;
			bProvablyNoInterior = r.bProvablyNoInterior;
			arrivalGeomFacing = r.arrivalGeomFacing;
			ptCoord = r.ptCoord;
			ptCoord1 = r.ptCoord1;
			bHasTexCoord1 = r.bHasTexCoord1;
			bUVGeneratorApplied = r.bUVGeneratorApplied;
			derivatives = r.derivatives;
			signals = r.signals;
			txFootprint = r.txFootprint;
			ptIntersection = r.ptIntersection;
			ptExit = r.ptExit;
			ptObjIntersec = r.ptObjIntersec;
			ptObjExit = r.ptObjExit;
			pmxWorldToObject = r.pmxWorldToObject;
			pmxObjectToWorld = r.pmxObjectToWorld;
			pCamera = r.pCamera;
			pObjectInfo = r.pObjectInfo;
			onb = r.onb;
			glossyFilterWidth = r.glossyFilterWidth;
			ambientIOR = r.ambientIOR;
			vColor = r.vColor;
			bHasVertexColor = r.bHasVertexColor;
			vTangent = r.vTangent;
			bitangentSign = r.bitangentSign;
			bHasTangent = r.bHasTangent;
			bShadingTangentFromGeometry = r.bShadingTangentFromGeometry;
            vShaderDirection=r.vShaderDirection; bHasShaderDirection=r.bHasShaderDirection;
            normalMapOnb=r.normalMapOnb; bHasNormalMapFrame=r.bHasNormalMapFrame;
            coatDecodeOnb=r.coatDecodeOnb; bHasCoatDecodeOnb=r.bHasCoatDecodeOnb;
			vShadingTangent = r.vShadingTangent;
			shadingBitangentSign = r.shadingBitangentSign;
			bHasShadingTangent = r.bHasShadingTangent;
			ptWireNearestEdge = r.ptWireNearestEdge;
			bHasWireEdgeInfo = r.bHasWireEdgeInfo;
			bWantsWireEdgeInfo = r.bWantsWireEdgeInfo;

			safe_release( pCustom );
			pCustom = r.pCustom;

			if( pCustom ) {
				pCustom->addref();
			}

			return *this;
		}
	};
}

#endif
