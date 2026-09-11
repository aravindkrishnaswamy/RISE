//////////////////////////////////////////////////////////////////////
//
//  BDPTVertexRIGRebuildTest.cpp - Verifies that
//    PathVertexEval::PopulateRIGFromVertex copies every surface-state
//    field from a BDPTVertex into a freshly-built
//    RayIntersectionGeometric.
//
//    Why this test exists:  BDPT and VCM used to contain several
//    sites that each manually reconstructed a
//    RayIntersectionGeometric from a stored BDPTVertex and then handed
//    that `ri` to a BSDF / painter.  When a new field was added to
//    RayIntersectionGeometric (e.g. vertex color) and to the
//    BDPTVertex mirror, some of those sites were silently left out,
//    biasing one BDPT strategy's BSDF evaluation relative to the
//    others and producing fireflies in MIS-weighted output.  Every
//    reconstruction now goes through the one helper -- six call sites
//    inside PathVertexEval.h itself (the RGB and NM BSDF / pdf
//    evaluators plus their two BSSRDF-profile branches), five in
//    BDPTIntegrator.cpp and two in VCMIntegrator.cpp, all of which
//    reach it by name, so the helper IS the copy list.  This test is
//    the canary on that helper: if a future refactor drops a field
//    from it, the corresponding sentinel assertion below fails.
//
//    GRANULARITY IS THE SCALAR, POINTER OR FLAG -- never the struct.
//    Three of the mirrored fields (derivatives, signals, txFootprint)
//    are themselves structs, and a member-by-member copy that dropped
//    one member would still look like a copy at the struct level, so
//    each of their members gets its own uniquely-valued sentinel and
//    its own Check.  When adding a new mirrored field, extend this
//    test the same way.
//
//    BOOLEANS NEED A SECOND, MIXED PASS.  A uniquely-valued sentinel
//    means nothing for a bool -- there are only two values, so every
//    boolean in a struct shares its value with every other boolean in
//    the same struct across the all-true (TestPopulateRIG_AllFields)
//    and all-false (TestPopulateRIG_DefaultsAlsoCopy) cases.  A
//    member-by-member copy that SWAPPED two same-struct booleans (e.g.
//    `valid` <-> `curvatureValid` in `derivatives`) would pass both of
//    those cases undetected: PopulateCurvature would take the wrong
//    branch and nothing here would catch it.
//    TestPopulateRIG_MixedBooleanPatterns runs two additional passes
//    whose boolean assignments are chosen so every pair of same-struct
//    booleans differs in at least one pass -- see that function's
//    comment for the derivation.  `signals.bComplementedField` has no
//    same-struct boolean sibling to be swapped with, so it needs no
//    mixed pass of its own; the existing all-true / all-false cases
//    already cover it.
//
//    ambientIOR (BDPTVertex::mediumIOR, the G6 stamp) is a scalar
//    guarded by a branch, not a straight copy, so it gets its own
//    sentinel function: TestPopulateRIG_AmbientIOR exercises the
//    pass-through branch and both guard inputs (zero and negative).
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>

#include "../src/Library/Shaders/BDPTVertex.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/PathVertexEval.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool condition, const char* testName )
{
	if( condition ) {
		passCount++;
	} else {
		failCount++;
		std::cout << "  FAIL: " << testName << std::endl;
	}
}

static bool IsClose( Scalar a, Scalar b, Scalar eps = 1e-9 )
{
	return std::fabs( a - b ) < eps;
}

//////////////////////////////////////////////////////////////////////
// Distinct, never-dereferenced pointer sentinels for the three
// back-pointers SurfaceSignalInfo carries.  They must be three
// DIFFERENT non-null addresses: pSelf and pScene in particular are
// stamped side by side by ObjectManager::IntersectRay, so a copy that
// wrote one into the other's slot would be invisible to a "non-null"
// check.  Nothing ever dereferences these -- the helper copies
// pointers, it does not call through them -- so the incomplete types
// are fine.
//////////////////////////////////////////////////////////////////////
static const char kProviderTagByte = 'p';
static const char kSceneTagByte    = 's';
static const char kSelfTagByte     = 'o';

static const ISurfaceSignalProvider* const kProviderSentinel =
	reinterpret_cast<const ISurfaceSignalProvider*>( &kProviderTagByte );
static const IObjectManager* const kSceneSentinel =
	reinterpret_cast<const IObjectManager*>( &kSceneTagByte );
