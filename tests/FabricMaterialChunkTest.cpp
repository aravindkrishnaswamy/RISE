//////////////////////////////////////////////////////////////////////
//
//  FabricMaterialChunkTest.cpp - Contract test for the
//    `fabric_material` chunk (docs/CLOTH_FABRIC_DESIGN.md Phase 1,
//    9.2 - 9.6): the parser / Job / registration surface, plus the two
//    numeric claims that have no other home.
//
//  WHAT THIS TEST OWNS -- and, as importantly, what it does not.  The
//  LAYER ENERGY is owned by LayeredWhiteFurnaceTest (gate 3), the
//  value<->Scatter agreement and reciprocity by SPFBSDFConsistencyTest
//  (gate 5a) and SPFPdfConsistencyTest (gate 6).  This file owns the
//  boundary a scene author actually hits, and two measurements:
//
//    1. THE PRESET TABLE, SLOT BY SLOT.  `fabric <name>` is RISE's
//       first multi-slot preset seeder, so each of the eight presets is
//       checked BEHAVIOURALLY: a chunk naming only `fabric X` must
//       respond BIT-IDENTICALLY to a `fabric custom` chunk whose slots
//       are written out by hand at the table's values.  That is what
//       makes FabricPresets.h's numbers the contract rather than an
//       implementation detail, and it is what would catch a preset
//       silently ceasing to seed.
//
//    2. EXPLICIT SLOTS WIN OVER THE PRESET.  9.3's rule is "`fabric
//       <name>` supplies the default for every slot the author did not
//       write; any explicitly written slot wins".  `bag.GetString(name,
//       default)` alone cannot express that -- it cannot tell "omitted"
//       from "written at the default value" -- so this checks the
//       `ParseStateBag::Has()` path directly, INCLUDING the case where
//       an author overrides one preset slot and inherits another.
//
//    3. THE SUBSTRATE ALLOWLIST, as a refusal MATRIX, with each refusal
//       checked for the SPECIFIC diagnostic that distinguishes its
//       branch -- and the positive controls, including
//       pbr_metallic_roughness (accepted because it resolves to a
//       ggx_material at scene-build time).
//
//    4. THE PRESET-vs-SUBSTRATE MISMATCH IS A WARNING, NOT AN ERROR --
//       and the material still registers.  This is the one place the
//       two diagnostics could be conflated, and conflating them either
//       way is a real defect: erroring would refuse a legal
//       composition, and staying silent would let `fabric satin` over a
//       Lambertian look like it worked.
//
//    5. sheen_color's `none` -> PRESET-COLOUR-else-WHITE default.
//       `none` is the colour manager's built-in BLACK painter, so a
//       naive resolve would switch the sheen lobe OFF.  Checked
//       behaviourally against explicit white and explicit black.
//
//    6. weave_rotation REACHES THE SUBSTRATE'S FRAME.  Over an
//       ANISOTROPIC ggx base (alphax != alphay) a non-zero rotation
//       must MOVE the response; over an ISOTROPIC one it must not (a
//       rotation about the normal is a no-op on an isotropic lobe).
//       Both halves are needed: the first alone would pass if the
//       rotation were being applied to the wrong thing, the second
//       alone if it were being applied to nothing.  Also: an inline
//       literal and a named scalar_painter at the same angle must agree
//       bit-for-bit.
//
//    6b. THE ROUGHNESS FLOOR, behaviourally: `sheen_roughness 0.01` must
//       be clamped to 0.04 (identical response at every probed
//       direction) while 0.05 must NOT be (so the first half pins the
//       clamp rather than an ignored slot).  Nothing exercised
//       `ResolveFabric`'s `r_max( kMinSheenAlpha, ... )` before this.
//
//    7. requireSingle ON BOTH SCALAR SLOTS, and editor introspection.
//
//    8. THE API-LEVEL ALLOWLIST, driven with no Job in the picture, so
//       the deliberate duplicate in RISE_API_CreateFabricMaterial
//       cannot rot unnoticed.
//
//    9. GATE 5b -- hemisphericalAlbedo's SUBSTRATE-COUPLING ERROR,
//       MEASURED.  Round 5's route 1 returns
//       `substrate.hemisphericalAlbedo() * (1 - m*EHatMean(alpha)) +
//       sheenColor * EHatMean(alpha)` -- a CLOSED FORM, no kernel table --
//       exact for a Lambertian substrate and an uncorrelated-response
//       approximation for Oren-Nayar and GGX.  This runs a brute-force
//       double quadrature of the REAL fabric `value()` and reports the
//       relative error DECOMPOSED into the part fabric_material owns
//       and the part it merely inherits from the substrate's own
//       hemisphericalAlbedo.  The tolerance is PRE-COMMITTED at 5 %
//       (kHemiAlbedoTol below).  The LAMBERTIAN row is the harness's own
//       control: route 1 is exact there BY CONSTRUCTION, so a non-zero
//       factorisation error on that row indicts the quadrature rather
//       than the model.
//
//   10. GATE 8 -- SPECTRAL PARITY AT AUTHORED WHITE.  Three checks, and
//       the split matters.  (i) The GUARD ITSELF, asserted directly on
//       the painter under an EXACT oracle: `GuardedGetColorNM` must
//       return literally 1.0 at every wavelength, plus a red-proof that
//       the raw unguarded uplift is < 1 somewhere so the guard is
//       load-bearing.  (ii) END-TO-END through the material, to <= 4
//       ulp rather than bit-equality -- every input is bit-identical by
//       (i), so the 1 ulp measured is the RGB and NM expressions'
//       floating-point evaluation differing under `-ffast-math`, which
//       is not a guard failure.  (iii) A negative control: a saturated
//       dye MUST be wavelength-dependent, or (i) and (ii) would also
//       pass with the tint term dead.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>   // std::min / std::max -- used by the fabric energy helpers
#ifdef _WIN32
	#include <process.h>
	#include <io.h>
	#define getpid _getpid
	#define RISE_TEST_DUP    _dup
	#define RISE_TEST_DUP2   _dup2
	#define RISE_TEST_CLOSE  _close
	#define RISE_TEST_FILENO _fileno
#else
	#include <unistd.h>
	#define RISE_TEST_DUP    dup
	#define RISE_TEST_DUP2   dup2
	#define RISE_TEST_CLOSE  close
	#define RISE_TEST_FILENO fileno
#endif

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IJobPriv.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Utilities/Reference.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Materials/FabricPresets.h"
#include "../src/Library/Materials/SheenDirectionalAlbedo.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/OrenNayarMaterial.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "WeaveTestFixture.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"
#include "../src/Library/Interfaces/ILogPriv.h"
#include "../src/Library/Interfaces/ILogPrinter.h"
#include <mutex>

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const std::string& name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

namespace {

//! GATE 5b's PRE-COMMITTED tolerance.  Stated here, before the
//! measurement, exactly as 9.9 gate 5(b) requires.  Do NOT widen it
//! after seeing a number: the doc's remedy for an exceedance is the
//! per-substrate-class 3D table S(alpha, m, sigma_base), not a looser
//! band.
static const double kHemiAlbedoTol = 0.05;		// 5 %, relative

//! The sheen alpha every gate-5b row is measured at.  0.3 is comfortably
//! above FabricBRDF::kMinSheenAlpha (so the clamp is not what is being
//! measured) and in the middle of the presets' range.
static const double kSheenAlpha = 0.3;

//////////////////////////////////////////////////////////////////////
// Scene plumbing -- CoatedMaterialChunkTest's pattern, unchanged.
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_fabricmat_" + tag + "_" + pid + ".RISEscene";
	std::ofstream f( path.c_str(), std::ios::binary | std::ios::trunc );
	f << body;
	f.close();
	return path;
}

bool ParseBodyInto( const std::string& tag, const std::string& body, IJobPriv& job )
{
	const std::string path = WriteTempScene( tag, "RISE ASCII SCENE 7\n" + body );
	const bool ok = job.LoadAsciiSceneViaCst( path.c_str() );
	remove( path.c_str() );
	return ok;
}

//! ParseBodyInto with stdout captured, so the diagnostics
//! Job::AddFabricMaterial emits can be checked for their actual WORDING
//! -- and, for the preset mismatch, for their SEVERITY -- rather than
//! merely for a boolean.
bool ParseBodyCapturing( const std::string& tag, const std::string& body, IJobPriv& job,
                          std::string& capturedOutput )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_fabricmat_stdout_" + tag + "_" + pidbuf + ".txt";

	std::fflush( stdout );
	const int savedFd = RISE_TEST_DUP( RISE_TEST_FILENO( stdout ) );
	FILE* capFile = std::fopen( capPath.c_str(), "w" );
	if( capFile ) RISE_TEST_DUP2( RISE_TEST_FILENO( capFile ), RISE_TEST_FILENO( stdout ) );

	const bool ok = ParseBodyInto( tag, body, job );

	std::fflush( stdout );
	if( savedFd >= 0 ) { RISE_TEST_DUP2( savedFd, RISE_TEST_FILENO( stdout ) ); RISE_TEST_CLOSE( savedFd ); }
	if( capFile ) std::fclose( capFile );

	std::ifstream ifs( capPath.c_str() );
	if( ifs.is_open() ) { std::ostringstream oss; oss << ifs.rdbuf(); capturedOutput = oss.str(); }
	remove( capPath.c_str() );

	return ok;
}

bool Contains( const std::string& hay, const char* needle )
{
	return hay.find( needle ) != std::string::npos;
}

//////////////////////////////////////////////////////////////////////
// A capturing ILogPrinter that records each matching message's
// SEVERITY, not just its text.
//
// WHY TEXT IS NOT ENOUGH (M3 review, 2026-09-02).  The stdout-redirect
// technique the rest of this file uses cannot see severity at all:
// `StreamPrinter::Print` writes only `event.szMessage`, with no type
// tag.  So a check that reads "expects a ggx_material substrate" out of
// stdout is INFERRING warning-ness from wording -- and the single most
// important property of this particular diagnostic is that it is a
// WARNING and not an ERROR, because erroring would refuse a legal
// composition (`fabric satin` over a Lambertian is chalk with a faint
// sheen, which an author may well want) while going silent would let it
// look like it worked.  A future change that reclassified the severity
// while keeping similar text would sail through a text-only check.
//
// Installed once and never removed, matching CsgOperandTransformTest's
// and CstSourceInstanceTest's precedent.
class CapturingLogPrinter : public virtual RISE::ILogPrinter,
                            public virtual RISE::Implementation::Reference
{
public:
	explicit CapturingLogPrinter( std::string needle ) : mNeedle( std::move( needle ) ) {}

	void Print( const RISE::LogEvent& event ) override
	{
		const std::string msg( event.szMessage );
		if( msg.find( mNeedle ) != std::string::npos ) {
			std::lock_guard<std::mutex> lk( mMutex );
			mTypes.push_back( event.eType );
		}
	}
	void Flush() override {}

	int  Count() const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		return (int)mTypes.size();
	}
	//! True iff at least one match was recorded and EVERY match carried
	//! exactly `want`.  "Every" matters: a diagnostic emitted twice at
	//! two different severities is also a defect.
	bool AllOfType( RISE::LOG_ENUM want ) const
	{
		std::lock_guard<std::mutex> lk( mMutex );
		if( mTypes.empty() ) return false;
		for( RISE::LOG_ENUM t : mTypes ) { if( t != want ) return false; }
		return true;
	}
	void Reset()
	{
		std::lock_guard<std::mutex> lk( mMutex );
		mTypes.clear();
	}

protected:
	~CapturingLogPrinter() override {}

private:
	std::string                    mNeedle;
	mutable std::mutex             mMutex;
	std::vector<RISE::LOG_ENUM>    mTypes;
};

//! Installed lazily by TestPresetSubstrateMismatchWarns; raw read handle
//! (AddPrinter addref'd the object, which is never removed).
CapturingLogPrinter* g_mismatchLog = 0;

//////////////////////////////////////////////////////////////////////
// Scene fragments
//////////////////////////////////////////////////////////////////////

std::string ColorPainter( const char* name, const char* rgb )
{
	std::ostringstream o;
	o << "uniformcolor_painter\n{\n\tname\t" << name << "\n\tcolor\t" << rgb
	  << "\n\tcolorspace\tRec709RGB_Linear\n}\n";
	return o.str();
}

//! NOTE the 17-digit precision: an ostream's DEFAULT is 6 significant
//! digits, which would make a named painter and the inline literal of
//! "the same" angle differ in the 7th digit -- and the bit-exactness
//! check below would then be measuring the test harness rather than the
//! parser.
std::string ScalarPainter( const char* name, double v )
{
	std::ostringstream o;
	o.precision( 17 );
	o << "scalar_painter\n{\n\tname\t" << name << "\n\tvalue\t" << v << "\n}\n";
	return o.str();
}

//! A PER-CHANNEL scalar painter, for the requireSingle negative.
//! `scalar_painter`'s form-2 `values` slot builds an RGBScalarPainter,
//! whose HasPerChannelVariation() is what requireSingle rejects; form 1
//! (`value`) builds a UniformScalarPainter and would NOT trip it -- so
//! the slot name here is load-bearing, not incidental.
std::string RGBScalarPainter( const char* name, const char* rgb )
{
	std::ostringstream o;
	o << "scalar_painter\n{\n\tname\t" << name << "\n\tvalues\t" << rgb << "\n}\n";
	return o.str();
}

