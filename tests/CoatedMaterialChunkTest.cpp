//////////////////////////////////////////////////////////////////////
//
//  CoatedMaterialChunkTest.cpp - Contract test for the
//    `coated_material` chunk (docs/WETNESS_COAT_DESIGN.md Phase 2,
//    item 6): the parser / Job / registration surface.
//
//  WHAT THIS TEST OWNS -- and, as importantly, what it does not.  The
//  LAYER MATH is owned by LayeredWhiteFurnaceTest (energy) and
//  SPFBSDFConsistencyTest (value<->Scatter agreement and reciprocity).
//  This file owns the boundary a scene author actually hits:
//
//    1. THE SUBSTRATE ALLOWLIST, as a refusal MATRIX.  7.2 requires the
//       material to "refuse anything else at parse time with a message
//       naming the allowlist, rather than rendering something quietly
//       wrong".  A boolean parse-failure check would pass even if the
//       message were empty or wrong, so each refusal is checked for the
//       SPECIFIC diagnostic that distinguishes its branch:
//         - base that was never registered
//         - base that is a luminaire (has an emitter)
//         - base with no BSDF/SPF at all (dielectric)
//         - base of a supported PIPE but unsupported class (cooktorrance)
//         - coated-over-coated (the recursion the allowlist also closes)
//       Plus the positive controls -- lambertian, orennayar, ggx and
//       pbr_metallic_roughness must all be ACCEPTED, the last because
//       it resolves to a ggx_material at scene-build time.
//
//    2. coat_tint's `none` -> WHITE default.  `none` is the colour
//       manager's built-in BLACK painter, so a naive resolve would make
//       the default (untinted) coat OPAQUE.  Job::AddCoatedMaterial
//       special-cases it.  Checked BEHAVIOURALLY -- an omitted
//       coat_tint must produce the same BRDF response as one bound
//       explicitly to a white painter, and a DIFFERENT response from
//       one bound to black -- not by inspecting internals.
//
//    2b. THE SPECTRAL RED-PROOF for that default.  At 660 nm -- where
//       a white painter's Jakob-Hanika uplift collapses to 1.28e-5 --
//       an untinted coat must be indistinguishable from an explicitly
//       white one AND from an explicitly-neutral Beer-Lambert
//       baseline, while a saturated tint must differ.  This is the
//       check that catches a coat deciding "am I tinted?" from the
//       per-wavelength sample; every RGB assertion above stays green
//       through that bug.
//
//    3. DESCRIPTOR DEFAULTS ROUND-TRIP.  A chunk that omits every
//       optional coat parameter must behave EXACTLY like one that binds
//       each to the value its descriptor advertises (coat_weight 1.0,
//       coat_ior 1.33, coat_roughness 0.02, coat_thickness 0.0,
//       coat_absorption 0.0).  Proved by asking both BRDFs the same
//       value() question and requiring bit-identical answers, so the
//       descriptor's defaultValueHint cannot drift from Finalize()'s
//       actual bag.GetString() default.
//
//    4. requireSingle ON THE FIVE SCALAR SLOTS.  Every coat scalar is
//       read as ONE value; a per-channel painter bound there would have
//       g and b silently dropped on the RGB pipe while the spectral
//       pipe read something else through GetValueAtNM.  Refused.
//
//    5. THE SLOTS REACH THE MODEL.  A named scalar_painter bound to
//       coat_weight at 0.0 must give the BARE substrate response and at
//       1.0 must differ from it -- proving the binding is plumbed
//       through, not merely accepted.
//
//    6. EDITOR INTROSPECTION.  The material headers advertise
//       read-back and rebind, so GetTypeName / GetSlot / SetSlot are
//       exercised for real: every coat_* slot on the RIGHT pipe, a
//       rebind that visibly moves the BRDF, wrong-pipe rebinds
//       refused, and `base` reported as None.
//
//    7. THE API-LEVEL ALLOWLIST.  RISE_API_CreateCoatedMaterial
//       repeats the substrate check for callers that bypass the scene
//       language.  Driven directly, with no Job, so the deliberate
//       duplicate cannot rot unnoticed.
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
#include <cstdio>
#include <cstdlib>
#include <cmath>
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
#include "../src/Library/Materials/CoatedMaterial.h"
#include "../src/Library/SceneEditor/MaterialIntrospection.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Materials/LambertianMaterial.h"
#include "../src/Library/Materials/PerfectReflectorMaterial.h"

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