static const IObject* const kSelfSentinel =
	reinterpret_cast<const IObject*>( &kSelfTagByte );

//////////////////////////////////////////////////////////////////////
// FillSurfaceStateSentinels
//
// Populates a BDPTVertex with non-default, distinguishable values for
// every field that PopulateRIGFromVertex copies.  Each scalar is
// unique so a swapped-source field would produce a detectable mismatch.
//////////////////////////////////////////////////////////////////////
static BDPTVertex MakeSentinelSurfaceVertex()
{
	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position      = Point3( 7.0,  -3.0,  11.0 );
	v.normal        = Vector3( 0.0,  1.0,   0.0 );
	// geomNormal sentinel intentionally distinct from normal — on a real
	// triangle mesh under heavy bump-mapping these can diverge by tens of
	// degrees; the rebuild must propagate them independently.
	v.geomNormal    = Vector3( 0.0,  0.0,   1.0 );
	v.onb.CreateFromW( v.normal );
	v.ptCoord       = Point2( 0.375, 0.625 );
	// ptCoord1 sentinel intentionally distinct from ptCoord — on real
	// glTF assets with a TEXCOORD_1 binding, the secondary UV chart can
	// be wholly different (lightmap atlas, AO bake) from the primary;
	// the rebuild must propagate the secondary coords independently or
	// any TexCoord1Painter wrapped by the importer would silently sample
	// from TEXCOORD_0 on every BDPT/VCM-rebuilt path vertex.
	v.ptCoord1      = Point2( 0.875, 0.125 );
	v.bHasTexCoord1 = true;
	v.ptObjIntersec = Point3( 0.125, -0.875, 0.5 );
	v.vColor          = RISEPel( 0.42, 0.71, 0.13 );
	v.bHasVertexColor = true;

	// ---- derivatives: the expression VM's curv / curvR, plus the UV
	// Jacobian and the texcoord chart map.  Every scalar distinct, and
	// every one distinct from the struct's OWN default (scaleHint 1.0,
	// dsdu/dtdv 1, dsdv/dtdu 0), so a dropped member leaves a value the
	// matching Check rejects.
	v.derivatives.dpdu           = Vector3( 1.01, 1.02, 1.03 );
	v.derivatives.dpdv           = Vector3( 1.04, 1.05, 1.06 );
	v.derivatives.dndu           = Vector3( 1.07, 1.08, 1.09 );
	v.derivatives.dndv           = Vector3( 1.10, 1.11, 1.12 );
	v.derivatives.valid          = true;
	v.derivatives.scaleHint      = 1.13;
	v.derivatives.curvature      = 1.14;
	v.derivatives.curvatureValid = true;
	v.derivatives.dsdu           = 1.15;
	v.derivatives.dsdv           = 1.16;
	v.derivatives.dtdu           = 1.17;
	v.derivatives.dtdv           = 1.18;
	v.derivatives.texChartValid  = true;

	// ---- signals: the own-surface half (provider + where on it) AND
	// the cross-object half (pScene / pSelf / ptWorld / time).  Three
	// DIFFERENT pointer sentinels so a slot swap is detectable.
	v.signals.pProvider          = kProviderSentinel;
	v.signals.ptObject           = Point3( 2.01, 2.02, 2.03 );
	v.signals.nObject            = Vector3( 2.04, 2.05, 2.06 );
	v.signals.primId             = 271;
	v.signals.baryA              = 2.07;
	v.signals.baryB              = 2.08;
	v.signals.bComplementedField = true;
	v.signals.pScene             = kSceneSentinel;
	v.signals.pSelf              = kSelfSentinel;
	v.signals.ptWorld            = Point3( 2.09, 2.10, 2.11 );
	v.signals.time               = 2.12;

	// ---- txFootprint: the expression VM's fw / fwo.  All-zero under
	// today's bidirectional rasterizers (they emit no ray
	// differentials), carried so a future landing cannot silently
	// reopen the gap -- which is exactly why the sentinels here are
	// non-zero.
	v.txFootprint.dudx        = 3.01;
	v.txFootprint.dudy        = 3.02;
	v.txFootprint.dvdx        = 3.03;
	v.txFootprint.dvdy        = 3.04;
	v.txFootprint.dpdx        = Vector3( 3.05, 3.06, 3.07 );
	v.txFootprint.dpdy        = Vector3( 3.08, 3.09, 3.10 );
	v.txFootprint.worldWidth  = 3.11;
	v.txFootprint.objectWidth = 3.12;
	v.txFootprint.valid       = true;
	v.txFootprint.widthValid  = true;

	return v;
}