std::string LambertianMat( const char* name, const char* painter )
{
	std::ostringstream o;
	o << "lambertian_material\n{\n\tname\t" << name << "\n\treflectance\t" << painter << "\n}\n";
	return o.str();
}

std::string OrenNayarMat( const char* name, const char* painter, double sigma )
{
	std::ostringstream o;
	o << "orennayar_material\n{\n\tname\t" << name << "\n\treflectance\t" << painter
	  << "\n\troughness\t" << sigma << "\n}\n";
	return o.str();
}

//! An anisotropic (or isotropic, when ax == ay) `ggx_material` in the
//! dielectric `schlick_f0` mode fabric actually wants -- see 9.3's
//! dagger note: a GGX base minted with only roughness and `rd` renders
//! with NO dielectric specular at all, which is the single most likely
//! way to hand-author a silent black satin.
std::string GgxMat( const char* name, const char* diffuse, const char* f0,
                     double ax, double ay )
{
	std::ostringstream o;
	o << "ggx_material\n{\n\tname\t" << name << "\n\trd\t" << diffuse << "\n\trs\t" << f0
	  << "\n\talphax\t" << ax << "\n\talphay\t" << ay
	  << "\n\tfresnel_mode\tschlick_f0\n}\n";
	return o.str();
}

//! `fabric_material` fragment; omitted parameters exercise the preset
//! seeding and the descriptor defaults.
std::string FabricMat( const char* name, const char* base,
                        const char* fabric = 0, const char* color = 0,
                        const char* rough = 0, const char* rot = 0 )
{
	std::ostringstream o;
	o << "fabric_material\n{\n\tname\t" << name << "\n";
	if( fabric ) o << "\tfabric\t"          << fabric << "\n";
	o << "\tbase\t" << base << "\n";
	if( color )  o << "\tsheen_color\t"     << color << "\n";
	if( rough )  o << "\tsheen_roughness\t" << rough << "\n";
	if( rot )    o << "\tweave_rotation\t"  << rot   << "\n";
	o << "}\n";
	return o.str();
}

//////////////////////////////////////////////////////////////////////
// A fixed shading point + probe pair, so materials can be compared by
// BEHAVIOUR rather than by poking at their members.
//////////////////////////////////////////////////////////////////////

RayIntersectionGeometric MakeProbe()
{
	const double th = 40.0 * PI / 180.0;
	const Vector3 inDir( sin(th), 0, -cos(th) );
	Ray inRay( Point3( sin(th), 0, 1.0 ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

//! An off-axis light direction, chosen so an ANISOTROPIC substrate
//! responds differently under a frame rotation (a light in the x-z
//! plane with the view also in the x-z plane would leave a rotation
//! about z detectable, but a generic azimuth makes the signal larger).
Vector3 ProbeLight()
{
	return Vector3Ops::Normalize( Vector3( -0.3, 0.5, 0.8 ) );
}

//! Max-channel BRDF response of a registered material at the probe.
//! Returns -1 when the material (or its BSDF) is missing.
double Respond( IJobPriv& job, const char* matName )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe();
	return ColorMath::MaxValue( m->GetBSDF()->value( ProbeLight(), ri ) );
}

//! Channel-0 BRDF response -- used where the max-channel reduction
//! would hide a per-channel difference (the tint checks).
double RespondCh0( IJobPriv& job, const char* matName )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe();
	return m->GetBSDF()->value( ProbeLight(), ri )[0];
}

double RespondNM( IJobPriv& job, const char* matName, double nm )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe();
	return m->GetBSDF()->valueNM( ProbeLight(), ri, nm );
}

bool Registered( IJobPriv& job, const char* matName )
{
	return job.GetMaterials()->GetItem( matName ) != 0;
}

//! A fabric material must actually BE a FabricMaterial, not merely
//! something that registered under the name.
bool IsFabric( IJobPriv& job, const char* matName )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	return m && dynamic_cast<FabricMaterial*>( m ) != 0;
}

//////////////////////////////////////////////////////////////////////
// 1. The preset table, slot by slot
//////////////////////////////////////////////////////////////////////