//////////////////////////////////////////////////////////////////////
// Scene plumbing -- HairMaterialChunkTest's pattern, unchanged.
//////////////////////////////////////////////////////////////////////

std::string WriteTempScene( const std::string& tag, const std::string& body )
{
	const char* tmp = getenv( "TMPDIR" );
	std::string dir = tmp ? tmp : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pid[32];
	std::snprintf( pid, sizeof(pid), "%d", static_cast<int>( ::getpid() ) );
	const std::string path = dir + "rise_coatedmat_" + tag + "_" + pid + ".RISEscene";
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

//! ParseBodyInto with stdout captured, so the eLog_Error diagnostics
//! Job::AddCoatedMaterial emits can be checked for their actual
//! WORDING and not merely for the boolean failure.
bool ParseBodyCapturing( const std::string& tag, const std::string& body, IJobPriv& job,
                          std::string& capturedOutput )
{
	const char* tmpEnv = getenv( "TMPDIR" );
	std::string dir = tmpEnv ? tmpEnv : "/tmp/";
	if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += "/";
	char pidbuf[32];
	std::snprintf( pidbuf, sizeof(pidbuf), "%d", static_cast<int>( ::getpid() ) );
	const std::string capPath = dir + "rise_coatedmat_stdout_" + tag + "_" + pidbuf + ".txt";

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
// Scene fragments
//////////////////////////////////////////////////////////////////////

std::string ColorPainter( const char* name, const char* rgb )
{
	std::ostringstream o;
	o << "uniformcolor_painter\n{\n\tname\t" << name << "\n\tcolor\t" << rgb << "\n}\n";
	return o.str();
}

std::string ScalarPainter( const char* name, double v )
{
	std::ostringstream o;
	o << "scalar_painter\n{\n\tname\t" << name << "\n\tvalue\t" << v << "\n}\n";
	return o.str();
}

//! A per-channel scalar painter, for the requireSingle negative.
//! `scalar_painter`'s form-2 `values` slot builds an RGBScalarPainter,
//! whose HasPerChannelVariation() is what requireSingle rejects.  Form
//! 1 (`value`) builds a UniformScalarPainter and would NOT trip it --
//! which is exactly the distinction under test, so the slot name here
//! is load-bearing rather than incidental.
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

//! `coated_material` fragment; omitted parameters exercise the
//! descriptor defaults.
std::string CoatedMat( const char* name, const char* base,
                        const char* weight = 0, const char* ior = 0,
                        const char* rough = 0, const char* thick = 0,
                        const char* absorb = 0, const char* tint = 0 )
{
	std::ostringstream o;
	o << "coated_material\n{\n\tname\t" << name << "\n\tbase\t" << base << "\n";
	if( weight ) o << "\tcoat_weight\t"     << weight << "\n";
	if( ior )    o << "\tcoat_ior\t"        << ior    << "\n";
	if( rough )  o << "\tcoat_roughness\t"  << rough  << "\n";
	if( thick )  o << "\tcoat_thickness\t"  << thick  << "\n";
	if( absorb ) o << "\tcoat_absorption\t" << absorb << "\n";
	if( tint )   o << "\tcoat_tint\t"       << tint   << "\n";
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

//! Max-channel BRDF response of a registered material at the probe.
//! Returns -1 when the material (or its BSDF) is missing.
double Respond( IJobPriv& job, const char* matName )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe();
	const Vector3 light = Vector3Ops::Normalize( Vector3( -0.3, 0.5, 0.8 ) );
	return ColorMath::MaxValue( m->GetBSDF()->value( light, ri ) );
}

//! Single-wavelength BRDF response at the probe.  660 nm is chosen
//! deliberately: it is where the Jakob-Hanika uplift of pure WHITE
//! collapses (1.28e-5, measured), so it is exactly the wavelength at
//! which a coat that decided "am I tinted?" from the SPECTRAL sample
//! would go opaque for an UNTINTED coat.  A 550 nm check would pass
//! either way and prove nothing.
double RespondNM( IJobPriv& job, const char* matName, double nm )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	if( !m || !m->GetBSDF() ) return -1.0;
	RayIntersectionGeometric ri = MakeProbe();
	const Vector3 light = Vector3Ops::Normalize( Vector3( -0.3, 0.5, 0.8 ) );
	return m->GetBSDF()->valueNM( light, ri, nm );
}

bool Registered( IJobPriv& job, const char* matName )
{
	return job.GetMaterials()->GetItem( matName ) != 0;
}

//! A coated material must actually BE a CoatedMaterial, not merely
//! something that registered under the name.
bool IsCoated( IJobPriv& job, const char* matName )
{
	IMaterial* m = job.GetMaterials()->GetItem( matName );
	return m && dynamic_cast<CoatedMaterial*>( m ) != 0;
}

//////////////////////////////////////////////////////////////////////
// 1. The substrate allowlist, as a refusal matrix
//////////////////////////////////////////////////////////////////////

void TestAllowlistAccepts()
{
	std::cout << "AllowlistAccepts" << std::endl;

	// lambertian
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + CoatedMat( "wet", "base" );
		ParseBodyInto( "acc_lamb", body, *job );
		Check( IsCoated( *job, "wet" ), "lambertian substrate accepted" );
		safe_release( job );
	}

	// orennayar
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + "orennayar_material\n{\n\tname\tbase\n\treflectance\tpnt\n\troughness\t0.4\n}\n"
		                 + CoatedMat( "wet", "base" );
		ParseBodyInto( "acc_on", body, *job );
		Check( IsCoated( *job, "wet" ), "orennayar substrate accepted" );
		safe_release( job );
	}

	// ggx
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string body = ColorPainter( "pd", "0.6 0.4 0.3" ) + ColorPainter( "ps", "0.04 0.04 0.04" )
		                 + "ggx_material\n{\n\tname\tbase\n\trd\tpd\n\trs\tps\n"
		                   "\talphax\t0.2\n\talphay\t0.2\n\tior\t1.5\n\textinction\t0.0\n}\n"
		                 + CoatedMat( "wet", "base" );
		ParseBodyInto( "acc_ggx", body, *job );
		Check( IsCoated( *job, "wet" ), "ggx substrate accepted" );
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
		                 + CoatedMat( "wet", "base" );
		ParseBodyInto( "acc_pbr", body, *job );
		Check( IsCoated( *job, "wet" ), "pbr_metallic_roughness substrate accepted (resolves to ggx)" );
		safe_release( job );
	}
}