//////////////////////////////////////////////////////////////////////
// TestPopulateRIG_AllFields
//
// The canonical sentinel-value test.  Every field in the helper's copy
// list must be propagated.  If a future refactor drops a field, the
// matching Check fails and points at the omitted line in
// PathVertexEval::PopulateRIGFromVertex.
//////////////////////////////////////////////////////////////////////
void TestPopulateRIG_AllFields()
{
	std::cout << "Testing PopulateRIGFromVertex copies every mirrored field..." << std::endl;

	const BDPTVertex v = MakeSentinelSurfaceVertex();

	const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
	RayIntersectionGeometric ri( dummyRay, nullRasterizerState );

	PathVertexEval::PopulateRIGFromVertex( v, ri );

	// bHit
	Check( ri.bHit == true,
		"bHit should be set to true" );

	// position -> ptIntersection
	Check( IsClose( ri.ptIntersection.x, 7.0 )  &&
		   IsClose( ri.ptIntersection.y, -3.0 ) &&
		   IsClose( ri.ptIntersection.z, 11.0 ),
		"ptIntersection should mirror vertex.position" );

	// normal -> vNormal
	Check( IsClose( ri.vNormal.x, 0.0 ) &&
		   IsClose( ri.vNormal.y, 1.0 ) &&
		   IsClose( ri.vNormal.z, 0.0 ),
		"vNormal should mirror vertex.normal" );

	// geomNormal -> vGeomNormal — must propagate independently of vNormal
	// so downstream side-of-surface tests can read the geometric face
	// normal even when the shading normal has been Phong-interpolated or
	// perturbed by one of the normal-perturbing modifiers (bump map,
	// normal map, glint, relief).
	Check( IsClose( ri.vGeomNormal.x, 0.0 ) &&
		   IsClose( ri.vGeomNormal.y, 0.0 ) &&
		   IsClose( ri.vGeomNormal.z, 1.0 ),
		"vGeomNormal should mirror vertex.geomNormal" );

	// onb (compare W axis — the onb was built from the normal)
	Check( IsClose( ri.onb.w().x, 0.0 ) &&
		   IsClose( ri.onb.w().y, 1.0 ) &&
		   IsClose( ri.onb.w().z, 0.0 ),
		"onb.w() should mirror the vertex onb (built from normal)" );

	// ptCoord
	Check( IsClose( ri.ptCoord.x, 0.375 ) &&
		   IsClose( ri.ptCoord.y, 0.625 ),
		"ptCoord should mirror vertex.ptCoord" );

	// ptCoord1 — TEXCOORD_1 (secondary glTF UV).  Distinct from ptCoord
	// so a swapped-source field (e.g. accidentally copying ptCoord into
	// both slots) is detectable.
	Check( IsClose( ri.ptCoord1.x, 0.875 ) &&
		   IsClose( ri.ptCoord1.y, 0.125 ),
		"ptCoord1 should mirror vertex.ptCoord1" );

	// bHasTexCoord1 — gates whether TexCoord1Painter swaps in ptCoord1.
	Check( ri.bHasTexCoord1 == true,
		"bHasTexCoord1 should mirror vertex.bHasTexCoord1" );

	// ptObjIntersec
	Check( IsClose( ri.ptObjIntersec.x, 0.125 )  &&
		   IsClose( ri.ptObjIntersec.y, -0.875 ) &&
		   IsClose( ri.ptObjIntersec.z, 0.5 ),
		"ptObjIntersec should mirror vertex.ptObjIntersec" );

	// vColor — the firefly-prone field that prompted the helper.
	Check( IsClose( ri.vColor.r, 0.42 ) &&
		   IsClose( ri.vColor.g, 0.71 ) &&
		   IsClose( ri.vColor.b, 0.13 ),
		"vColor should mirror vertex.vColor" );

	// bHasVertexColor — gates whether painters consume vColor.
	Check( ri.bHasVertexColor == true,
		"bHasVertexColor should mirror vertex.bHasVertexColor" );

	// ------------------------------------------------------------------
	// derivatives — the expression VM's `curv` / `curvR` come from this
	// struct (scaleHint normalises H; curvature/curvatureValid is the
	// SDF family's direct answer), and SolveFootprintUV's chart map
	// lives here too.  One Check per member: a member-by-member copy
	// that dropped one would still look like a copy at struct level.
	// ------------------------------------------------------------------
	Check( IsClose( ri.derivatives.dpdu.x, 1.01 ) &&
		   IsClose( ri.derivatives.dpdu.y, 1.02 ) &&
		   IsClose( ri.derivatives.dpdu.z, 1.03 ),
		"derivatives.dpdu should mirror vertex.derivatives.dpdu" );

	Check( IsClose( ri.derivatives.dpdv.x, 1.04 ) &&
		   IsClose( ri.derivatives.dpdv.y, 1.05 ) &&
		   IsClose( ri.derivatives.dpdv.z, 1.06 ),
		"derivatives.dpdv should mirror vertex.derivatives.dpdv" );

	Check( IsClose( ri.derivatives.dndu.x, 1.07 ) &&
		   IsClose( ri.derivatives.dndu.y, 1.08 ) &&
		   IsClose( ri.derivatives.dndu.z, 1.09 ),
		"derivatives.dndu should mirror vertex.derivatives.dndu" );

	Check( IsClose( ri.derivatives.dndv.x, 1.10 ) &&
		   IsClose( ri.derivatives.dndv.y, 1.11 ) &&
		   IsClose( ri.derivatives.dndv.z, 1.12 ),
		"derivatives.dndv should mirror vertex.derivatives.dndv" );

	Check( ri.derivatives.valid == true,
		"derivatives.valid should mirror vertex.derivatives.valid" );

	// scaleHint's own default is 1.0, so the sentinel 1.13 separates
	// "copied" from "left at the struct default".
	Check( IsClose( ri.derivatives.scaleHint, 1.13 ),
		"derivatives.scaleHint should mirror vertex.derivatives.scaleHint "
		"(and not fall back to the 1.0 default)" );

	Check( IsClose( ri.derivatives.curvature, 1.14 ),
		"derivatives.curvature should mirror vertex.derivatives.curvature" );

	Check( ri.derivatives.curvatureValid == true,
		"derivatives.curvatureValid should mirror vertex.derivatives.curvatureValid" );

	// The chart map's defaults are the IDENTITY (1,0,0,1), so these four
	// sentinels also catch a copy that silently reinitialised the struct.
	Check( IsClose( ri.derivatives.dsdu, 1.15 ),
		"derivatives.dsdu should mirror vertex.derivatives.dsdu" );
	Check( IsClose( ri.derivatives.dsdv, 1.16 ),
		"derivatives.dsdv should mirror vertex.derivatives.dsdv" );
	Check( IsClose( ri.derivatives.dtdu, 1.17 ),
		"derivatives.dtdu should mirror vertex.derivatives.dtdu" );
	Check( IsClose( ri.derivatives.dtdv, 1.18 ),
		"derivatives.dtdv should mirror vertex.derivatives.dtdv" );

	Check( ri.derivatives.texChartValid == true,
		"derivatives.texChartValid should mirror vertex.derivatives.texChartValid" );

	// ------------------------------------------------------------------
	// signals — both halves.  The own-surface half feeds `occlusion(r)`
	// / `thickness(r)` / `convexity(r)`; the cross-object half feeds
	// `proximity(r)` / `interior(r)`.  A dropped pScene turns every
	// proximity() in the frame into its neutral 0, which still renders
	// — just wrong — so each pointer is checked by IDENTITY against its
	// own sentinel, not merely for non-nullness.
	// ------------------------------------------------------------------
	Check( ri.signals.pProvider == kProviderSentinel,
		"signals.pProvider should mirror vertex.signals.pProvider" );

	Check( IsClose( ri.signals.ptObject.x, 2.01 ) &&
		   IsClose( ri.signals.ptObject.y, 2.02 ) &&
		   IsClose( ri.signals.ptObject.z, 2.03 ),
		"signals.ptObject should mirror vertex.signals.ptObject" );

	Check( IsClose( ri.signals.nObject.x, 2.04 ) &&
		   IsClose( ri.signals.nObject.y, 2.05 ) &&
		   IsClose( ri.signals.nObject.z, 2.06 ),
		"signals.nObject should mirror vertex.signals.nObject" );

	// primId's default is -1 ("answers positionally"), so a dropped copy
	// would make a mesh provider look like an SDF one.
	Check( ri.signals.primId == 271,
		"signals.primId should mirror vertex.signals.primId "
		"(and not fall back to the -1 default)" );

	Check( IsClose( ri.signals.baryA, 2.07 ),
		"signals.baryA should mirror vertex.signals.baryA" );
	Check( IsClose( ri.signals.baryB, 2.08 ),
		"signals.baryB should mirror vertex.signals.baryB" );

	Check( ri.signals.bComplementedField == true,
		"signals.bComplementedField should mirror vertex.signals.bComplementedField "
		"(a dropped flag inverts occlusion/convexity's sense under CSG subtraction)" );

	Check( ri.signals.pScene == kSceneSentinel,
		"signals.pScene should mirror vertex.signals.pScene — this is the "
		"object manager's own stamp, forwarded, and a null here silently "
		"neutralises every proximity() and interior() on the path" );

	Check( ri.signals.pSelf == kSelfSentinel,
		"signals.pSelf should mirror vertex.signals.pSelf, distinctly from pScene" );

	Check( IsClose( ri.signals.ptWorld.x, 2.09 ) &&
		   IsClose( ri.signals.ptWorld.y, 2.10 ) &&
		   IsClose( ri.signals.ptWorld.z, 2.11 ),
		"signals.ptWorld should mirror vertex.signals.ptWorld" );

	Check( IsClose( ri.signals.time, 2.12 ),
		"signals.time should mirror vertex.signals.time" );

	// ------------------------------------------------------------------
	// txFootprint — the expression VM's `fw` / `fwo` octave fade.  Zero
	// in practice under today's bidirectional rasterizers; carried, and
	// pinned here, so a future ray-differential landing on BDPT / VCM
	// cannot reopen the gap by omission.
	// ------------------------------------------------------------------
	Check( IsClose( ri.txFootprint.dudx, 3.01 ),
		"txFootprint.dudx should mirror vertex.txFootprint.dudx" );
	Check( IsClose( ri.txFootprint.dudy, 3.02 ),
		"txFootprint.dudy should mirror vertex.txFootprint.dudy" );
	Check( IsClose( ri.txFootprint.dvdx, 3.03 ),
		"txFootprint.dvdx should mirror vertex.txFootprint.dvdx" );
	Check( IsClose( ri.txFootprint.dvdy, 3.04 ),
		"txFootprint.dvdy should mirror vertex.txFootprint.dvdy" );

	Check( IsClose( ri.txFootprint.dpdx.x, 3.05 ) &&
		   IsClose( ri.txFootprint.dpdx.y, 3.06 ) &&
		   IsClose( ri.txFootprint.dpdx.z, 3.07 ),
		"txFootprint.dpdx should mirror vertex.txFootprint.dpdx" );

	Check( IsClose( ri.txFootprint.dpdy.x, 3.08 ) &&
		   IsClose( ri.txFootprint.dpdy.y, 3.09 ) &&
		   IsClose( ri.txFootprint.dpdy.z, 3.10 ),
		"txFootprint.dpdy should mirror vertex.txFootprint.dpdy" );

	Check( IsClose( ri.txFootprint.worldWidth, 3.11 ),
		"txFootprint.worldWidth should mirror vertex.txFootprint.worldWidth" );
	Check( IsClose( ri.txFootprint.objectWidth, 3.12 ),
		"txFootprint.objectWidth should mirror vertex.txFootprint.objectWidth "
		"(distinct from worldWidth — it is the same width in ptObjIntersec's frame)" );

	Check( ri.txFootprint.valid == true,
		"txFootprint.valid should mirror vertex.txFootprint.valid" );
	Check( ri.txFootprint.widthValid == true,
		"txFootprint.widthValid should mirror vertex.txFootprint.widthValid" );
}