void TestPresetsSeedTheSlots()
{
	std::cout << "PresetsSeedTheSlots" << std::endl;

	// Every preset, against a hand-written `custom` twin.  The table's
	// numbers are restated HERE, literally, rather than read back out of
	// FabricPresets.h: a test that read the same constant it is checking
	// would stay green through any edit to it, which is exactly the
	// drift this check exists to catch.
	struct Row { const char* name; const char* rough; const char* color; };
	static const Row rows[] = {
		{ "cotton", "0.55", "white" },
		{ "linen",  "0.65", "white" },
		{ "denim",  "0.45", "white" },
		{ "wool",   "0.75", "white" },
		{ "silk",   "0.20", "white" },
		{ "satin",  "0.12", "white" },
		{ "velvet", "0.08", "dark"  },		// the ONE preset that sets a colour
		{ "custom", "0.50", "white" },
	};

	for( const Row& r : rows )
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pnt",   "0.6 0.4 0.3" )
		                 + ColorPainter( "white", "1 1 1" )
		                 + ColorPainter( "dark",  "0.3 0.3 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + FabricMat( "preset",   "base", r.name )
		                 + FabricMat( "explicit", "base", "custom", r.color, r.rough );
		ParseBodyInto( std::string( "preset_" ) + r.name, body, *job );

		const double seeded = Respond( *job, "preset" );
		const double spelled = Respond( *job, "explicit" );

		Check( IsFabric( *job, "preset" ),
		       std::string( "fabric " ) + r.name + " parses and registers" );
		Check( seeded > 0,
		       std::string( "fabric " ) + r.name + " responds" );
		Check( seeded == spelled,
		       std::string( "fabric " ) + r.name + " seeds EXACTLY the table's slots "
		       "(roughness + colour), bit-identically to a hand-written custom chunk" );

		safe_release( job );
	}

	// The presets must not all be the same material: at least two
	// distinct responses across the table, or the "seeding" above could
	// be a no-op agreeing with a no-op.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + FabricMat( "velvet", "base", "velvet" )
		                 + FabricMat( "wool",   "base", "wool" );
		ParseBodyInto( "preset_distinct", body, *job );
		Check( Respond( *job, "velvet" ) != Respond( *job, "wool" ),
		       "different presets give different materials (the seeding is not a no-op)" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 2. Explicit slots win over the preset (the `Has()` rule)
//////////////////////////////////////////////////////////////////////

void TestExplicitSlotsWin()
{
	std::cout << "ExplicitSlotsWin" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt",   "0.6 0.4 0.3" )
	                 + ColorPainter( "white", "1 1 1" )
	                 + ColorPainter( "dark",  "0.3 0.3 0.3" )
	                 + LambertianMat( "base", "pnt" )
	                 // velvet seeds roughness 0.08 AND a dark colour.
	                 + FabricMat( "plain",     "base", "velvet" )
	                 // Author overrides ONLY the roughness: the preset's
	                 // COLOUR must still be inherited.
	                 + FabricMat( "overrough", "base", "velvet", 0, "0.55" )
	                 // The hand-written equivalent of that mixture.
	                 + FabricMat( "mixture",   "base", "custom", "dark", "0.55" )
	                 // Author overrides ONLY the colour: the preset's
	                 // ROUGHNESS must still be inherited.
	                 + FabricMat( "overcolor", "base", "velvet", "white" )
	                 + FabricMat( "mixture2",  "base", "custom", "white", "0.08" );
	ParseBodyInto( "explicit", body, *job );

	const double plain     = Respond( *job, "plain" );
	const double overrough = Respond( *job, "overrough" );
	const double mixture   = Respond( *job, "mixture" );
	const double overcolor = Respond( *job, "overcolor" );
	const double mixture2  = Respond( *job, "mixture2" );

	Check( plain > 0 && overrough > 0, "override fixtures parse and respond" );
	Check( overrough != plain,
	       "an explicitly written sheen_roughness OVERRIDES the preset's" );
	Check( overrough == mixture,
	       "overriding roughness alone still INHERITS the preset's colour" );
	Check( overcolor != plain,
	       "an explicitly written sheen_color OVERRIDES the preset's" );
	Check( overcolor == mixture2,
	       "overriding colour alone still INHERITS the preset's roughness" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 3. The substrate allowlist, as a refusal matrix
//////////////////////////////////////////////////////////////////////

void TestAllowlistAccepts()
{
	std::cout << "AllowlistAccepts" << std::endl;

	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + FabricMat( "cloth", "base", "velvet" );
		ParseBodyInto( "acc_lamb", body, *job );
		Check( IsFabric( *job, "cloth" ), "lambertian substrate accepted" );
		safe_release( job );
	}
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + OrenNayarMat( "base", "pnt", 0.4 )
		                 + FabricMat( "cloth", "base", "cotton" );
		ParseBodyInto( "acc_on", body, *job );
		Check( IsFabric( *job, "cloth" ), "orennayar substrate accepted" );
		safe_release( job );
	}
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pd", "0.6 0.4 0.3" )
		                 + ColorPainter( "ps", "0.04 0.04 0.04" )
		                 + GgxMat( "base", "pd", "ps", 0.30, 0.10 )
		                 + FabricMat( "cloth", "base", "silk" );
		ParseBodyInto( "acc_ggx", body, *job );
		Check( IsFabric( *job, "cloth" ), "ggx substrate accepted" );
		safe_release( job );
	}
	// pbr_metallic_roughness -- accepted BECAUSE it resolves to a
	// ggx_material at scene-build time, which is the whole reason the
	// allowlist needs no separate case for it.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pbase", "0.6 0.4 0.3" )
		                 + "pbr_metallic_roughness_material\n{\n\tname\tbase\n"
		                   "\tbase_color\tpbase\n\tmetallic\t0.0\n\troughness\t0.4\n}\n"
		                 + FabricMat( "cloth", "base", "denim" );
		ParseBodyInto( "acc_pbr", body, *job );
		Check( IsFabric( *job, "cloth" ),
		       "pbr_metallic_roughness substrate accepted (resolves to ggx)" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 3a. The allowlist TEXT itself -- REVIEW_P2R2.md P1: `IsSupportedSubstrate`
//     and `SubstrateClassText` were extended for `weave_material` in the
//     same diff that left `SubstrateAllowlistText()` unchanged, so the
//     refusal/diagnostic MESSAGE still named only four classes while a
//     fifth was silently accepted.  No test asserted the literal string,
//     so the omission went undetected.  This pins the exact text --
//     Job::AddFabricMaterial and RISE_API_CreateFabricMaterial print it
//     verbatim on refusal, and AgentSession's make_fabric refusal
//     messages quote it too, so a drift here is a drift everywhere an
//     author or the agent sees "Supported:".
//////////////////////////////////////////////////////////////////////

void TestAllowlistTextNamesWeave()
{
	std::cout << "AllowlistTextNamesWeave" << std::endl;

	const std::string text = RISE::Implementation::FabricMaterial::SubstrateAllowlistText();
	Check( text.find( "lambertian_material" ) != std::string::npos,
	       "allowlist text names lambertian_material" );
	Check( text.find( "orennayar_material" ) != std::string::npos,
	       "allowlist text names orennayar_material" );
	Check( text.find( "ggx_material" ) != std::string::npos,
	       "allowlist text names ggx_material" );
	Check( text.find( "pbr_metallic_roughness_material" ) != std::string::npos,
	       "allowlist text names pbr_metallic_roughness_material" );
	Check( text.find( "weave_material" ) != std::string::npos,
	       "allowlist text names weave_material -- REVIEW_P2R2.md P1's omission" );
	Check( text == "lambertian_material, orennayar_material, ggx_material, "
	               "pbr_metallic_roughness_material, weave_material",
	       "allowlist text is exactly the FIVE-class string Job/RISE_API/AgentSession all quote" );
}

void TestAllowlistRefusals()
{
	std::cout << "AllowlistRefusals" << std::endl;

	// (a) base never registered
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		ParseBodyCapturing( "ref_missing", FabricMat( "cloth", "nosuchmaterial" ), *job, out );
		Check( !Registered( *job, "cloth" ), "unregistered base: material not registered" );
		Check( Contains( out.c_str(), "is not a registered material" ),
		       "unregistered base: diagnostic names the cause" );
		safe_release( job );
	}

	// (b) luminaire base -- refused even though its SCATTERING class
	//     (lambertian) is on the list.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pnt", "0.6 0.6 0.6" )
		                 + LambertianMat( "inner", "pnt" )
		                 + "lambertian_luminaire_material\n{\n\tname\tbase\n\texitance\tpnt\n"
		                   "\tmaterial\tinner\n\tscale\t1.0\n}\n"
		                 + FabricMat( "cloth", "base" );
		ParseBodyCapturing( "ref_lum", body, *job, out );
		Check( !Registered( *job, "cloth" ), "luminaire base: material not registered" );
		Check( Contains( out.c_str(), "the substrate is a luminaire" ),
		       "luminaire base: diagnostic names the emitter" );
		Check( Contains( out.c_str(), "lambertian_material, orennayar_material, ggx_material" ),
		       "luminaire base: diagnostic names the allowlist" );
		safe_release( job );
	}

	// (c) base with no BSDF/SPF at all
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ScalarPainter( "tau", 1.0 )
		                 + "dielectric_material\n{\n\tname\tbase\n\ttau\ttau\n\tior\t1.5\n"
		                   "\tscattering\t1000000\n}\n"
		                 + FabricMat( "cloth", "base" );
		ParseBodyCapturing( "ref_diel", body, *job, out );
		Check( !Registered( *job, "cloth" ), "no-BSDF base: material not registered" );
		Check( Contains( out.c_str(), "no BSDF" ),
		       "no-BSDF base: diagnostic names the missing BSDF/SPF" );
		safe_release( job );
	}

	// (d) supported PIPE, unsupported CLASS
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pnt", "0.5 0.5 0.5" )
		                 + "cooktorrance_material\n{\n\tname\tbase\n\trd\tpnt\n\trs\tpnt\n"
		                   "\tfacets\t0.2\n\tior\t1.5\n\textinction\t0.0\n}\n"
		                 + FabricMat( "cloth", "base" );
		ParseBodyCapturing( "ref_ct", body, *job, out );
		Check( !Registered( *job, "cloth" ), "cooktorrance base: material not registered" );
		Check( Contains( out.c_str(), "not one of the supported scattering classes" ),
		       "cooktorrance base: diagnostic names the class" );
		safe_release( job );
	}

	// (e) fabric over fabric -- the allowlist closes this recursion too.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + FabricMat( "cloth1", "base" )
		                 + FabricMat( "cloth2", "cloth1" );
		ParseBodyCapturing( "ref_fabfab", body, *job, out );
		Check( IsFabric( *job, "cloth1" ), "fabric-over-fabric: the inner fabric still registers" );
		Check( !Registered( *job, "cloth2" ), "fabric-over-fabric: the outer fabric is refused" );
		Check( Contains( out.c_str(), "not one of the supported scattering classes" ),
		       "fabric-over-fabric: diagnostic names the class" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 4. The preset-vs-substrate MISMATCH is a WARNING, not an error
//////////////////////////////////////////////////////////////////////

void TestPresetSubstrateMismatchWarns()
{
	std::cout << "PresetSubstrateMismatchWarns" << std::endl;

	// Install the severity sink once.  The needle is the stable stem of
	// the mismatch diagnostic; the surrounding wording names both the
	// expected and the bound class and may be reworded, but "expects a"
	// is what identifies THIS diagnostic.
	if( !g_mismatchLog ) {
		CapturingLogPrinter* owned = new CapturingLogPrinter( "expects a" );
		RISE::GlobalLogPriv()->AddPrinter( owned );
		g_mismatchLog = owned;		// AddPrinter addref'd; raw read handle
	}
	g_mismatchLog->Reset();

	// `fabric satin` over a LAMBERTIAN: the classic mistake 9.3 calls
	// "chalk with a faint sheen".  It must WARN and still build.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + FabricMat( "cloth", "base", "satin" );
		ParseBodyCapturing( "mismatch_satin", body, *job, out );
		Check( IsFabric( *job, "cloth" ),
		       "satin-over-lambertian STILL REGISTERS (the composition is legal)" );
		// PHASE 2 MOVED THIS EXPECTATION, and the move is the point of
		// the update rather than an incidental edit: `satin` used to
		// recommend an anisotropic `ggx_material`, and 9.9 gate 9b
		// measured that composition and found it reads as brushed metal
		// (95-99 % of the substrate's anisotropy survives the sheen and
		// it STILL has no pattern scale).  The preset now recommends a
		// `weave_material`, so the class this diagnostic names moved
		// with it.  The GGX numbers stay in FabricPresets.h as the
		// documented Phase-1 fallback.
		Check( Contains( out.c_str(), "expects a weave_material substrate" ),
		       "satin-over-lambertian warns, naming the class the preset wanted" );
		Check( Contains( out.c_str(), "make_fabric" ),
		       "the mismatch warning points at the verb that would fix it" );
		Check( !Contains( out.c_str(), "is not a supported substrate" ),
		       "the mismatch does NOT emit the allowlist REFUSAL (warning != error)" );

		// §9.3 requires the message to name the class the author ACTUALLY
		// bound, not only its name -- otherwise they learn what was
		// expected but not what they wrote.
		Check( Contains( out.c_str(), "bound to lambertian_material `base`" ),
		       "the mismatch warning names the BOUND substrate's actual class, not just its name" );

		// THE SEVERITY ITSELF, read off the log event rather than
		// inferred from wording.  This is the check that survives a
		// rewording and fails on a reclassification.
		Check( g_mismatchLog->Count() > 0,
		       "the severity sink saw the mismatch diagnostic" );
		Check( g_mismatchLog->AllOfType( RISE::eLog_Warning ),
		       "the preset/substrate mismatch is logged at eLog_WARNING severity "
		       "(not eLog_Error -- the composition is legal)" );

		safe_release( job );
	}

	// The matching case must be SILENT -- otherwise the warning is
	// noise, not information.  `cotton` over an `orennayar_material` is
	// used because it is a preset whose recommendation Phase 2 did NOT
	// move; picking one of the three that moved would make this control
	// a moving target every time the table is retuned.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pd", "0.6 0.4 0.3" )
		                 + OrenNayarMat( "base", "pd", 0.4 )
		                 + FabricMat( "cloth", "base", "cotton" );
		g_mismatchLog->Reset();
		ParseBodyCapturing( "mismatch_ok", body, *job, out );
		Check( IsFabric( *job, "cloth" ), "cotton-over-orennayar registers" );
		Check( !Contains( out.c_str(), "expects a" ),
		       "cotton-over-OrenNayar does NOT warn (the preset's class matched)" );
		Check( g_mismatchLog->Count() == 0,
		       "cotton-over-OrenNayar emits NO mismatch diagnostic at any severity" );
		safe_release( job );
	}

	// AND THE PHASE-2 MATCH: `satin` over a `weave_material` is now the
	// composition the preset recommends, so it must be the silent one.
	// Its Phase-1 shape -- satin over a bare anisotropic GGX -- must now
	// WARN, and both halves are checked here because a change that
	// forgot one of them would leave the diagnostic pointing at the
	// wrong composition without failing anything.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = "weave_material\n{\n\tname\twbase\n\tfabric\tsatin\n}\n"
		                 + FabricMat( "cloth", "wbase", "satin" );
		g_mismatchLog->Reset();
		ParseBodyCapturing( "mismatch_weave_ok", body, *job, out );
		Check( IsFabric( *job, "cloth" ), "satin-over-weave registers" );
		Check( !Contains( out.c_str(), "expects a" ),
		       "satin-over-WEAVE does NOT warn (Phase 2's recommended class)" );
		Check( g_mismatchLog->Count() == 0,
		       "satin-over-WEAVE emits NO mismatch diagnostic at any severity" );
		safe_release( job );
	}

	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pd", "0.6 0.4 0.3" )
		                 + ColorPainter( "ps", "0.04 0.04 0.04" )
		                 + GgxMat( "base", "pd", "ps", 0.34, 0.06 )
		                 + FabricMat( "cloth", "base", "satin" );
		g_mismatchLog->Reset();
		ParseBodyCapturing( "mismatch_satin_ggx", body, *job, out );
		Check( IsFabric( *job, "cloth" ), "satin-over-ggx STILL REGISTERS (still legal)" );
		Check( Contains( out.c_str(), "expects a weave_material substrate" ),
		       "satin-over-GGX now WARNS -- the Phase-1 shape is the documented fallback, "
		       "not the recommendation" );
		safe_release( job );
	}

	// `custom` recommends nothing, so it must never warn -- over any
	// allowlisted base.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + FabricMat( "cloth", "base", "custom" );
		g_mismatchLog->Reset();
		ParseBodyCapturing( "mismatch_custom", body, *job, out );
		Check( IsFabric( *job, "cloth" ), "custom-over-lambertian registers" );
		Check( !Contains( out.c_str(), "expects a" ),
		       "`fabric custom` never warns (it recommends no substrate)" );
		Check( g_mismatchLog->Count() == 0,
		       "`fabric custom` emits NO mismatch diagnostic at any severity" );
		safe_release( job );
	}

	// A PBR BASE.  This row used to prove that PBR satisfies a GGX
	// recommendation TRANSITIVELY (it resolves to a `ggx_material` at
	// scene-build time), driven by `silk` -- whose recommendation moved
	// to `weave_material` in Phase 2, so silk-over-PBR now warns like
	// any other non-matching class.  The transitive-ALLOWLIST claim,
	// which is the one that matters for glTF import, is unaffected and
	// is checked by TestAllowlistAccepts.
	//
	// What is checked here instead is that a PBR base does not emit a
	// SPURIOUS diagnostic under a preset that recommends nothing --
	// `fabric custom`, which is what the glTF importer actually mints
	// (it carries no preset).  A warning there would put a diagnostic on
	// every imported KHR_materials_sheen asset.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pbase", "0.6 0.4 0.3" )
		                 + "pbr_metallic_roughness_material\n{\n\tname\tbase\n"
		                   "\tbase_color\tpbase\n\tmetallic\t0.0\n\troughness\t0.4\n}\n"
		                 + FabricMat( "cloth", "base", "custom" );
		g_mismatchLog->Reset();
		ParseBodyCapturing( "mismatch_pbr", body, *job, out );
		Check( IsFabric( *job, "cloth" ), "custom-over-PBR registers" );
		Check( !Contains( out.c_str(), "expects a" ),
		       "custom-over-PBR does NOT warn (the glTF importer's own shape)" );
		Check( g_mismatchLog->Count() == 0,
		       "custom-over-PBR emits NO mismatch diagnostic at any severity" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 5. sheen_color's `none` -> preset-colour-else-WHITE
//////////////////////////////////////////////////////////////////////

void TestSheenColorDefault()
{
	std::cout << "SheenColorDefault" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt",   "0.6 0.4 0.3" )
	                 + ColorPainter( "white", "1 1 1" )
	                 + ColorPainter( "black", "0 0 0" )
	                 + LambertianMat( "base", "pnt" )
	                 // `custom` sets no colour, so an omitted slot must
	                 // become WHITE -- not the manager's black `none`.
	                 + FabricMat( "omitted",  "base", "custom" )
	                 + FabricMat( "expwhite", "base", "custom", "white" )
	                 + FabricMat( "expblack", "base", "custom", "black" );
	ParseBodyInto( "color", body, *job );

	const double omitted = RespondCh0( *job, "omitted" );
	const double white   = RespondCh0( *job, "expwhite" );
	const double black   = RespondCh0( *job, "expblack" );

	Check( omitted > 0, "omitted sheen_color parses and responds" );
	Check( omitted == white,
	       "omitted sheen_color behaves EXACTLY like an explicit white one" );
	// If `none` were resolved through the painter manager it would bind
	// the built-in BLACK painter and this would be an equality instead.
	Check( black >= 0 && black != omitted,
	       "an explicitly BLACK sheen_color differs -- `none` is not being read as black" );
	// A black dye switches the lobe off AND stops suppressing the base,
	// so it must be BRIGHTER than white at this geometry (m = 0 means
	// the substrate is returned untouched).
	Check( black > 0,
	       "a black sheen_color leaves the substrate fully unattenuated (m = 0)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 6. weave_rotation reaches the SUBSTRATE's frame
//////////////////////////////////////////////////////////////////////

void TestWeaveRotationReachesTheSubstrate()
{
	std::cout << "WeaveRotationReachesTheSubstrate" << std::endl;

	// (a) ANISOTROPIC ggx base: a rotation MUST move the response.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pd", "0.6 0.4 0.3" )
		                 + ColorPainter( "ps", "0.04 0.04 0.04" )
		                 + GgxMat( "base", "pd", "ps", 0.34, 0.06 )
		                 + ScalarPainter( "quarter", 0.7853981633974483 )	// pi/4
		                 + FabricMat( "unrot",   "base", "satin" )
		                 + FabricMat( "rotlit",  "base", "satin", 0, 0, "0.7853981633974483" )
		                 + FabricMat( "rotpnt",  "base", "satin", 0, 0, "quarter" )
		                 + FabricMat( "rotzero", "base", "satin", 0, 0, "0.0" );
		ParseBodyInto( "weave_aniso", body, *job );

		const double unrot   = Respond( *job, "unrot" );
		const double rotlit  = Respond( *job, "rotlit" );
		const double rotpnt  = Respond( *job, "rotpnt" );
		const double rotzero = Respond( *job, "rotzero" );

		Check( unrot > 0, "anisotropic-base fabric responds" );
		Check( rotlit != unrot,
		       "weave_rotation MOVES an anisotropic substrate's response" );
		Check( rotlit == rotpnt,
		       "an inline literal and a named scalar_painter at the same angle agree bit-for-bit" );
		Check( rotzero == unrot,
		       "weave_rotation 0 is EXACTLY the unrotated response (the fast-path no-op)" );
		safe_release( job );
	}

	// (b) ISOTROPIC ggx base: a rotation about the normal is a NO-OP,
	//     and must be bit-identical.  Without this half, (a) would also
	//     pass if the rotation were being applied to something it should
	//     not touch (the sheen lobe, the light direction, the normal).
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pd", "0.6 0.4 0.3" )
		                 + ColorPainter( "ps", "0.04 0.04 0.04" )
		                 + GgxMat( "base", "pd", "ps", 0.2, 0.2 )
		                 + FabricMat( "unrot", "base", "custom" )
		                 + FabricMat( "rot",   "base", "custom", 0, 0, "0.7853981633974483" );
		ParseBodyInto( "weave_iso", body, *job );
		Check( Respond( *job, "unrot" ) > 0, "isotropic-base fabric responds" );
		Check( Respond( *job, "unrot" ) == Respond( *job, "rot" ),
		       "weave_rotation is a bit-exact NO-OP on an ISOTROPIC substrate" );
		safe_release( job );
	}

	// (c) The rotation must not disturb the SHEEN lobe.  Over a BLACK
	//     Lambertian base -- i.e. with the substrate contributing
	//     nothing -- rotating the weave must change nothing at all.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "blk",   "0 0 0" )
		                 + ColorPainter( "white", "1 1 1" )
		                 + LambertianMat( "base", "blk" )
		                 + FabricMat( "unrot", "base", "custom", "white" )
		                 + FabricMat( "rot",   "base", "custom", "white", 0, "1.1" );
		ParseBodyInto( "weave_sheenonly", body, *job );
		Check( Respond( *job, "unrot" ) > 0, "sheen-only fabric responds" );
		Check( Respond( *job, "unrot" ) == Respond( *job, "rot" ),
		       "weave_rotation leaves the ISOTROPIC sheen lobe bit-identical" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 6b. THE ROUGHNESS FLOOR, tested behaviourally.
//
// FabricBRDF.h documents at length that `kMinSheenAlpha` = 0.04 is what
// keeps `1 - m*Ehat >= 0` and the product form exactly conserving for
// n.v >= 0.03, and `ResolveFabric`'s `r_max( kMinSheenAlpha, rawAlpha )`
// is the one line that enforces it.  Until now NOTHING exercised that
// line: SheenDirectionalAlbedoTest validates the raw table, and no
// scene or chunk case authored a roughness below the floor.  Deleting
// the clamp was therefore a free mutation -- every gate in the slice
// stayed green.  (M3 review, 2026-09-02.)
//
// BOTH halves are needed.  "0.01 behaves like 0.04" alone would also
// pass if `sheen_roughness` were ignored entirely; "0.05 differs from
// 0.04" alone would pass with no clamp at all.  Together they pin the
// clamp AND its threshold.
//////////////////////////////////////////////////////////////////////

void TestRoughnessFloor()
{
	std::cout << "RoughnessFloor" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt",   "0.6 0.4 0.3" )
	                 + ColorPainter( "white", "1 1 1" )
	                 + LambertianMat( "base", "pnt" )
	                 // Three roughnesses: one WELL below the floor, one
	                 // AT it, and one just above.
	                 + FabricMat( "below", "base", "custom", "white", "0.01" )
	                 + FabricMat( "at",    "base", "custom", "white", "0.04" )
	                 + FabricMat( "above", "base", "custom", "white", "0.05" );
	ParseBodyInto( "roughfloor", body, *job );

	IMaterial* mBelow = job->GetMaterials()->GetItem( "below" );
	IMaterial* mAt    = job->GetMaterials()->GetItem( "at" );
	IMaterial* mAbove = job->GetMaterials()->GetItem( "above" );
	Check( mBelow && mAt && mAbove, "roughness-floor fixtures registered" );
	if( !mBelow || !mAt || !mAbove ) { safe_release( job ); return; }

	// Several directions, not one: a single probe could coincide by
	// accident, and the clamp must hold across the hemisphere -- including
	// near grazing, which is where the roughness actually matters.
	static const double kThetaDeg[] = { 5.0, 30.0, 60.0, 80.0, 88.0 };
	int clampedSame = 0, aboveDiffers = 0, probes = 0;

	for( double td : kThetaDeg )
	{
		const double th = td * PI / 180.0;
		const Vector3 inDir( sin(th), 0, -cos(th) );
		Ray inRay( Point3( sin(th), 0, 1.0 ), inDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );

		const Vector3 l = ProbeLight();
		const double below = ColorMath::MaxValue( mBelow->GetBSDF()->value( l, ri ) );
		const double at    = ColorMath::MaxValue( mAt->GetBSDF()->value( l, ri ) );
		const double above = ColorMath::MaxValue( mAbove->GetBSDF()->value( l, ri ) );

		++probes;
		if( below == at )    ++clampedSame;
		if( above != at )    ++aboveDiffers;
	}

	std::printf( "    sheen_roughness 0.01 == 0.04 at %d/%d directions; "
	             "0.05 differs from 0.04 at %d/%d\n",
	             clampedSame, probes, aboveDiffers, probes );

	Check( clampedSame == probes,
	       "sheen_roughness BELOW the floor (0.01) is clamped: identical response to 0.04 "
	       "at every probed direction" );
	Check( aboveDiffers == probes,
	       "sheen_roughness ABOVE the floor (0.05) is NOT clamped: it differs from 0.04 "
	       "(so the check above pins the clamp, not an ignored slot)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 7. requireSingle on both scalar slots + editor introspection
//////////////////////////////////////////////////////////////////////

void TestRequireSingleOnScalarSlots()
{
	std::cout << "RequireSingleOnScalarSlots" << std::endl;

	const char* slots[] = { "sheen_roughness", "weave_rotation" };

	for( const char* slot : slots )
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::ostringstream chunk;
		chunk << "fabric_material\n{\n\tname\tcloth\n\tbase\tbase\n\t"
		      << slot << "\tpercc\n}\n";
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + RGBScalarPainter( "percc", "0.2 0.5 0.9" )
		                 + chunk.str();
		ParseBodyCapturing( std::string( "single_" ) + slot, body, *job, out );

		Check( !Registered( *job, "cloth" ),
		       std::string( slot ) + ": per-channel scalar painter refused" );
		safe_release( job );
	}
}

void TestEditorIntrospection()
{
	std::cout << "EditorIntrospection" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
	                 + LambertianMat( "base", "pnt" )
	                 + ScalarPainter( "half", 0.5 )
	                 + FabricMat( "cloth", "base", "wool" );
	ParseBodyInto( "editor", body, *job );

	IMaterial* m = job->GetMaterials()->GetItem( "cloth" );
	Check( m != 0, "introspection fixture registered" );
	if( !m ) { safe_release( job ); return; }

	Check( MaterialIntrospection::GetTypeName( *m ) == String( "Fabric" ),
	       "introspection reports the type name `Fabric`" );

	{
		const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *m, String( "sheen_color" ) );
		Check( ref.kind == MaterialSlotRef::Painter && ref.painter != 0,
		       "GetSlot exposes sheen_color on the COLOUR pipe" );
	}
	const char* scalarSlots[] = { "sheen_roughness", "weave_rotation" };
	for( const char* want : scalarSlots )
	{
		const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *m, String( want ) );
		Check( ref.kind == MaterialSlotRef::ScalarPainter && ref.scalarPainter != 0,
		       std::string( "GetSlot exposes " ) + want + " on the SCALAR pipe" );
	}
	{
		const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *m, String( "base" ) );
		Check( ref.kind == MaterialSlotRef::None,
		       "GetSlot reports `base` as None (a material, not a painter slot)" );
	}

	// SetSlot actually reaches the model.
	const double before = Respond( *job, "cloth" );
	IScalarPainter* half = job->GetScalarPainters()->GetItem( "half" );
	Check( half != 0, "named scalar_painter available for rebind" );
	if( half ) {
		Check( MaterialIntrospection::SetSlot( *m, String( "sheen_roughness" ), 0, half ),
		       "SetSlot accepts sheen_roughness" );
		Check( Respond( *job, "cloth" ) != before,
		       "SetSlot( sheen_roughness ) changes the BRDF response" );
	}

	IPainter* col = job->GetPainters()->GetItem( "pnt" );
	Check( col != 0, "named colour painter available" );
	if( col ) {
		Check( !MaterialIntrospection::SetSlot( *m, String( "sheen_roughness" ), col, 0 ),
		       "SetSlot refuses a COLOUR painter on the scalar sheen_roughness slot" );
		Check( MaterialIntrospection::SetSlot( *m, String( "sheen_color" ), col, 0 ),
		       "SetSlot accepts a colour painter on sheen_color" );
	}
	Check( !MaterialIntrospection::SetSlot( *m, String( "base" ), col, 0 ),
	       "SetSlot refuses `base` (a material, not a painter slot)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 8. The API-level allowlist, exercised directly (no Job)
//////////////////////////////////////////////////////////////////////

void TestApiLevelAllowlist()
{
	std::cout << "ApiLevelAllowlist" << std::endl;

	UniformColorPainter*  col = new UniformColorPainter( RISEPel( 0.6, 0.4, 0.3 ) ); col->addref();
	UniformColorPainter*  wht = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); wht->addref();
	UniformScalarPainter* rgh = new UniformScalarPainter( 0.5 ); rgh->addref();
	UniformScalarPainter* zed = new UniformScalarPainter( 0.0 ); zed->addref();

	{
		LambertianMaterial* lamb = new LambertianMaterial( *col ); lamb->addref();
		IMaterial* out = 0;
		const bool ok = RISE_API_CreateFabricMaterial( &out, *lamb, *wht, *rgh, *zed );
		Check( ok && out != 0, "API accepts a lambertian substrate" );
		safe_release( out );
		safe_release( lamb );
	}
	{
		// PerfectReflectorMaterial has no continuum BSDF at all, so it
		// trips the same branch a dielectric does through the parser.
		PerfectReflectorMaterial* mirror = new PerfectReflectorMaterial( *col ); mirror->addref();
		IMaterial* out = reinterpret_cast<IMaterial*>( 0x1 );	// poison, to prove it is untouched
		const bool ok = RISE_API_CreateFabricMaterial( &out, *mirror, *wht, *rgh, *zed );
		Check( !ok, "API REFUSES a disallowed substrate (perfect reflector)" );
		Check( out == reinterpret_cast<IMaterial*>( 0x1 ),
		       "API leaves the out-pointer untouched on refusal (no partial construction)" );
		safe_release( mirror );
	}
	{
		LambertianMaterial* lamb = new LambertianMaterial( *col ); lamb->addref();
		Check( !RISE_API_CreateFabricMaterial( 0, *lamb, *wht, *rgh, *zed ),
		       "API refuses a null out-pointer" );
		safe_release( lamb );
	}

	safe_release( zed ); safe_release( rgh ); safe_release( wht ); safe_release( col );
}

//////////////////////////////////////////////////////////////////////
// 8b. THE "REFLECTION-ONLY UNCHANGED" LOCK (R8 P1.1).
//
//  WHY THIS EXISTS.  P1.1 taught `fabric_material` to forward a
//  substrate's TRANSMISSION (a `transmission thin` `weave_material`
//  underneath used to be rendered 100 % opaque by the wrapper -- see
//  docs/CLOTH_FABRIC_DESIGN.md 15 debt 22).  The whole safety argument
//  for that change is that a substrate which does NOT transmit is
//  UNTOUCHED: every new term in `FabricBRDF` / `FabricSPF` is reached
//  only through a branch the wrapper takes when
//  `IMaterial::ScattersFullSphere()` is true on the base, so a
//  reflection-only stack must answer exactly as the committed code did.
//
//  NOTHING ELSE IN THE SUITE PINS THAT.  LayeredWhiteFurnaceTest's
//  fabric rows are Monte-Carlo directional albedos with 1-6 % pass
//  bands; SPFBSDFConsistencyTest and SPFPdfConsistencyTest compare the
//  material against ITSELF (kray*pdf vs value*cos, sampler vs density),
//  so a uniform scale on the reflect lobe would satisfy both. This block
//  pins the ABSOLUTE numbers, captured off the COMMITTED (pre-P1.1)
//  binary before a line of `src/` was edited, at a hand-picked grid that
//  covers normal / mid / grazing views, in-plane and out-of-plane
//  lights, and the two below-horizon rows (which must stay EXACTLY 0 for
//  a reflection-only substrate -- the single most likely way to break
//  this is a transmission branch that forgets to ask whether the base
//  can transmit at all).
//
//  TWO SUBSTRATES, deliberately: a Lambertian (the class the product
//  form is exact over) and a `transmission none` satin weave (the class
//  P1.1 actually touches -- the SAME material the thin rows use, with
//  its one enum flipped).
//
//  WHY 1e-9 RELATIVE AND NOT `==`.  The intent IS bit-identity, and on
//  this machine every row reproduces to the last digit. The comparison
//  is banded at 1e-9 relative only so a last-ulp `pow`/`exp`/`atan`
//  difference in another platform's libm cannot fail the suite for a
//  reason that has nothing to do with the model: 1e-9 is ~7 orders
//  tighter than the smallest change any of P1.1's branches could make
//  (they either leave the reflect path alone entirely or move it by
//  O(1)).
//////////////////////////////////////////////////////////////////////

//! Defined with the gate-5b helpers further down; declared here so this
//! block and 8c can sit next to the allowlist tests they belong with.
RayIntersectionGeometric MakeIntersectionFromView( const Vector3& view );
double LocalBaseScaling( double alpha, double m, double cosV, double cosL );

//! One locked probe: view latitude, light latitude (SIGNED -- negative
//! is below the shading normal), light azimuth, and the three numbers
//! the committed code returned there.
struct FabricReflectLockRow
{
	double	muV;			//!< cos of the view latitude, > 0
	double	muL;			//!< cos of the light latitude; NEGATIVE = below the horizon
	double	phiL;			//!< light azimuth, radians
	double	value0;			//!< FabricBRDF::value(...)[0]
	double	valueNM;		//!< FabricBRDF::valueNM(..., 550 nm)
	double	pdf;			//!< FabricSPF::Pdf(...)
};

//! The probe grid, shared by both substrates so a row index means the
//! same geometry in either table.
static const double kFabricLockMuV[]  = { 1.0, 0.7071067811865476, 0.17364817766693041 };
static const double kFabricLockMuL[]  = { 0.9, 0.5, 0.1, -0.5, -0.9 };
static const double kFabricLockPhiL[] = { 0.0, 1.2, 2.9 };

void RunFabricReflectLock(
	const char* label,
	const IMaterial& fab,
	const FabricReflectLockRow* locked,
	const size_t nLocked,
	const bool bEmitCapture )
{
	IORStack iorStack( 1.0 );
	size_t idx = 0;
	int    mismatches = 0;
	int    belowHorizonNonZero = 0;

	for( size_t iv = 0; iv < sizeof( kFabricLockMuV ) / sizeof( kFabricLockMuV[0] ); ++iv )
	{
		const double muV = kFabricLockMuV[iv];
		const double sV  = std::sqrt( std::max( 0.0, 1.0 - muV * muV ) );
		const RayIntersectionGeometric ri = MakeIntersectionFromView( Vector3( sV, 0, muV ) );

		for( size_t il = 0; il < sizeof( kFabricLockMuL ) / sizeof( kFabricLockMuL[0] ); ++il )
		{
			const double muL = kFabricLockMuL[il];
			const double sL  = std::sqrt( std::max( 0.0, 1.0 - muL * muL ) );
			for( size_t ip = 0; ip < sizeof( kFabricLockPhiL ) / sizeof( kFabricLockPhiL[0] ); ++ip )
			{
				const double phi = kFabricLockPhiL[ip];
				const Vector3 l( sL * std::cos( phi ), sL * std::sin( phi ), muL );

				const double v0  = fab.GetBSDF()->value( l, ri )[0];
				const double vNM = fab.GetBSDF()->valueNM( l, ri, 550.0 );
				const double pdf = fab.GetSPF()->Pdf( ri, l, iorStack );

				if( muL < 0 && ( v0 != 0.0 || vNM != 0.0 || pdf != 0.0 ) ) {
					++belowHorizonNonZero;
				}

				if( bEmitCapture ) {
					std::printf( "\t{ %.17g, %.17g, %.17g, %.17g, %.17g, %.17g },\n",
					             muV, muL, phi, v0, vNM, pdf );
				} else if( idx < nLocked ) {
					const FabricReflectLockRow& L = locked[idx];
					const double got[3] = { v0, vNM, pdf };
					const double exp3[3] = { L.value0, L.valueNM, L.pdf };
					for( int k = 0; k < 3; ++k ) {
						const double denom = std::max( 1e-30, std::fabs( exp3[k] ) );
						if( std::fabs( got[k] - exp3[k] ) / denom > 1e-9 ) {
							++mismatches;
							std::printf( "    LOCK MISMATCH %s row %d field %d: got %.17g expected %.17g\n",
							             label, (int)idx, k, got[k], exp3[k] );
						}
					}
				}
				++idx;
			}
		}
	}

	if( bEmitCapture ) {
		return;
	}

	char buf[192];
	std::snprintf( buf, sizeof( buf ),
	               "reflection-only lock: %s -- row count matches the locked table", label );
	Check( idx == nLocked, buf );
	std::snprintf( buf, sizeof( buf ),
	               "reflection-only lock: %s -- every value/valueNM/Pdf matches the PRE-P1.1 "
	               "committed number to 1e-9 relative", label );
	Check( mismatches == 0, buf );
	std::snprintf( buf, sizeof( buf ),
	               "reflection-only lock: %s -- a below-horizon light is EXACTLY zero "
	               "(value, valueNM AND Pdf), because this substrate cannot transmit", label );
	Check( belowHorizonNonZero == 0, buf );
}

// Captured 2026-09-04 off the committed pre-P1.1 binary (HEAD 73d0c983)
// by running this file with `kFabricLockCapture = true`.  DO NOT retune
// these to make a change pass -- they are the definition of "the
// reflection-only path did not move".
static const FabricReflectLockRow kFabricLockLambertian[] = {
	{ 1, 0.90000000000000002, 0, 0.24021944635299111, 0.2402467648223503, 0.28647889756541162 },
	{ 1, 0.90000000000000002, 1.2, 0.24021944635299111, 0.2402467648223503, 0.28647889756541173 },
	{ 1, 0.90000000000000002, 2.8999999999999999, 0.24021944635299111, 0.2402467648223503, 0.28647889756541162 },
	{ 1, 0.5, 0, 0.22008781209757053, 0.22011085334578737, 0.15915494309189537 },
	{ 1, 0.5, 1.2, 0.22008781209757047, 0.22011085334578731, 0.15915494309189537 },
	{ 1, 0.5, 2.8999999999999999, 0.22008781209757047, 0.22011085334578731, 0.15915494309189535 },
	{ 1, 0.10000000000000001, 0, 0.15998498430331512, 0.15999711382879292, 0.031830988618379068 },
	{ 1, 0.10000000000000001, 1.2, 0.1599849843033152, 0.159997113828793, 0.031830988618379082 },
	{ 1, 0.10000000000000001, 2.8999999999999999, 0.15998498430331512, 0.15999711382879292, 0.031830988618379068 },
	{ 1, -0.5, 0, 0, 0, 0 },
	{ 1, -0.5, 1.2, 0, 0, 0 },
	{ 1, -0.5, 2.8999999999999999, 0, 0, 0 },
	{ 1, -0.90000000000000002, 0, 0, 0, 0 },
	{ 1, -0.90000000000000002, 1.2, 0, 0, 0 },
	{ 1, -0.90000000000000002, 2.8999999999999999, 0, 0, 0 },
	{ 0.70710678118654757, 0.90000000000000002, 0, 0.24911039588901623, 0.24913535666852837, 0.28647889756541162 },
	{ 0.70710678118654757, 0.90000000000000002, 1.2, 0.23854341668694792, 0.23856837746646009, 0.28647889756541173 },
	{ 0.70710678118654757, 0.90000000000000002, 2.8999999999999999, 0.21892221458820876, 0.2189471753677209, 0.28647889756541162 },
	{ 0.70710678118654757, 0.5, 0, 0.28162558817363337, 0.28164664087277486, 0.15915494309189537 },
	{ 0.70710678118654757, 0.5, 1.2, 0.25907157907748801, 0.25909263177662956, 0.15915494309189537 },
	{ 0.70710678118654757, 0.5, 2.8999999999999999, 0.18505799189119854, 0.18507904459034002, 0.15915494309189535 },
	{ 0.70710678118654757, 0.10000000000000001, 0, 0.2917965945436411, 0.29180767724403173, 0.031830988618379068 },
	{ 0.70710678118654757, 0.10000000000000001, 1.2, 0.26771161436117141, 0.2677226970615621, 0.031830988618379082 },
	{ 0.70710678118654757, 0.10000000000000001, 2.8999999999999999, 0.109735663336575, 0.10974674603696565, 0.031830988618379068 },
	{ 0.70710678118654757, -0.5, 0, 0, 0, 0 },
	{ 0.70710678118654757, -0.5, 1.2, 0, 0, 0 },
	{ 0.70710678118654757, -0.5, 2.8999999999999999, 0, 0, 0 },
	{ 0.70710678118654757, -0.90000000000000002, 0, 0, 0, 0 },
	{ 0.70710678118654757, -0.90000000000000002, 1.2, 0, 0, 0 },
	{ 0.70710678118654757, -0.90000000000000002, 2.8999999999999999, 0, 0, 0 },
	{ 0.17364817766693041, 0.90000000000000002, 0, 0.23254401388031021, 0.23255899210191505, 0.28647889756541162 },
	{ 0.17364817766693041, 0.90000000000000002, 1.2, 0.21318059018232918, 0.21319556840393405, 0.28647889756541173 },
	{ 0.17364817766693041, 0.90000000000000002, 2.8999999999999999, 0.14832836050487433, 0.14834333872647917, 0.28647889756541162 },
	{ 0.17364817766693041, 0.5, 0, 0.37940190576147759, 0.37941453886025434, 0.15915494309189537 },
	{ 0.17364817766693041, 0.5, 1.2, 0.35666751306392463, 0.35668014616270138, 0.15915494309189537 },
	{ 0.17364817766693041, 0.5, 2.8999999999999999, 0.12052057971342069, 0.12053321281219743, 0.15915494309189535 },
	{ 0.17364817766693041, 0.10000000000000001, 0, 0.82093637999749791, 0.82094303039647087, 0.031830988618379068 },
	{ 0.17364817766693041, 0.10000000000000001, 1.2, 0.80991101763273921, 0.80991766803171217, 0.031830988618379082 },
	{ 0.17364817766693041, 0.10000000000000001, 2.8999999999999999, 0.2527008060076103, 0.25270745640658321, 0.031830988618379068 },
	{ 0.17364817766693041, -0.5, 0, 0, 0, 0 },
	{ 0.17364817766693041, -0.5, 1.2, 0, 0, 0 },
	{ 0.17364817766693041, -0.5, 2.8999999999999999, 0, 0, 0 },
	{ 0.17364817766693041, -0.90000000000000002, 0, 0, 0, 0 },
	{ 0.17364817766693041, -0.90000000000000002, 1.2, 0, 0, 0 },
	{ 0.17364817766693041, -0.90000000000000002, 2.8999999999999999, 0, 0, 0 },
};
static const FabricReflectLockRow kFabricLockWeaveNone[] = {
	{ 1, 0.90000000000000002, 0, 0.20058142302250609, 0.084271780865640245, 0.26460649146109561 },
	{ 1, 0.90000000000000002, 1.2, 0.42287021313633538, 0.20718199087585465, 0.44866624181912279 },
	{ 1, 0.90000000000000002, 2.8999999999999999, 0.057694015415843562, 0.025784403976089738, 0.25652164985874637 },
	{ 1, 0.5, 0, 0.05347613811081902, 0.033377431291895646, 0.14574495818717656 },
	{ 1, 0.5, 1.2, 0.1628275141583212, 0.078144437458396929, 0.15670115060460274 },
	{ 1, 0.5, 2.8999999999999999, 0.052102763984241782, 0.032589713577046389, 0.14326268842760709 },
	{ 1, 0.10000000000000001, 0, 0.068649287101749154, 0.05987464281926546, 0.03402042688904347 },
	{ 1, 0.10000000000000001, 1.2, 0.065092861139260064, 0.058402056173172186, 0.028771384294276153 },
	{ 1, 0.10000000000000001, 2.8999999999999999, 0.055834026099828349, 0.054737494139072598, 0.0320978938879189 },
	{ 1, -0.5, 0, 0, 0, 0 },
	{ 1, -0.5, 1.2, 0, 0, 0 },
	{ 1, -0.5, 2.8999999999999999, 0, 0, 0 },
	{ 1, -0.90000000000000002, 0, 0, 0, 0 },
	{ 1, -0.90000000000000002, 1.2, 0, 0, 0 },
	{ 1, -0.90000000000000002, 2.8999999999999999, 0, 0, 0 },
	{ 0.70710678118654757, 0.90000000000000002, 0, 0.073882559019115585, 0.049785233041783013, 0.2628656555870974 },
	{ 0.70710678118654757, 0.90000000000000002, 1.2, 0.060633082294016288, 0.037001549708642473, 0.25407270529698239 },
	{ 0.70710678118654757, 0.90000000000000002, 2.8999999999999999, 0.38483723545508819, 0.16812671372073845, 0.31845846126218247 },
	{ 0.70710678118654757, 0.5, 0, 0.13215673419699286, 0.11250519162466863, 0.15297612401431851 },
	{ 0.70710678118654757, 0.5, 1.2, 0.10368691333258848, 0.086443180372481471, 0.13897991826189907 },
	{ 0.70710678118654757, 0.5, 2.8999999999999999, 0.12639994800406801, 0.051286620181832526, 0.14198614119987443 },
	{ 0.70710678118654757, 0.10000000000000001, 0, 0.21102873275425554, 0.20141741038253844, 0.040479640182373607 },
	{ 0.70710678118654757, 0.10000000000000001, 1.2, 0.17256092121622127, 0.17150834562186065, 0.027898411766998685 },
	{ 0.70710678118654757, 0.10000000000000001, 2.8999999999999999, 0.014495129924751875, 0.013533105264439174, 0.029860205972170435 },
	{ 0.70710678118654757, -0.5, 0, 0, 0, 0 },
	{ 0.70710678118654757, -0.5, 1.2, 0, 0, 0 },
	{ 0.70710678118654757, -0.5, 2.8999999999999999, 0, 0, 0 },
	{ 0.70710678118654757, -0.90000000000000002, 0, 0, 0, 0 },
	{ 0.70710678118654757, -0.90000000000000002, 1.2, 0, 0, 0 },
	{ 0.70710678118654757, -0.90000000000000002, 2.8999999999999999, 0, 0, 0 },
	{ 0.17364817766693041, 0.90000000000000002, 0, 0.12108612910135154, 0.10956756233649459, 0.23420486428562601 },
	{ 0.17364817766693041, 0.90000000000000002, 1.2, 0.099671944720272701, 0.089258809080487736, 0.21349581726773598 },
	{ 0.17364817766693041, 0.90000000000000002, 2.8999999999999999, 0.037632029845143494, 0.025566040150138667, 0.2142089789827962 },
	{ 0.17364817766693041, 0.5, 0, 0.29094335065915705, 0.27794797413052186, 0.15749165569592477 },
	{ 0.17364817766693041, 0.5, 1.2, 0.25598048774399668, 0.25010758815096551, 0.11405185346966858 },
	{ 0.17364817766693041, 0.5, 2.8999999999999999, 0.5497640686204327, 0.23954914861989321, 0.20547349830387998 },
	{ 0.17364817766693041, 0.10000000000000001, 0, 0.78191481160212661, 0.77051123381734477, 0.070981621153956398 },
	{ 0.17364817766693041, 0.10000000000000001, 1.2, 0.75317031948344015, 0.75232464634538632, 0.023581456820924107 },
	{ 0.17364817766693041, 0.10000000000000001, 2.8999999999999999, 0.25067443499269881, 0.22251782767231698, 0.33895812384832691 },
	{ 0.17364817766693041, -0.5, 0, 0, 0, 0 },
	{ 0.17364817766693041, -0.5, 1.2, 0, 0, 0 },
	{ 0.17364817766693041, -0.5, 2.8999999999999999, 0, 0, 0 },
	{ 0.17364817766693041, -0.90000000000000002, 0, 0, 0, 0 },
	{ 0.17364817766693041, -0.90000000000000002, 1.2, 0, 0, 0 },
	{ 0.17364817766693041, -0.90000000000000002, 2.8999999999999999, 0, 0, 0 },
};

//! Flip to `true`, rebuild and run to RE-CAPTURE the two tables above
//! (the output is paste-ready).  Left in the file on purpose: a future
//! deliberate model change needs a documented way to re-derive them.
static const bool kFabricLockCapture = false;

void TestReflectionOnlyUnchanged()
{
	std::cout << "ReflectionOnlyUnchanged (R8 P1.1 -- the pre-change value lock)" << std::endl;

	UniformColorPainter*  white = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); white->addref();
	UniformColorPainter*  grey  = new UniformColorPainter( RISEPel( 0.7, 0.7, 0.7 ) ); grey->addref();
	UniformScalarPainter* alph  = new UniformScalarPainter( kSheenAlpha ); alph->addref();
	UniformScalarPainter* zeroS = new UniformScalarPainter( 0.0 ); zeroS->addref();

	{
		LambertianMaterial* lamb = new LambertianMaterial( *grey ); lamb->addref();
		FabricMaterial* fab = new FabricMaterial( *lamb, *white, *alph, *zeroS ); fab->addref();
		if( kFabricLockCapture ) {
			std::printf( "  --- capture: kFabricLockLambertian ---\n" );
		}
		RunFabricReflectLock( "fabric / lambertian", *fab,
		                      kFabricLockLambertian,
		                      sizeof( kFabricLockLambertian ) / sizeof( kFabricLockLambertian[0] ),
		                      kFabricLockCapture );
		safe_release( fab );
		safe_release( lamb );
	}

	{
		// The SAME satin the P2-B thin rows use, with `transmission`
		// left at `none`: this is the substrate class P1.1 actually
		// touches, so it is the one whose non-thin behaviour has to be
		// proven untouched.
		RISE::WeaveTest::PresetWeave satin( "satin" );
		FabricMaterial* fab = new FabricMaterial( *satin.Material(), *white, *alph, *zeroS );
		fab->addref();
		if( kFabricLockCapture ) {
			std::printf( "  --- capture: kFabricLockWeaveNone ---\n" );
		}
		RunFabricReflectLock( "fabric / weave satin (transmission none)", *fab,
		                      kFabricLockWeaveNone,
		                      sizeof( kFabricLockWeaveNone ) / sizeof( kFabricLockWeaveNone[0] ),
		                      kFabricLockCapture );
		safe_release( fab );
	}

	safe_release( zeroS ); safe_release( alph );
	safe_release( grey ); safe_release( white );
}

//////////////////////////////////////////////////////////////////////
// 8c. THE SUBSTRATE'S TRANSMISSION SURVIVES THE WRAPPER
//     (R8 P1.1, docs/CLOTH_FABRIC_DESIGN.md 15 debt 22).
//
//  THE DEFECT THIS PINS.  `fabric_material` admits `weave_material` as a
//  substrate, and since P2-B a weave under `transmission thin` carries
//  two below-horizon lobes.  The wrapper reported neither flag and
//  returned 0 for every opposite-hemisphere pair, so a sheen layer over
//  a sheer curtain made it 100 % OPAQUE -- with no diagnostic anywhere.
//
//  THREE THINGS ARE CHECKED, and all three are needed:
//    (a) the FLAGS forward (`ScattersFullSphere` / `CouldLightPassThrough`),
//        which is what turns on `LightSampler`'s full-sphere NEE and
//        `AutoRasterizer`'s transmissive-material signal;
//    (b) `value()` is actually NON-ZERO below the horizon -- claiming
//        (a) while still returning 0 there would be strictly WORSE than
//        the bug, because NEE would then spend shadow rays on a
//        direction that contributes nothing;
//    (c) the wrapped value equals the bare weave's times the CLOSED-FORM
//        Kulla-Conty factor, re-derived here from `SheenDirectionalAlbedo`
//        (via this file's own `LocalBaseScaling`, which shares no code
//        with `FabricBRDF::BaseScaling`) -- so a wrapper that transmitted
//        SOMETHING but with the wrong law still fails.
//
//  Plus the negative controls: over a `transmission none` weave and over
//  a Lambertian, both flags stay false and below-horizon stays exactly 0.
//////////////////////////////////////////////////////////////////////

void TestTransmissiveSubstrateForwarding()
{
	std::cout << "TransmissiveSubstrateForwarding (R8 P1.1 / debt 22)" << std::endl;

	UniformColorPainter*  white = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); white->addref();
	UniformColorPainter*  grey  = new UniformColorPainter( RISEPel( 0.7, 0.7, 0.7 ) ); grey->addref();
	UniformScalarPainter* alph  = new UniformScalarPainter( kSheenAlpha ); alph->addref();
	UniformScalarPainter* zeroS = new UniformScalarPainter( 0.0 ); zeroS->addref();

	// The showcase configuration: sheer white linen, gap 0.2, transmit
	// 0.25 on both families -- the same numbers LayeredWhiteFurnaceTest's
	// P2-B rows use, so the two suites talk about one material.
	RISE::WeaveTest::PresetWeave thin( "linen", 0.0, 0.5, /*whiteDyes=*/true,
	                                   /*thin=*/true, 0.25, 0.25, /*gapOverride=*/0.2 );
	RISE::WeaveTest::PresetWeave opaque( "linen", 0.0, 0.5, /*whiteDyes=*/true,
	                                     /*thin=*/false, -1, -1, /*gapOverride=*/0.2 );
	LambertianMaterial* lamb = new LambertianMaterial( *grey ); lamb->addref();

	FabricMaterial* fabThin = new FabricMaterial( *thin.Material(), *white, *alph, *zeroS );
	fabThin->addref();
	FabricMaterial* fabOpaque = new FabricMaterial( *opaque.Material(), *white, *alph, *zeroS );
	fabOpaque->addref();
	FabricMaterial* fabLamb = new FabricMaterial( *lamb, *white, *alph, *zeroS );
	fabLamb->addref();

	// (a) the flags.
	Check( thin.Material()->ScattersFullSphere() && thin.Material()->CouldLightPassThrough(),
	       "premise: the BARE `transmission thin` weave reports both full-sphere flags" );
	Check( fabThin->ScattersFullSphere(),
	       "fabric over a `transmission thin` weave FORWARDS ScattersFullSphere()" );
	Check( fabThin->CouldLightPassThrough(),
	       "fabric over a `transmission thin` weave FORWARDS CouldLightPassThrough()" );
	Check( !fabOpaque->ScattersFullSphere() && !fabOpaque->CouldLightPassThrough(),
	       "fabric over a `transmission none` weave reports NEITHER flag (Phase-1 behaviour intact)" );
	Check( !fabLamb->ScattersFullSphere() && !fabLamb->CouldLightPassThrough(),
	       "fabric over a Lambertian reports NEITHER flag" );

	// (b) + (c): the below-horizon response, against the closed form.
	//
	// `m` is 1 (an authored-white sheen) and `alpha` is kSheenAlpha,
	// which is above the 0.04 floor, so the resolved parameters are known
	// exactly here without asking the material for them.
	static const double kMuV[] = { 1.0, 0.7071067811865476, 0.3420201433256687 };
	static const double kMuL[] = { -0.25, -0.6, -0.95 };
	const double relTol = 1e-9;

	int    probes = 0, positives = 0, lawFailures = 0, opaqueLeaks = 0;
	double worstRel = 0.0;

	for( size_t iv = 0; iv < sizeof( kMuV ) / sizeof( kMuV[0] ); ++iv )
	{
		const double muV = kMuV[iv];
		const double sV  = std::sqrt( std::max( 0.0, 1.0 - muV * muV ) );
		const RayIntersectionGeometric ri = MakeIntersectionFromView( Vector3( sV, 0, muV ) );

		for( size_t il = 0; il < sizeof( kMuL ) / sizeof( kMuL[0] ); ++il )
		{
			const double muL = kMuL[il];
			const double sL  = std::sqrt( std::max( 0.0, 1.0 - muL * muL ) );
			const Vector3 l( sL * 0.6, sL * 0.8, muL );		// an out-of-plane azimuth

			const double bare    = thin.BSDF()->value( l, ri )[0];
			const double wrapped = fabThin->GetBSDF()->value( l, ri )[0];
			// Re-derived from the baked table, NOT FabricBRDF's helper --
			// same discipline gate 5b's quadrature follows.
			const double expected = bare * LocalBaseScaling( kSheenAlpha, 1.0, muV, -muL );

			++probes;
			if( bare > 0 && wrapped > 0 ) ++positives;
			const double denom = std::max( 1e-30, std::fabs( expected ) );
			const double rel   = std::fabs( wrapped - expected ) / denom;
			if( rel > worstRel ) worstRel = rel;
			if( rel > relTol ) ++lawFailures;

			// Negative controls at the SAME pair.
			if( fabOpaque->GetBSDF()->value( l, ri )[0] != 0.0 ) ++opaqueLeaks;
			if( fabLamb->GetBSDF()->value( l, ri )[0] != 0.0 )   ++opaqueLeaks;
		}
	}

	std::printf( "    below-horizon probes = %d, non-zero on BOTH bare and wrapped = %d, "
	             "worst |wrapped - bare*scale| / expected = %.3g\n",
	             probes, positives, worstRel );

	Check( positives == probes,
	       "MONEY: `fabric_material` over a `transmission thin` weave is NON-ZERO below the "
	       "horizon at every probed pair -- the silent extinction (debt 22) is gone" );
	Check( lawFailures == 0,
	       "the transmitted value is the substrate's times the CLOSED-FORM two-arm Kulla-Conty "
	       "scale (1-m*Ehat(|n.v|))(1-m*Ehat(|n.l|))/(1-m*Ebar), re-derived independently" );
	Check( opaqueLeaks == 0,
	       "negative control: fabric over a `transmission none` weave AND over a Lambertian both "
	       "stay EXACTLY zero below the horizon" );

	safe_release( fabLamb );
	safe_release( fabOpaque );
	safe_release( fabThin );
	safe_release( lamb );
	safe_release( zeroS ); safe_release( alph );
	safe_release( grey ); safe_release( white );
}