void TestAllowlistRefusals()
{
	std::cout << "AllowlistRefusals" << std::endl;

	// (a) base never registered
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = CoatedMat( "wet", "nosuchmaterial" );
		ParseBodyCapturing( "ref_missing", body, *job, out );
		Check( !Registered( *job, "wet" ), "unregistered base: material not registered" );
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
		                 + CoatedMat( "wet", "base" );
		ParseBodyCapturing( "ref_lum", body, *job, out );
		Check( !Registered( *job, "wet" ), "luminaire base: material not registered" );
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
		                 + CoatedMat( "wet", "base" );
		ParseBodyCapturing( "ref_diel", body, *job, out );
		Check( !Registered( *job, "wet" ), "no-BSDF base: material not registered" );
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
		                 + CoatedMat( "wet", "base" );
		ParseBodyCapturing( "ref_ct", body, *job, out );
		Check( !Registered( *job, "wet" ), "cooktorrance base: material not registered" );
		Check( Contains( out.c_str(), "not one of the supported scattering classes" ),
		       "cooktorrance base: diagnostic names the class" );
		safe_release( job );
	}

	// (e) coated over coated -- the allowlist closes this recursion
	//     too, which matters because a coated stack would otherwise
	//     compound recycling series with no depth bound.
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + CoatedMat( "wet1", "base" )
		                 + CoatedMat( "wet2", "wet1" );
		ParseBodyCapturing( "ref_coated", body, *job, out );
		Check( IsCoated( *job, "wet1" ), "coated-over-coated: the inner coat still registers" );
		Check( !Registered( *job, "wet2" ), "coated-over-coated: the outer coat is refused" );
		Check( Contains( out.c_str(), "not one of the supported scattering classes" ),
		       "coated-over-coated: diagnostic names the class" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 2. coat_tint `none` -> white (NOT the manager's black `none`)
//////////////////////////////////////////////////////////////////////

void TestCoatTintDefault()
{
	std::cout << "CoatTintDefault" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt",   "0.6 0.4 0.3" )
	                 + ColorPainter( "white", "1 1 1" )
	                 + ColorPainter( "black", "0 0 0" )
	                 + LambertianMat( "base", "pnt" )
	                 // thickness/absorption left at 0 so tint is the ONLY
	                 // term separating these three.
	                 + CoatedMat( "omitted", "base" )
	                 + CoatedMat( "explicitwhite", "base", 0, 0, 0, 0, 0, "white" )
	                 + CoatedMat( "explicitblack", "base", 0, 0, 0, 0, 0, "black" );
	ParseBodyInto( "tint", body, *job );

	const double omitted = Respond( *job, "omitted" );
	const double white   = Respond( *job, "explicitwhite" );
	const double black   = Respond( *job, "explicitblack" );

	Check( omitted > 0, "omitted coat_tint parses and responds" );
	Check( white   > 0, "explicit white coat_tint parses and responds" );
	Check( omitted == white,
	       "omitted coat_tint behaves EXACTLY like an explicit white one" );
	// If `none` were resolved through the painter manager it would bind
	// the built-in BLACK painter and this would be an equality instead.
	Check( black >= 0 && black < omitted,
	       "an explicitly BLACK coat_tint is darker -- `none` is not being read as black" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 2b. THE SPECTRAL RED-PROOF for the untinted default.
//
// This is the check that would have caught the round-2 bug, and the
// RGB checks above could not: an untinted coat that decides "am I
// tinted?" from the per-wavelength sample sees a white painter's
// Jakob-Hanika uplift COLLAPSE off the red end (1.28e-5 at 660 nm,
// measured) and multiplies pow(1.3e-5, 1/cos) ~ 0 into its
// transmittance.  RGB stays perfect throughout, so only a spectral
// assertion at a red wavelength can see it.
//
// The invariant: at 660 nm an untinted coat must be INDISTINGUISHABLE
// from an explicitly white one AND from a coat whose Beer-Lambert
// terms are explicitly neutral -- three spellings of "no attenuation"
// that must agree exactly -- while a genuinely tinted coat must
// differ.  Both halves matter: the first alone would also pass if the
// tint term were dead code.
//////////////////////////////////////////////////////////////////////

void TestUntintedIsClearSpectrally()
{
	std::cout << "UntintedIsClearSpectrally" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt",   "0.6 0.4 0.3" )
	                 + ColorPainter( "white", "1 1 1" )
	                 + ColorPainter( "amber", "0.92 0.55 0.18" )
	                 + LambertianMat( "base", "pnt" )
	                 + CoatedMat( "omitted",  "base" )
	                 + CoatedMat( "white",    "base", 0, 0, 0, 0, 0, "white" )
	                 // Beer-Lambert explicitly neutral: absorption 0 at
	                 // thickness 0, white tint.  A third independent
	                 // spelling of "the coat attenuates nothing".
	                 + CoatedMat( "neutral",  "base", "1.0", "1.33", "0.02", "0.0", "0.0", "white" )
	                 + CoatedMat( "tinted",   "base", 0, 0, 0, 0, 0, "amber" );
	ParseBodyInto( "specclear", body, *job );

	const double kRed = 660.0;
	const double omitted = RespondNM( *job, "omitted", kRed );
	const double white   = RespondNM( *job, "white",   kRed );
	const double neutral = RespondNM( *job, "neutral", kRed );
	const double tinted  = RespondNM( *job, "tinted",  kRed );

	Check( omitted > 0, "untinted coat has NON-ZERO response at 660 nm (not opaque in the red)" );
	Check( omitted == white,
	       "660 nm: omitted coat_tint == explicit white" );
	Check( omitted == neutral,
	       "660 nm: omitted coat_tint == explicitly-neutral Beer-Lambert baseline" );
	Check( tinted >= 0 && tinted != omitted,
	       "660 nm: a saturated tint DOES change the response (the tint term is live)" );

	// The same three-way identity must hold across the band, not just
	// at the one wavelength -- a fix that special-cased 660 would pass
	// the checks above and still be wrong at 700.
	bool allAgree = true, anyPositive = false;
	for( double nm = 380.0; nm <= 780.0 + 1e-9; nm += 20.0 )
	{
		const double o = RespondNM( *job, "omitted", nm );
		const double w = RespondNM( *job, "white",   nm );
		const double n = RespondNM( *job, "neutral", nm );
		if( o != w || o != n ) allAgree = false;
		if( o > 0 ) anyPositive = true;
	}
	Check( allAgree,  "380-780 nm: untinted == white == neutral at every sampled wavelength" );
	Check( anyPositive, "380-780 nm: the untinted coat responds somewhere (sanity)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 3. Descriptor defaults round-trip
//////////////////////////////////////////////////////////////////////

void TestDescriptorDefaultsRoundTrip()
{
	std::cout << "DescriptorDefaultsRoundTrip" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
	                 + LambertianMat( "base", "pnt" )
	                 + CoatedMat( "defaults", "base" )
	                 // Exactly the values the descriptor advertises.
	                 + CoatedMat( "explicit", "base", "1.0", "1.33", "0.02", "0.0", "0.0" );
	ParseBodyInto( "defaults", body, *job );

	const double defaulted = Respond( *job, "defaults" );
	const double explicitv = Respond( *job, "explicit" );

	Check( defaulted > 0, "all-defaults chunk parses and responds" );
	Check( defaulted == explicitv,
	       "descriptor defaults (1.0/1.33/0.02/0.0/0.0) round-trip bit-identically" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 4. requireSingle on the five scalar slots
//////////////////////////////////////////////////////////////////////

void TestRequireSingleOnScalarSlots()
{
	std::cout << "RequireSingleOnScalarSlots" << std::endl;

	const char* slots[] = { "coat_weight", "coat_ior", "coat_roughness",
	                        "coat_thickness", "coat_absorption" };

	for( const char* slot : slots )
	{
		IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
		std::string out;
		std::ostringstream chunk;
		chunk << "coated_material\n{\n\tname\twet\n\tbase\tbase\n\t"
		      << slot << "\tpercc\n}\n";
		std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
		                 + LambertianMat( "base", "pnt" )
		                 + RGBScalarPainter( "percc", "0.2 0.5 0.9" )
		                 + chunk.str();
		ParseBodyCapturing( std::string( "single_" ) + slot, body, *job, out );

		Check( !Registered( *job, "wet" ),
		       std::string( slot ) + ": per-channel scalar painter refused" );
		safe_release( job );
	}
}

//////////////////////////////////////////////////////////////////////
// 5. The slots reach the model
//////////////////////////////////////////////////////////////////////

void TestSlotsReachTheModel()
{
	std::cout << "SlotsReachTheModel" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
	                 + LambertianMat( "base", "pnt" )
	                 + ScalarPainter( "zero", 0.0 )
	                 + ScalarPainter( "one",  1.0 )
	                 + CoatedMat( "nocover",   "base", "zero" )
	                 + CoatedMat( "fullcover", "base", "one" );
	ParseBodyInto( "slots", body, *job );

	const double bare      = Respond( *job, "base" );
	const double nocover   = Respond( *job, "nocover" );
	const double fullcover = Respond( *job, "fullcover" );

	Check( nocover == bare,
	       "coat_weight = 0 gives EXACTLY the bare substrate response" );
	Check( fullcover != bare,
	       "coat_weight = 1 changes the response (the slot reaches the model)" );

	// 7.3's coverage semantics, at the BRDF level: a wet substrate is
	// DARKER than a dry one, which is the phenomenon the whole design
	// doc opens with.  Checked here as a direction, not a magnitude --
	// the magnitude is LayeredWhiteFurnaceTest's job.
	Check( fullcover < bare,
	       "full coverage DARKENS a diffuse substrate (the wet-darkening direction)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 5b. COVERAGE IS EXACTLY LINEAR IN c.
//
// 7.3 defines coat_weight as a sub-pixel AREA fraction, so the
// response at coverage c must be the statistical mixture
//     f(c)  ==  (1 - c) * f_bare  +  c * f_fullycoated
// EXACTLY -- not approximately, and not merely monotonically.  That is
// the difference between a coverage fraction and a gloss knob, and it
// is the one property a lit render cannot check: in
// scenes/Tests/Materials/coated_material.RISEscene the half-coverage
// sphere sits visibly between its dry and wet neighbours, but the
// magnitude there is contaminated by floor bounce, inter-sphere
// transport and 8-bit sRGB encoding.  Here it is exact arithmetic on
// the BRDF, at several coverages and both colour pipes.
//////////////////////////////////////////////////////////////////////

void TestCoverageIsLinear()
{
	std::cout << "CoverageIsLinear" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt", "0.62 0.30 0.20" )
	                 + LambertianMat( "base", "pnt" )
	                 + CoatedMat( "c000", "base", "0.0" )
	                 + CoatedMat( "c025", "base", "0.25" )
	                 + CoatedMat( "c050", "base", "0.5" )
	                 + CoatedMat( "c075", "base", "0.75" )
	                 + CoatedMat( "c100", "base", "1.0" );
	ParseBodyInto( "linear", body, *job );

	const double f0 = Respond( *job, "c000" );
	const double f1 = Respond( *job, "c100" );
	Check( f0 > 0 && f1 > 0 && f1 < f0, "coverage endpoints are sane (RGB)" );

	struct Pt { const char* name; double c; };
	const Pt pts[] = { { "c025", 0.25 }, { "c050", 0.50 }, { "c075", 0.75 } };

	for( const Pt& p : pts )
	{
		const double got  = Respond( *job, p.name );
		const double want = ( 1.0 - p.c ) * f0 + p.c * f1;
		// Exact mixture: the same painters, the same intersection, the
		// same closed form -- the only slack is FP association order.
		const double rel = ( want > 0 ) ? std::fabs( got - want ) / want : std::fabs( got - want );
		Check( rel < 1e-12,
		       std::string( "RGB: f(c) is the exact mixture at " ) + p.name );
	}

	// Same statement on the spectral pipe, at the red wavelength where
	// the tint machinery is most fragile.
	const double g0 = RespondNM( *job, "c000", 660.0 );
	const double g1 = RespondNM( *job, "c100", 660.0 );
	Check( g0 > 0 && g1 > 0, "coverage endpoints are sane (NM @ 660)" );
	for( const Pt& p : pts )
	{
		const double got  = RespondNM( *job, p.name, 660.0 );
		const double want = ( 1.0 - p.c ) * g0 + p.c * g1;
		const double rel = ( want > 0 ) ? std::fabs( got - want ) / want : std::fabs( got - want );
		Check( rel < 1e-12,
		       std::string( "NM 660: f(c) is the exact mixture at " ) + p.name );
	}

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 6. Editor introspection -- the headers advertise read-back / rebind,
//    so make that true rather than aspirational.
//////////////////////////////////////////////////////////////////////

void TestEditorIntrospection()
{
	std::cout << "EditorIntrospection" << std::endl;

	IJobPriv* job = 0; RISE_CreateJob( (IJob**)&job );
	std::string body = ColorPainter( "pnt", "0.6 0.4 0.3" )
	                 + LambertianMat( "base", "pnt" )
	                 + ScalarPainter( "half", 0.5 )
	                 + CoatedMat( "wet", "base" );
	ParseBodyInto( "editor", body, *job );

	IMaterial* m = job->GetMaterials()->GetItem( "wet" );
	Check( m != 0, "introspection fixture registered" );
	if( !m ) { safe_release( job ); return; }

	// Type name -- what the property panel labels the material.
	Check( MaterialIntrospection::GetTypeName( *m ) == String( "Coated" ),
	       "introspection reports the type name `Coated`" );

	// Every coat_* slot must be readable through GetSlot, on the RIGHT
	// pipe: the five physical scalars on ScalarPainter, coat_tint on
	// Painter.  A slot registered on the wrong pipe would still be
	// "present" but would refuse every rebind the editor attempted.
	const char* scalarSlots[] = { "coat_weight", "coat_ior", "coat_roughness",
	                              "coat_thickness", "coat_absorption" };
	for( const char* want : scalarSlots )
	{
		const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *m, String( want ) );
		Check( ref.kind == MaterialSlotRef::ScalarPainter && ref.scalarPainter != 0,
		       std::string( "GetSlot exposes " ) + want + " on the SCALAR pipe" );
	}
	{
		const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *m, String( "coat_tint" ) );
		Check( ref.kind == MaterialSlotRef::Painter && ref.painter != 0,
		       "GetSlot exposes coat_tint on the COLOUR pipe" );
	}
	{
		const MaterialSlotRef ref = MaterialIntrospection::GetSlot( *m, String( "base" ) );
		Check( ref.kind == MaterialSlotRef::None,
		       "GetSlot reports `base` as None (a material, not a painter slot)" );
	}

	// SetSlot actually reaches the model: rebind coat_weight to 0.5 and
	// require the BRDF response to move.
	const double before = Respond( *job, "wet" );

	IScalarPainter* half = job->GetScalarPainters()->GetItem( "half" );
	Check( half != 0, "named scalar_painter available for rebind" );
	if( half ) {
		const bool set = MaterialIntrospection::SetSlot( *m, String( "coat_weight" ), 0, half );
		Check( set, "SetSlot accepts coat_weight" );
		const double after = Respond( *job, "wet" );
		Check( after != before, "SetSlot( coat_weight ) changes the BRDF response" );
	}

	// A scalar slot must REFUSE a colour painter and vice versa.
	IPainter* col = job->GetPainters()->GetItem( "pnt" );
	Check( col != 0, "named colour painter available" );
	if( col ) {
		Check( !MaterialIntrospection::SetSlot( *m, String( "coat_weight" ), col, 0 ),
		       "SetSlot refuses a COLOUR painter on the scalar coat_weight slot" );
		Check( MaterialIntrospection::SetSlot( *m, String( "coat_tint" ), col, 0 ),
		       "SetSlot accepts a colour painter on coat_tint" );
	}

	// `base` is deliberately not a rebindable slot.
	Check( !MaterialIntrospection::SetSlot( *m, String( "base" ), col, 0 ),
	       "SetSlot refuses `base` (a material, not a painter slot)" );

	safe_release( job );
}

//////////////////////////////////////////////////////////////////////
// 7. THE API-LEVEL ALLOWLIST CHECK, exercised directly.
//
// `RISE_API_CreateCoatedMaterial` repeats the allowlist check that
// `Job::AddCoatedMaterial` already performs.  That duplication is
// deliberate -- it covers callers that never touch the scene language
// (the glTF importer, the interactive editor, tests) -- but a
// deliberate duplicate is exactly the kind of code that rots, because
// the scene-language tests above would stay green if it were deleted.
// So drive the factory directly, with no Job in the picture.
//////////////////////////////////////////////////////////////////////

void TestApiLevelAllowlist()
{
	std::cout << "ApiLevelAllowlist" << std::endl;

	UniformColorPainter* col  = new UniformColorPainter( RISEPel( 0.6, 0.4, 0.3 ) ); col->addref();
	UniformColorPainter* wht  = new UniformColorPainter( RISEPel( 1.0, 1.0, 1.0 ) ); wht->addref();
	UniformScalarPainter* one = new UniformScalarPainter( 1.0 );  one->addref();
	UniformScalarPainter* ior = new UniformScalarPainter( 1.33 ); ior->addref();
	UniformScalarPainter* rgh = new UniformScalarPainter( 0.02 ); rgh->addref();
	UniformScalarPainter* zed = new UniformScalarPainter( 0.0 );  zed->addref();

	// Positive control: an allowlisted substrate is accepted.
	{
		LambertianMaterial* lamb = new LambertianMaterial( *col ); lamb->addref();
		IMaterial* out = 0;
		const bool ok = RISE_API_CreateCoatedMaterial( &out, *lamb, *one, *ior, *rgh, *zed, *zed, *wht );
		Check( ok && out != 0, "API accepts a lambertian substrate" );
		safe_release( out );
		safe_release( lamb );
	}

	// Negative: a disallowed substrate is refused, `out` left null.
	// PerfectReflectorMaterial has no continuum BSDF at all, so it
	// trips the same branch a dielectric does through the parser.
	{
		PerfectReflectorMaterial* mirror = new PerfectReflectorMaterial( *col ); mirror->addref();
		IMaterial* out = reinterpret_cast<IMaterial*>( 0x1 );	// poison, to prove it is overwritten-or-untouched
		const bool ok = RISE_API_CreateCoatedMaterial( &out, *mirror, *one, *ior, *rgh, *zed, *zed, *wht );
		Check( !ok, "API REFUSES a disallowed substrate (perfect reflector)" );
		Check( out == reinterpret_cast<IMaterial*>( 0x1 ),
		       "API leaves the out-pointer untouched on refusal (no partial construction)" );
		safe_release( mirror );
	}

	// Negative: a null out-pointer is refused rather than crashing.
	{
		LambertianMaterial* lamb = new LambertianMaterial( *col ); lamb->addref();
		Check( !RISE_API_CreateCoatedMaterial( 0, *lamb, *one, *ior, *rgh, *zed, *zed, *wht ),
		       "API refuses a null out-pointer" );
		safe_release( lamb );
	}

	safe_release( zed ); safe_release( rgh ); safe_release( ior );
	safe_release( one ); safe_release( wht ); safe_release( col );
}

} // anonymous namespace

int main()
{
	GlobalLog();

	std::cout << "===== coated_material chunk contract test =====" << std::endl;

	TestAllowlistAccepts();
	TestAllowlistRefusals();
	TestCoatTintDefault();
	TestUntintedIsClearSpectrally();
	TestDescriptorDefaultsRoundTrip();
	TestRequireSingleOnScalarSlots();
	TestSlotsReachTheModel();
	TestCoverageIsLinear();
	TestEditorIntrospection();
	TestApiLevelAllowlist();

	std::cout << std::endl
	          << "Results: " << passCount << " passed, "
	          << failCount << " failed" << std::endl;
	return ( failCount > 0 ) ? 1 : 0;
}