//////////////////////////////////////////////////////////////////////
// TestPopulateRIG_DefaultsAlsoCopy
//
// Confirms the helper isn't selectively copying "interesting" values:
// a vertex with bHasVertexColor=false and a zero vColor must produce
// the same false flag and zero color in `ri`.  Important because the
// painter's behaviour gates on bHasVertexColor; a copy that always set
// it to true (or always set vColor to a sentinel) would silently break
// non-vertex-color materials.
//////////////////////////////////////////////////////////////////////
void TestPopulateRIG_DefaultsAlsoCopy()
{
	std::cout << "Testing PopulateRIGFromVertex preserves default/false values..." << std::endl;

	BDPTVertex v;
	v.type = BDPTVertex::SURFACE;
	v.position        = Point3( 0, 0, 0 );
	v.normal          = Vector3( 0, 0, 1 );
	v.onb.CreateFromW( v.normal );
	// Leave ptCoord, ptObjIntersec, vColor as default-constructed (zero).
	// bHasVertexColor is already false from the BDPTVertex default ctor.

	const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
	RayIntersectionGeometric ri( dummyRay, nullRasterizerState );

	// Pre-set the destination ri's vertex-color fields to sentinel
	// values so we can detect whether the helper actually overwrites
	// them — a no-op helper would let the sentinel through.
	ri.vColor          = RISEPel( 9.99, 9.99, 9.99 );
	ri.bHasVertexColor = true;
	// Same defensive sentinel for the UV1 fields — if the helper
	// silently leaves them, a TexCoord1Painter wrapped on the rebuilt
	// ri would sample at the sentinel UV instead of UV0.
	ri.ptCoord1        = Point2( 9.99, 9.99 );
	ri.bHasTexCoord1   = true;
	// Same defensive sentinel for the three painter-input structs.  This
	// direction matters more than it looks: a NON-surface vertex (camera,
	// light, env, medium) and a BSSRDF entry vertex both carry the
	// defaults deliberately, and the record handed to the helper may be
	// reused across evaluations.  A helper that copied only "interesting"
	// values would leak a previous surface hit's provider, scene pointer
	// or curvature into a vertex that has none — painting a signal where
	// the honest answer is its neutral.
	ri.derivatives.dpdu           = Vector3( 9.99, 9.99, 9.99 );
	ri.derivatives.valid          = true;
	ri.derivatives.scaleHint      = 9.99;
	ri.derivatives.curvature      = 9.99;
	ri.derivatives.curvatureValid = true;
	ri.derivatives.dsdu           = 9.99;
	ri.derivatives.texChartValid  = true;
	ri.signals.pProvider          = kProviderSentinel;
	ri.signals.pScene             = kSceneSentinel;
	ri.signals.pSelf              = kSelfSentinel;
	ri.signals.primId             = 271;
	ri.signals.bComplementedField = true;
	ri.signals.time               = 9.99;
	ri.txFootprint.worldWidth     = 9.99;
	ri.txFootprint.objectWidth    = 9.99;
	ri.txFootprint.valid          = true;
	ri.txFootprint.widthValid     = true;

	PathVertexEval::PopulateRIGFromVertex( v, ri );

	Check( IsClose( ri.vColor.r, 0.0 ) &&
		   IsClose( ri.vColor.g, 0.0 ) &&
		   IsClose( ri.vColor.b, 0.0 ),
		"vColor should be overwritten with vertex.vColor (zero) — not the pre-existing sentinel" );

	Check( ri.bHasVertexColor == false,
		"bHasVertexColor should be overwritten with vertex.bHasVertexColor (false)" );

	Check( IsClose( ri.ptCoord1.x, 0.0 ) &&
		   IsClose( ri.ptCoord1.y, 0.0 ),
		"ptCoord1 should be overwritten with vertex.ptCoord1 (zero) — not the pre-existing sentinel" );

	Check( ri.bHasTexCoord1 == false,
		"bHasTexCoord1 should be overwritten with vertex.bHasTexCoord1 (false)" );

	Check( IsClose( ri.derivatives.dpdu.x, 0.0 ) &&
		   IsClose( ri.derivatives.dpdu.y, 0.0 ) &&
		   IsClose( ri.derivatives.dpdu.z, 0.0 ) &&
		   ri.derivatives.valid == false,
		"derivatives should be overwritten with the vertex's default "
		"(zero basis, valid=false) — not the pre-existing sentinel" );

	Check( IsClose( ri.derivatives.scaleHint, 1.0 ) &&
		   IsClose( ri.derivatives.curvature, 0.0 ) &&
		   ri.derivatives.curvatureValid == false,
		"derivatives' curvature trio should be overwritten with the vertex's "
		"default (scaleHint 1.0 — the honest `this geometry did not say`, "
		"which collapses curv to raw curvR rather than to zero)" );

	Check( IsClose( ri.derivatives.dsdu, 1.0 ) &&
		   ri.derivatives.texChartValid == false,
		"derivatives' chart map should be overwritten with the vertex's "
		"default identity + texChartValid=false" );

	Check( ri.signals.pProvider == 0 &&
		   ri.signals.pScene == 0 &&
		   ri.signals.pSelf == 0,
		"signals' three back-pointers should be overwritten with the vertex's "
		"defaults (null) — a leaked provider or scene pointer would paint a "
		"signal at a vertex that publishes none" );

	Check( ri.signals.primId == -1 &&
		   ri.signals.bComplementedField == false &&
		   IsClose( ri.signals.time, 0.0 ),
		"signals' primId / bComplementedField / time should be overwritten "
		"with the vertex's defaults" );

	Check( IsClose( ri.txFootprint.worldWidth, 0.0 ) &&
		   IsClose( ri.txFootprint.objectWidth, 0.0 ) &&
		   ri.txFootprint.valid == false &&
		   ri.txFootprint.widthValid == false,
		"txFootprint should be overwritten with the vertex's default "
		"(zero widths, both validity flags false)" );
}