//////////////////////////////////////////////////////////////////////
// 9. GATE 5b -- hemisphericalAlbedo's substrate-coupling error,
//    MEASURED against a brute-force double quadrature of the REAL
//    fabric value().
//
//    HemiAlbedo = (1/pi) * INT_v [ INT_l f(l,v) (n.l) dl ] (n.v) dv
//
//    Both integrals are midpoint rules in (mu, phi).  The OUTER azimuth
//    integrates out to a bare 2*pi exactly, because every substrate
//    used here is isotropic about the normal and the sheen lobe is
//    isotropic by construction -- so the outer loop fixes phi_v = 0 and
//    multiplies by 2*pi rather than sampling it.  That is an EXACT
//    reduction, not an approximation, and it is what makes a 5-digit
//    quadrature affordable in a unit test.
//
//    Self-check: for a Lambertian substrate route 1 is EXACT, so that
//    row doubles as the harness's own control -- a failure there
//    indicts the quadrature, not the model.
//////////////////////////////////////////////////////////////////////

//! `1 - m*Ehat(alpha, cosTheta)`, Ehat = min(E, 1) -- re-derived from
//! the baked table so this test's oracle shares no code with the
//! material under test.  See BruteForceIntegrals' call site.
double LocalSheenTransmit( double alpha, double m, double cosTheta )
{
	const double eHat = std::min( 1.0, (double)SheenDirectionalAlbedo::E( alpha, cosTheta ) );
	return 1.0 - std::min( 1.0, m * eHat );
}