//////////////////////////////////////////////////////////////////////
// TestPopulateRIG_MixedBooleanPatterns
//
// Closes the boolean-swap blind spot in the all-true / all-false
// sentinel cases above.  With only two values, a bool can never carry
// a "uniquely-valued" sentinel the way a scalar or pointer can -- so a
// member-by-member copy that SWAPPED two same-struct booleans (e.g.
// `derivatives.valid` <-> `derivatives.curvatureValid`, which would
// make PopulateCurvature take the wrong branch) passes both the
// all-true and all-false cases undetected, because within each of
// those cases every boolean in the struct already shares its
// neighbours' value.
//
// The fix: run two more passes whose per-boolean assignment is chosen
// so every PAIR of same-struct booleans differs in at least one pass.
// Treat each boolean's two-pass assignment as a 2-bit column; a swap
// between booleans i and j is invisible iff their columns are
// identical, so the passes below give `derivatives`' three booleans
// (valid, curvatureValid, texChartValid) the columns (T,F), (F,T) and
// (F,F) -- pairwise distinct, so no swap among the three can hide --
// and give `txFootprint`'s two booleans (valid, widthValid) the
// columns (T,F) and (F,T), the inverse of each other.
//
// `signals.bComplementedField` is the only boolean in SurfaceSignalInfo
// -- it has no same-struct sibling to be swapped with, so it is not
// re-exercised here; the existing all-true / all-false cases already
// pin its value.
//////////////////////////////////////////////////////////////////////
void TestPopulateRIG_MixedBooleanPatterns()
{
	std::cout << "Testing PopulateRIGFromVertex against mixed boolean patterns "
		"(catches a same-struct boolean swap the all-true/all-false cases can't)..." << std::endl;

	// ---- Pass A: derivatives (valid, curvatureValid, texChartValid) =
	// (true, false, false); txFootprint (valid, widthValid) = (true, false).
	{
		BDPTVertex v;
		v.type   = BDPTVertex::SURFACE;
		v.normal = Vector3( 0, 0, 1 );
		v.onb.CreateFromW( v.normal );

		v.derivatives.valid          = true;
		v.derivatives.curvatureValid = false;
		v.derivatives.texChartValid  = false;

		v.txFootprint.valid      = true;
		v.txFootprint.widthValid = false;

		const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersectionGeometric ri( dummyRay, nullRasterizerState );

		PathVertexEval::PopulateRIGFromVertex( v, ri );

		Check( ri.derivatives.valid == true,
			"[mixed pass A] derivatives.valid should be true" );
		Check( ri.derivatives.curvatureValid == false,
			"[mixed pass A] derivatives.curvatureValid should be false "
			"(a valid<->curvatureValid swap would flip this)" );
		Check( ri.derivatives.texChartValid == false,
			"[mixed pass A] derivatives.texChartValid should be false "
			"(a valid<->texChartValid swap would flip this)" );

		Check( ri.txFootprint.valid == true,
			"[mixed pass A] txFootprint.valid should be true" );
		Check( ri.txFootprint.widthValid == false,
			"[mixed pass A] txFootprint.widthValid should be false "
			"(a valid<->widthValid swap would flip this)" );
	}

	// ---- Pass B: derivatives (valid, curvatureValid, texChartValid) =
	// (false, true, false); txFootprint (valid, widthValid) = (false, true)
	// -- the inverse of pass A.
	{
		BDPTVertex v;
		v.type   = BDPTVertex::SURFACE;
		v.normal = Vector3( 0, 0, 1 );
		v.onb.CreateFromW( v.normal );

		v.derivatives.valid          = false;
		v.derivatives.curvatureValid = true;
		v.derivatives.texChartValid  = false;

		v.txFootprint.valid      = false;
		v.txFootprint.widthValid = true;

		const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersectionGeometric ri( dummyRay, nullRasterizerState );

		PathVertexEval::PopulateRIGFromVertex( v, ri );

		Check( ri.derivatives.valid == false,
			"[mixed pass B] derivatives.valid should be false "
			"(a valid<->curvatureValid swap would flip this)" );
		Check( ri.derivatives.curvatureValid == true,
			"[mixed pass B] derivatives.curvatureValid should be true" );
		Check( ri.derivatives.texChartValid == false,
			"[mixed pass B] derivatives.texChartValid should be false "
			"(a curvatureValid<->texChartValid swap would flip this)" );

		Check( ri.txFootprint.valid == false,
			"[mixed pass B] txFootprint.valid should be false "
			"(a valid<->widthValid swap would flip this)" );
		Check( ri.txFootprint.widthValid == true,
			"[mixed pass B] txFootprint.widthValid should be true" );
	}
}