//! `1 - m*EhatMean(alpha)`.
double LocalSheenTransmitMean( double alpha, double m )
{
	return 1.0 - std::min( 1.0, m * (double)SheenDirectionalAlbedo::EHatMean( alpha ) );
}

//! The full Kulla-Conty base scaling, re-derived.
double LocalBaseScaling( double alpha, double m, double cosV, double cosL )
{
	const double denom = std::max( 1e-6, LocalSheenTransmitMean( alpha, m ) );
	return LocalSheenTransmit( alpha, m, cosV )
	     * LocalSheenTransmit( alpha, m, cosL ) / denom;
}

//! Intersection whose VIEW direction (toward the viewer) is `view`.
//! `vGeomNormal` is left zero so the horizon gate degenerates to the
//! shading-hemisphere test (SPFBSDFConsistencyTest's fixture).
RayIntersectionGeometric MakeIntersectionFromView( const Vector3& view )
{
	const Vector3 inDir = -view;
	Ray inRay( Point3( view.x, view.y, view.z ), inDir );
	RasterizerState rs = { 0, 0 };
	RayIntersectionGeometric ri( inRay, rs );
	ri.bHit = true;
	ri.range = 1.0;
	ri.ptIntersection = Point3( 0, 0, 0 );
	ri.vNormal = Vector3( 0, 0, 1 );
	ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
	ri.ptCoord = Point2( 0.5, 0.5 );
	return ri;
}

//! One pass over the (v, l) grid, accumulating the THREE integrals the
//! decomposition below needs.  Sharing the grid is not an optimisation:
//! it makes `S_true = scaled / base` exact on this quadrature, because
//! the discretisation error cancels in the ratio.
struct BruteForceSums
{
	double fabric;		//!< (1/pi) INT INT f_fabric(l,v) (n.l)(n.v)   -- the whole material
	double base;		//!< (1/pi) INT INT f_base(l,v)   (n.l)(n.v)   -- the bare substrate's TRUE bihemispherical albedo
	double scaled;		//!< (1/pi) INT INT f_base(l,v) * scale(l,v) (n.l)(n.v) -- the joint integral route 1 factorises
};

BruteForceSums BruteForceIntegrals(
	const IBSDF& fabricBRDF,
	const IBSDF& baseBRDF,
	const double alpha,				// the CLAMPED sheen alpha the fabric actually uses
	const double m,					// max3(sheenColor)
	const int nMu,
	const int nPhi )
{
	double accF = 0, accB = 0, accS = 0;

	for( int iv = 0; iv < nMu; ++iv )
	{
		const double muV = ( iv + 0.5 ) / (double)nMu;
		const double sV  = std::sqrt( std::max( 0.0, 1.0 - muV * muV ) );
		const RayIntersectionGeometric ri = MakeIntersectionFromView( Vector3( sV, 0, muV ) );

		double innF = 0, innB = 0, innS = 0;
		for( int il = 0; il < nMu; ++il )
		{
			const double muL = ( il + 0.5 ) / (double)nMu;
			const double sL  = std::sqrt( std::max( 0.0, 1.0 - muL * muL ) );
			// RE-DERIVED HERE from the baked table, deliberately NOT
			// FabricBRDF::BaseScaling.  Calling the material's own
			// helper would put the same code on both sides of the
			// comparison, so a self-consistent error inside it could not
			// show up (M3 review, 2026-09-02).  The only surface still
			// shared with the model is SheenDirectionalAlbedo itself,
			// which has its own brute-force test against CharlieSheen.h.
			const double kern = LocalBaseScaling( alpha, m, muV, muL );

			for( int ip = 0; ip < nPhi; ++ip )
			{
				const double phi = 2.0 * PI * ( ip + 0.5 ) / (double)nPhi;
				const Vector3 l( sL * std::cos( phi ), sL * std::sin( phi ), muL );

				const double f = fabricBRDF.value( l, ri )[0];
				const double b = baseBRDF.value( l, ri )[0];
				innF += f * muL;
				innB += b * muL;
				innS += b * kern * muL;
			}
		}
		const double wIn = ( 1.0 / nMu ) * ( 2.0 * PI / nPhi );
		accF += innF * wIn * muV;
		accB += innB * wIn * muV;
		accS += innS * wIn * muV;
	}

	const double wOut = ( 1.0 / nMu ) * ( 2.0 * PI ) / PI;
	BruteForceSums out;
	out.fabric = accF * wOut;
	out.base   = accB * wOut;
	out.scaled = accS * wOut;
	return out;
}