//////////////////////////////////////////////////////////////////////
// TestPopulateRIG_AmbientIOR
//
// `PopulateRIGFromVertex` writes `ri.ambientIOR` from
// `vertex.mediumIOR` through a guard -- `(mediumIOR > 0.0) ?
// mediumIOR : 1.0` -- rather than a straight copy, so it needs its
// own sentinel coverage distinct from the struct-copy fields above:
// one case exercising the pass-through branch, and two exercising the
// guard (zero and negative).  Each destination `ri.ambientIOR` is
// pre-poisoned to a value equal to none of the expected outcomes, so a
// forgotten write is caught rather than accidentally matching either
// branch's result.
//////////////////////////////////////////////////////////////////////
void TestPopulateRIG_AmbientIOR()
{
	std::cout << "Testing PopulateRIGFromVertex's ambientIOR guard..." << std::endl;

	const Scalar kPoison = -999.0;

	// mediumIOR = 1.337 (positive, non-default) -> ambientIOR should
	// pass through unchanged.
	{
		BDPTVertex v;
		v.type      = BDPTVertex::SURFACE;
		v.normal    = Vector3( 0, 0, 1 );
		v.onb.CreateFromW( v.normal );
		v.mediumIOR = 1.337;

		const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
		ri.ambientIOR = kPoison;

		PathVertexEval::PopulateRIGFromVertex( v, ri );

		Check( IsClose( ri.ambientIOR, 1.337 ),
			"ambientIOR should mirror vertex.mediumIOR (1.337) when positive" );
	}

	// mediumIOR = 0.0 -> guard should substitute 1.0 (air).
	{
		BDPTVertex v;
		v.type      = BDPTVertex::SURFACE;
		v.normal    = Vector3( 0, 0, 1 );
		v.onb.CreateFromW( v.normal );
		v.mediumIOR = 0.0;

		const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
		ri.ambientIOR = kPoison;

		PathVertexEval::PopulateRIGFromVertex( v, ri );

		Check( IsClose( ri.ambientIOR, 1.0 ),
			"ambientIOR should guard mediumIOR == 0.0 to 1.0 (air), "
			"not pass the non-positive value through" );
	}

	// mediumIOR = -2.0 -> guard should substitute 1.0 (air).
	{
		BDPTVertex v;
		v.type      = BDPTVertex::SURFACE;
		v.normal    = Vector3( 0, 0, 1 );
		v.onb.CreateFromW( v.normal );
		v.mediumIOR = -2.0;

		const Ray dummyRay( Point3( 0, 0, 0 ), Vector3( 1, 0, 0 ) );
		RayIntersectionGeometric ri( dummyRay, nullRasterizerState );
		ri.ambientIOR = kPoison;

		PathVertexEval::PopulateRIGFromVertex( v, ri );

		Check( IsClose( ri.ambientIOR, 1.0 ),
			"ambientIOR should guard mediumIOR == -2.0 (negative) to 1.0 (air), "
			"not pass the non-positive value through" );
	}
}

//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////
int main()
{
	std::cout << "=== BDPTVertexRIGRebuildTest ===" << std::endl;

	TestPopulateRIG_AllFields();
	TestPopulateRIG_DefaultsAlsoCopy();
	TestPopulateRIG_MixedBooleanPatterns();
	TestPopulateRIG_AmbientIOR();

	std::cout << std::endl;
	std::cout << "Passed: " << passCount << std::endl;
	std::cout << "Failed: " << failCount << std::endl;

	return failCount == 0 ? 0 : 1;
}