void TestHemisphericalAlbedoError()
{
	std::cout << "HemisphericalAlbedoError (gate 5b; PRE-COMMITTED tolerance "
	          << ( kHemiAlbedoTol * 100.0 ) << " %)" << std::endl;

	// Grey substrates: a single reflectance so channel 0 carries the
	// whole story and the quadrature stays cheap.
	UniformColorPainter* grey  = new UniformColorPainter( RISEPel( 0.7, 0.7, 0.7 ) ); grey->addref();
	UniformColorPainter* black = new UniformColorPainter( RISEPel( 0.0, 0.0, 0.0 ) ); black->addref();
	UniformColorPainter* f0    = new UniformColorPainter( RISEPel( 0.04, 0.04, 0.04 ) ); f0->addref();
	UniformColorPainter* white = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); white->addref();
	UniformColorPainter* half  = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) ); half->addref();
	UniformScalarPainter* zeroSc = new UniformScalarPainter( 0.0 ); zeroSc->addref();
	UniformScalarPainter* alphaSheen = new UniformScalarPainter( kSheenAlpha ); alphaSheen->addref();
	UniformScalarPainter* iorSc = new UniformScalarPainter( 1.5 ); iorSc->addref();

	// 64 x 128 inner, 64 outer.  Calibrated against the Lambertian
	// control row, where route 1 is exact in closed form.
	static const int kNMu  = 64;
	static const int kNPhi = 128;

	//------------------------------------------------------------------
	// (i) The SHEEN half of route 1, on its own.
	//
	// Over a BLACK substrate the base term vanishes identically, so
	// route 1 reduces to `sheenColor * EHatMean(alpha)` and the brute force
	// reduces to the bare Charlie lobe's bihemispherical albedo.  This
	// isolates the EHatMean bake from every substrate question below --
	// if it were off, EVERY row after it would be off by the same
	// amount and the decomposition would misattribute it.
	//------------------------------------------------------------------
	{
		LambertianMaterial* blackBase = new LambertianMaterial( *black ); blackBase->addref();
		FabricMaterial* sheenOnly = new FabricMaterial( *blackBase, *white, *alphaSheen, *zeroSc );
		sheenOnly->addref();

		RISEPel routed;
		const bool got = sheenOnly->GetBSDF()->hemisphericalAlbedo( MakeProbe(), routed );
		const BruteForceSums bf = BruteForceIntegrals(
			*sheenOnly->GetBSDF(), *blackBase->GetBSDF(), kSheenAlpha, 1.0, kNMu, kNPhi );
		const double rel = ( bf.fabric > 1e-9 ) ? std::fabs( routed[0] - bf.fabric ) / bf.fabric : 0.0;

		std::printf( "    [sheen half only, black substrate]  EHatMean(%.2f) = %.6f  brute = %.6f  rel = %+.3f %%\n",
		             kSheenAlpha, routed[0], bf.fabric, rel * 100.0 );
		Check( got, "hemisphericalAlbedo answered for the sheen-only fabric" );
		Check( rel <= kHemiAlbedoTol,
		       "gate 5b (i): the SHEEN half of route 1 (sheenColor * EHatMean) matches a "
		       "brute-force integral of the bare Charlie lobe" );

		safe_release( sheenOnly );
		safe_release( blackBase );
	}

	//------------------------------------------------------------------
	// (ii) The BASE half: the factorisation error, which is what gate
	//      5b's own remedy could ever have addressed.
	//
	// ROUND 5 CHANGED WHAT THIS MEASURES.  Under round 4's `min` kernel
	// the base factor came out of a baked 2-D table `S(alpha, m)`, and
	// this check compared that table against the true double integral;
	// the doc's remedy for an exceedance was a per-substrate-class 3-D
	// table.  The product form FACTORS, so the table is GONE and the
	// factor is the closed form `1 - m*Ebar(alpha)`.  For a LAMBERTIAN
	// substrate that is now EXACT BY CONSTRUCTION -- not "exact up to a
	// bake error" -- so the Lambertian row below should read 0.000 %,
	// and anything else indicts the quadrature rather than the model.
	//
	// DECOMPOSITION -- and why the end-to-end number is reported but not
	// asserted.  Route 1's fabric-level error still has TWO independent
	// sources, and only one of them is ours:
	//
	//   (a) THE FACTORISATION, `R * (1 - m*Ebar)` in place of the joint
	//       INT INT f_base * scale(l,v).  Pulling the substrate's albedo
	//       out treats `f_base` and the kernel as uncorrelated, which
	//       needs `f_base` CONSTANT in the integration variables --
	//       exact for Lambertian, an approximation for Oren-Nayar
	//       (coupled through max/min(theta_l, theta_v) and the azimuthal
	//       difference) and for GGX (coupled through the half-vector).
	//       Measured below as |factor_closed - factor_true| /
	//       factor_true, with factor_true = scaled/base on the SAME
	//       grid, so quadrature error cancels in the ratio.  THIS IS THE
	//       ASSERTED QUANTITY.
	//
	//   (b) THE SUBSTRATE'S OWN `hemisphericalAlbedo`, one layer down.
	//       `OrenNayarBRDF::hemisphericalAlbedo` returns `Rd` verbatim --
	//       documented in OrenNayarBRDF.cpp:148-190 as measured 12.6 %
	//       high at roughness 0.5 and 25.6 % high at roughness 1 -- and
	//       GGX's is likewise an estimate that runs high.  NOTHING in
	//       fabric_material can fix that: it is the substrate
	//       misreporting its own reflectance, and coated_material's
	//       recycling denominator already inherits the identical debt.
	//
	// So both are printed, and the fabric-level assertion is made
	// against route 1 evaluated with the substrate's TRUE (brute-forced)
	// albedo -- i.e. (a) alone, restated at the fabric level.  The raw
	// end-to-end error is printed beside it, unasserted, together with
	// the substrate's own error, so the arithmetic is visible rather
	// than hidden.
	//------------------------------------------------------------------

	struct SubstrateRow { const char* name; IMaterial* mat; };
	std::vector<SubstrateRow> subs;

	LambertianMaterial* lamb = new LambertianMaterial( *grey ); lamb->addref();
	subs.push_back( { "Lambertian (route 1 EXACT by construction)", lamb } );

	UniformScalarPainter* on03 = new UniformScalarPainter( 0.3 ); on03->addref();
	UniformScalarPainter* on06 = new UniformScalarPainter( 0.6 ); on06->addref();
	OrenNayarMaterial* on3 = new OrenNayarMaterial( *grey, *on03 ); on3->addref();
	OrenNayarMaterial* on6 = new OrenNayarMaterial( *grey, *on06 ); on6->addref();
	subs.push_back( { "OrenNayar sigma=0.3", on3 } );
	subs.push_back( { "OrenNayar sigma=0.6", on6 } );

	UniformScalarPainter* gx02 = new UniformScalarPainter( 0.2 ); gx02->addref();
	UniformScalarPainter* gx05 = new UniformScalarPainter( 0.5 ); gx05->addref();
	GGXMaterial* ggx2 = new GGXMaterial( *grey, *f0, *gx02, *gx02, *iorSc, *zeroSc, eFresnelSchlickF0 ); ggx2->addref();
	GGXMaterial* ggx5 = new GGXMaterial( *grey, *f0, *gx05, *gx05, *iorSc, *zeroSc, eFresnelSchlickF0 ); ggx5->addref();
	subs.push_back( { "GGX alpha=0.2", ggx2 } );
	subs.push_back( { "GGX alpha=0.5", ggx5 } );

	// m = max3(sheenColor): 0.5 and 1.0, the two the gate names.
	struct MRow { const char* name; IPainter* pnt; double m; };
	const MRow ms[] = { { "m=0.5", half, 0.5 }, { "m=1.0", white, 1.0 } };

	std::printf( "    %-46s %-6s   %-9s %-9s | %-9s %-9s %-9s\n",
	             "substrate", "m", "1-m*Ebar", "true",
	             "factor%", "substr%", "endend%" );

	double worstFactor = 0;
	double worstEndToEnd = 0;

	for( const SubstrateRow& s : subs )
	{
		for( const MRow& mr : ms )
		{
			FabricMaterial* fab = new FabricMaterial( *s.mat, *mr.pnt, *alphaSheen, *zeroSc );
			fab->addref();

			RISEPel routed;
			const bool got = fab->GetBSDF()->hemisphericalAlbedo( MakeProbe(), routed );

			RISEPel subReported;
			const bool gotSub = s.mat->GetBSDF()->hemisphericalAlbedo( MakeProbe(), subReported );

			const BruteForceSums bf = BruteForceIntegrals(
				*fab->GetBSDF(), *s.mat->GetBSDF(), kSheenAlpha, mr.m, kNMu, kNPhi );

			// The closed-form base factor route 1 uses, and the true one
			// this quadrature measures.
			const double sClosed = LocalSheenTransmitMean( kSheenAlpha, mr.m );
			const double sTrue   = ( bf.base > 1e-12 ) ? ( bf.scaled / bf.base ) : 0.0;
			const double sheenTerm = mr.m * SheenDirectionalAlbedo::EHatMean( kSheenAlpha );

			// (a) the factorisation error, isolated.
			const double errFactor = ( sTrue > 1e-12 ) ? std::fabs( sClosed - sTrue ) / sTrue : 0.0;
			// (a) restated at the fabric level: route 1 with an EXACT substrate albedo.
			const double route1True = bf.base * sClosed + sheenTerm;
			const double errRoute1True = ( bf.fabric > 1e-9 )
				? std::fabs( route1True - bf.fabric ) / bf.fabric : 0.0;
			// (b) the substrate's own hemisphericalAlbedo error -- pre-existing, reported.
			const double errSubstrate = ( bf.base > 1e-12 )
				? std::fabs( subReported[0] - bf.base ) / bf.base : 0.0;
			// The raw end-to-end number, reported.
			const double errEndToEnd = ( bf.fabric > 1e-9 )
				? std::fabs( routed[0] - bf.fabric ) / bf.fabric : 0.0;

			std::printf( "    %-46s %-6s   %9.6f %9.6f | %+8.3f %+8.3f %+8.3f\n",
			             s.name, mr.name, sClosed, sTrue,
			             errFactor * 100.0, errSubstrate * 100.0, errEndToEnd * 100.0 );

			Check( got,    std::string( "hemisphericalAlbedo answered for " ) + s.name );
			Check( gotSub, std::string( "the bare substrate answers too: " ) + s.name );
			Check( errFactor <= kHemiAlbedoTol,
			       std::string( "gate 5b (ii): FACTORISATION error for " ) + s.name + " " + mr.name
			       + " within the pre-committed tolerance" );
			Check( errRoute1True <= kHemiAlbedoTol,
			       std::string( "gate 5b (ii): route 1 with an EXACT substrate albedo, " )
			       + s.name + " " + mr.name + ", within the pre-committed tolerance" );

			if( errFactor   > worstFactor )   worstFactor   = errFactor;
			if( errEndToEnd > worstEndToEnd ) worstEndToEnd = errEndToEnd;

			safe_release( fab );
		}
	}

	std::printf( "    WORST factorisation error (ASSERTED, pre-committed %.1f %%): %.3f %%\n",
	             kHemiAlbedoTol * 100.0, worstFactor * 100.0 );
	std::printf( "    WORST end-to-end error (REPORTED; inherits the substrate's own\n"
	             "      hemisphericalAlbedo approximation, which a 3D S table cannot fix): %.3f %%\n",
	             worstEndToEnd * 100.0 );

	safe_release( ggx5 ); safe_release( ggx2 );
	safe_release( gx05 ); safe_release( gx02 );
	safe_release( on6 ); safe_release( on3 );
	safe_release( on06 ); safe_release( on03 );
	safe_release( lamb );
	safe_release( iorSc ); safe_release( alphaSheen ); safe_release( zeroSc );
	safe_release( half ); safe_release( white ); safe_release( f0 );
	safe_release( black ); safe_release( grey );
}

//////////////////////////////////////////////////////////////////////
// 10. GATE 8 -- spectral parity at an authored-white dye
//////////////////////////////////////////////////////////////////////

void TestSpectralParityAtWhiteDye()
{
	std::cout << "SpectralParityAtWhiteDye (gate 8)" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "white", "1 1 1" )
	                 + ColorPainter( "amber", "0.92 0.55 0.18" )
	                 // A WHITE Lambertian base, so the RGB response is
	                 // achromatic and its channel 0 is directly
	                 // comparable to the single-wavelength answer.
	                 + LambertianMat( "base", "white" )
	                 + FabricMat( "whitedye", "base", "custom", "white" )
	                 + FabricMat( "tinted",   "base", "custom", "amber" );
	ParseBodyInto( "specparity", body, *job );

	// One ulp of `x`, so the tolerance below is expressed in the unit the
	// discrepancy actually lives in rather than as a magic relative epsilon.
	auto UlpsApart = []( double a, double b ) -> double {
		const double m = std::max( std::fabs( a ), std::fabs( b ) );
		if( m == 0.0 ) return 0.0;
		const double ulp = std::nextafter( m, 1e308 ) - m;
		return ( ulp > 0.0 ) ? std::fabs( a - b ) / ulp : 0.0;
	};

	// ---- (i) THE GUARD ITSELF, asserted directly and EXACTLY.
	//
	// This is what gate 8 is actually about: an authored-white
	// multiplicative slot must be an exact no-op at every wavelength, so
	// `GuardedGetColorNM` must return LITERALLY 1.0 and not the
	// 1 - epsilon the Jakob-Hanika sigmoid reaches asymptotically.
	// Asserting it on the painter -- rather than inferring it from a
	// material response -- is what keeps this half under an exact oracle
	// regardless of anything the material does downstream.
	//
	// AN EARLIER REVISION TRIED TO ISOLATE THE TWO TERMS BY BINDING A
	// BLACK PAINTER TO THE OTHER ONE.  That does not work, and the way
	// it fails is worth recording: a black painter is NOT spectrally
	// black.  `GetColor` returns exactly (0,0,0), but `GetColorNM` runs
	// the JH uplift and comes back near-but-not-exactly zero, so a
	// "black dye" still contributes a tiny sheen term on the NM pipe
	// while contributing exactly nothing on the RGB one.  Both isolation
	// fixtures were therefore contaminated by the same effect they were
	// meant to exclude.  (The guard does not fire for black -- it is a
	// white-corner guard -- so this is the painter's own behaviour, not
	// a fabric defect, and it is out of scope here.)
	{
		IPainter* whitePnt = job->GetPainters()->GetItem( "white" );
		Check( whitePnt != 0, "gate 8: the white painter resolved" );
		if( whitePnt )
		{
			RayIntersectionGeometric ri = MakeProbe();
			const RISEPel rgbW = whitePnt->GetColor( ri );
			Check( rgbW[0] == 1.0 && rgbW[1] == 1.0 && rgbW[2] == 1.0,
			       "gate 8: the authored-white painter is exactly 1.0 on the RGB pipe" );

			bool guardExact = true;
			double worstRaw = 1.0;
			for( double nm = 380.0; nm <= 780.0 + 1e-9; nm += 20.0 ) {
				if( GuardedGetColorNM( *whitePnt, ri, nm ) != 1.0 ) guardExact = false;
				worstRaw = std::min( worstRaw, whitePnt->GetColorNM( ri, nm ) );
			}
			std::printf( "    guard: GuardedGetColorNM(white) == 1.0 exactly at every nm: %s "
			             "(raw unguarded uplift dips to %.9f)\n",
			             guardExact ? "YES" : "NO", worstRaw );
			Check( guardExact,
			       "gate 8: GuardedGetColorNM on an authored-white slot returns EXACTLY 1.0 "
			       "at every sampled wavelength -- the guard property, under an exact oracle" );
			// The guard must be doing real work, i.e. the raw uplift must
			// NOT already be exactly 1 -- otherwise the check above would
			// pass with the guard deleted.
			Check( worstRaw < 1.0,
			       "gate 8 (red-proof): the RAW uplift of white is < 1 somewhere, so the guard "
			       "is load-bearing rather than decorative" );
		}
	}

	// ---- Both terms together.
	//
	// TOLERANCE: 4 ulp, NOT bit-equality, and the reason is established
	// rather than assumed.  Check (i) proves that EVERY INPUT to the two
	// expressions is bit-identical: the dye's guarded NM sample is
	// exactly 1.0 and its RGB channels are exactly 1.0, the substrate's
	// white reflectance likewise, and alpha / m / the weave angle are
	// read from uniform painters that answer the same number on both
	// pipes.  So `value` and `valueNM` are evaluating the SAME algebra on
	// the SAME numbers, and any residual is the floating-point
	// EVALUATION differing -- the RGB path runs through RISEPel's
	// operator* / operator+ chain while the NM path is plain doubles,
	// and under this build's `-ffast-math` the two are free to contract
	// and reassociate differently.  Measured at 1.0 ulp (1.613e-16
	// relative at rho ~ 0.344).
	//
	// A ROUNDING-ORDER DIFFERENCE IS NOT A GUARD FAILURE.  Asserting
	// bit-equality here would make the check hostage to the optimiser's
	// contraction choices and would fail for a reason that has nothing
	// to do with the Jakob-Hanika white corner.  Check (i) keeps the
	// real property under an exact oracle; this one bounds the
	// end-to-end composition.
	//
	const double rgb = RespondCh0( *job, "whitedye" );
	Check( rgb > 0, "white-dye fabric responds on the RGB pipe" );

	double worstUlps = 0;
	double worstRel  = 0;
	for( double nm = 380.0; nm <= 780.0 + 1e-9; nm += 20.0 )
	{
		const double v = RespondNM( *job, "whitedye", nm );
		worstUlps = std::max( worstUlps, UlpsApart( v, rgb ) );
		worstRel  = std::max( worstRel, std::fabs( v - rgb ) / ( rgb > 0 ? rgb : 1.0 ) );
	}
	std::printf( "    white dye (both terms): RGB ch0 = %.17g, worst NM-vs-RGB over 380-780 nm "
	             "= %.1f ulp (%.3e relative)\n", rgb, worstUlps, worstRel );
	Check( worstUlps <= 4.0,
	       "gate 8: an authored-WHITE dye agrees between the RGB and NM paths to <= 4 ulp "
	       "at every sampled wavelength (each TERM is bit-exact; the residual is the "
	       "final addition's FP contraction, not the guard)" );

	// ---- (iv) DOES a spectral sample actually exceed its RGB max3?
	//
	// `ValueNMWithParams` caps `tintNM` at `m = max3(RGB)` so a single
	// wavelength cannot claim more of the sheen lobe than the achromatic
	// budget the base was suppressed by.  Whether that cap ever BINDS is
	// an empirical question about the Jakob-Hanika uplift, not something
	// to assume either way -- so measure it and print the answer rather
	// than leaving the next reader to wonder whether the clamp is dead
	// code.
	{
		IPainter* amberPnt = job->GetPainters()->GetItem( "amber" );
		if( amberPnt ) {
			RayIntersectionGeometric ri = MakeProbe();
			const RISEPel rgbA = amberPnt->GetColor( ri );
			const double m3 = ColorMath::MaxValue( rgbA );
			double worstRatio = 0.0, atNM = 0.0;
			for( double nm = 380.0; nm <= 780.0 + 1e-9; nm += 5.0 ) {
				const double r = amberPnt->GetColorNM( ri, nm ) / ( m3 > 0 ? m3 : 1.0 );
				if( r > worstRatio ) { worstRatio = r; atNM = nm; }
			}
			std::printf( "    saturated dye (0.92,0.55,0.18): max3 = %.4f, worst "
			             "GetColorNM/max3 = %.4f at %.0f nm -> the tintNM cap %s\n",
			             m3, worstRatio, atNM,
			             ( worstRatio > 1.0 ) ? "BINDS (the hazard is real)"
			                                  : "never binds for this painter" );
		}
	}

	// The other half: a genuinely tinted dye must NOT be flat, or the
	// check above would also pass with the tint term dead.
	const double t550 = RespondNM( *job, "tinted", 550.0 );
	const double t660 = RespondNM( *job, "tinted", 660.0 );
	Check( t550 >= 0 && t660 >= 0 && t550 != t660,
	       "gate 8 (negative control): a SATURATED dye is wavelength-dependent" );

	safe_release( job );
}

} // anonymous namespace

int main()
{
	GlobalLog();

	std::cout << "===== fabric_material chunk contract test =====" << std::endl;

	TestPresetsSeedTheSlots();
	TestExplicitSlotsWin();
	TestAllowlistAccepts();
	TestAllowlistTextNamesWeave();
	TestAllowlistRefusals();
	TestPresetSubstrateMismatchWarns();
	TestSheenColorDefault();
	TestWeaveRotationReachesTheSubstrate();
	TestRoughnessFloor();
	TestRequireSingleOnScalarSlots();
	TestEditorIntrospection();
	TestApiLevelAllowlist();
	TestReflectionOnlyUnchanged();
	TestTransmissiveSubstrateForwarding();
	TestHemisphericalAlbedoError();
	TestSpectralParityAtWhiteDye();

	std::cout << std::endl
	          << "Results: " << passCount << " passed, "
	          << failCount << " failed" << std::endl;
	return ( failCount > 0 ) ? 1 : 0;
}
