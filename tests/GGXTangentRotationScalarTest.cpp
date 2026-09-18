//////////////////////////////////////////////////////////////////////
//
//  GGXTangentRotationScalarTest.cpp - DL-16 regression
//    (docs/DEBT_LEDGER.md, docs/CLOTH_FABRIC_DESIGN.md sec 15 item 4).
//
//  `ggx_material.tangent_rotation` was Color-pipe ONLY -- the promised
//  Scalar-pipe alias (so a `fabric_material`'s `weave_rotation` and its
//  substrate's own rotation could share ONE painter) was never added.
//  Pre-fix, `ggx_material { ... tangent_rotation_scalar <name> ... }`
//  is an undeclared parameter and the scene HARD-FAILS to parse
//  ("not declared in `ggx_material` descriptor").
//
//  This file proves:
//    1. MONEY: a scene binding ONE scalar_painter to both a
//       `fabric_material`'s `weave_rotation` and its GGX base's new
//       `tangent_rotation_scalar` parses, and both rotations read back
//       IDENTICAL (the additive weave-then-substrate convention needs
//       them to be readable from the SAME source to compose sanely).
//    2. The legacy Color-pipe `tangent_rotation` keeps working
//       (deprecated, not removed) -- a plain regression pin.
//    3. Preferring Scalar: when BOTH `tangent_rotation` (Color) and
//       `tangent_rotation_scalar` (Scalar) are set to DIFFERENT values,
//       the Scalar one wins.
//    4. MONEY render-parity row: a GGX material with an anisotropic
//       lobe (alphax != alphay) rotated by `tangent_rotation` bound to
//       a colour painter and an IDENTICAL material rotated by
//       `tangent_rotation_scalar` bound to a scalar_painter of the SAME
//       numeric value produce the SAME BSDF value at a fixed
//       (incoming, outgoing) direction pair that only degenerates to
//       the same answer when the tangent frame is actually rotated
//       identically by both pipes.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/Library/Job.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Intersection/RayIntersectionGeometric.h"
#include "../src/Library/Utilities/Ray.h"
#include "../src/Library/Utilities/OrthonormalBasis3D.h"
#include "../src/Library/Utilities/Math3D/VectorsOps.h"
#include "../src/Library/Materials/GGXMaterial.h"
#include "../src/Library/Materials/FabricMaterial.h"
#include "../src/Library/Interfaces/IBSDF.h"

using namespace RISE;
using namespace RISE::Implementation;

namespace
{
	int s_pass = 0;
	int s_fail = 0;

	void Check( bool ok, const std::string& what )
	{
		if( ok ) { ++s_pass; std::printf( "  ok  : %s\n", what.c_str() ); }
		else     { ++s_fail; std::printf( "  FAIL: %s\n", what.c_str() ); }
	}

	std::string TempPath( const char* name )
	{
		const char* base = std::getenv( "TMPDIR" );
		std::string dir = base ? base : "/tmp";
		if( !dir.empty() && dir[dir.size()-1] != '/' ) dir += '/';
		return dir + name;
	}

	//! Loads `body` as a scene via the canonical CST path.  Returns
	//! nullptr (and leaves nothing to release) on a parse/derive
	//! failure -- exactly the outcome the pre-fix "undeclared parameter"
	//! path produces.
	Job* LoadScene( const std::string& body, const char* tag )
	{
		const std::string path = TempPath( ( std::string( "rise_ggxtanrot_" ) + tag + ".RISEscene" ).c_str() );
		{ std::ofstream o( path.c_str(), std::ios::binary | std::ios::trunc ); o << body; }
		Job* job = new Job();
		if( !job->LoadAsciiSceneViaCst( path.c_str() ) ) {
			job->release();
			std::remove( path.c_str() );
			return nullptr;
		}
		std::remove( path.c_str() );
		return job;
	}

	//! A fixed shading point + probe, mirroring FabricMaterialChunkTest.cpp's
	//! MakeProbe idiom: an oblique incidence so an anisotropic lobe's
	//! rotation actually changes the answer (a normal-incidence probe
	//! can't distinguish a rotated frame from an unrotated one for an
	//! isotropic-looking slice).
	RayIntersectionGeometric MakeProbe()
	{
		const double th = 35.0 * 3.14159265358979323846 / 180.0;
		const Vector3 inDir( sin(th), 0, -cos(th) );
		Ray inRay( Point3( sin(th), 0, 1.0 ), inDir );
		RasterizerState rs = { 0, 0 };
		RayIntersectionGeometric ri( inRay, rs );
		ri.bHit = true;
		ri.range = 1.0;
		ri.ptIntersection = Point3( 0, 0, 0 );
		ri.vNormal = Vector3( 0, 0, 1 );
		ri.vGeomNormal = ri.vNormal;
		ri.onb.CreateFromW( Vector3( 0, 0, 1 ) );
		ri.ptCoord = Point2( 0.5, 0.5 );
		return ri;
	}

	Vector3 OutgoingProbeDir()
	{
		const double th2 = 20.0 * 3.14159265358979323846 / 180.0;
		const double phi2 = 55.0 * 3.14159265358979323846 / 180.0;
		return Vector3Ops::Normalize( Vector3( sin(th2)*cos(phi2), sin(th2)*sin(phi2), cos(th2) ) );
	}

	std::string CommonPreamble()
	{
		return
			"RISE ASCII SCENE 7\n"
			"uniformcolor_painter\n{\n\tname\tgrey\n\tcolor\t0.6 0.6 0.6\n}\n"
			"uniformcolor_painter\n{\n\tname\tf0\n\tcolor\t0.9 0.9 0.9\n}\n";
	}
}

//! Test 1 + 2 + 3: parser/plumbing.
static void TestSharedPainterAndPreference()
{
	std::printf( "-- DL-16 shared scalar_painter + preference order --\n" );

	// Test 1 MONEY: one scalar_painter drives BOTH fabric_material's
	// weave_rotation AND the GGX base's tangent_rotation_scalar.
	{
		std::ostringstream o;
		o << CommonPreamble();
		o << "scalar_painter\n{\n\tname\tshared_rot\n\tvalue\t0.35\n}\n";
		o << "ggx_material\n{\n\tname\tbase1\n\trd\tgrey\n\trs\tf0\n"
		     "\talphax\t0.05\n\talphay\t0.30\n\tfresnel_mode\tschlick_f0\n"
		     "\ttangent_rotation_scalar\tshared_rot\n}\n";
		o << "fabric_material\n{\n\tname\tcloth1\n\tbase\tbase1\n\tweave_rotation\tshared_rot\n}\n";

		Job* job = LoadScene( o.str(), "shared" );
		Check( job != nullptr,
			"MONEY: ggx_material.tangent_rotation_scalar + fabric_material.weave_rotation "
			"bound to the SAME scalar_painter parses (pre-DL-16: `tangent_rotation_scalar` "
			"undeclared -> hard parse error)" );
		if( job ) {
			IMaterial* base = job->GetMaterials()->GetItem( "base1" );
			GGXMaterial* ggx = base ? dynamic_cast<GGXMaterial*>( base ) : nullptr;
			Check( ggx != nullptr, "base1 registered as a live GGXMaterial" );

			IMaterial* cloth = job->GetMaterials()->GetItem( "cloth1" );
			FabricMaterial* fab = cloth ? dynamic_cast<FabricMaterial*>( cloth ) : nullptr;
			Check( fab != nullptr, "cloth1 registered as a live FabricMaterial" );

			if( ggx && fab ) {
				const RayIntersectionGeometric probe = MakeProbe();
				const IScalarPainter* ggxRotScalar = ggx->GetTangentRotationScalar();
				Check( ggxRotScalar != nullptr,
					"ggx_material's tangent_rotation_scalar resolved to a live IScalarPainter" );
				if( ggxRotScalar ) {
					const double ggxAngle = ggxRotScalar->GetValuesAt( probe ).v[0];
					const double fabAngle = fab->GetWeaveRotation().GetValuesAt( probe ).v[0];
					Check( std::fabs( ggxAngle - 0.35 ) < 1e-12,
						"ggx tangent_rotation_scalar reads back 0.35 -- got " + std::to_string( ggxAngle ) );
					Check( std::fabs( fabAngle - 0.35 ) < 1e-12,
						"fabric weave_rotation reads back 0.35 -- got " + std::to_string( fabAngle ) );
					Check( std::fabs( ggxAngle - fabAngle ) < 1e-15,
						"MONEY: both rotations read back IDENTICAL from the ONE shared "
						"scalar_painter -- ggx=" + std::to_string( ggxAngle ) +
						" fabric=" + std::to_string( fabAngle ) );
				}
				// Legacy Color-pipe slot stays untouched (nullptr) when only
				// the scalar alias was authored.
				Check( ggx->GetTangentRotation() == nullptr,
					"legacy Color-pipe tangent_rotation stays null when only "
					"tangent_rotation_scalar was authored (no accidental cross-binding)" );
			}
			job->release();
		}
	}

	// Test 2: the legacy Color-pipe `tangent_rotation` still works
	// (deprecated, not removed) -- a plain regression pin.
	{
		std::ostringstream o;
		o << CommonPreamble();
		o << "uniformcolor_painter\n{\n\tname\trot_col\n\tcolor\t0.42 0.42 0.42\n}\n";
		o << "ggx_material\n{\n\tname\tbase2\n\trd\tgrey\n\trs\tf0\n"
		     "\talphax\t0.05\n\talphay\t0.30\n\tfresnel_mode\tschlick_f0\n"
		     "\ttangent_rotation\trot_col\n}\n";
		Job* job = LoadScene( o.str(), "legacycolor" );
		Check( job != nullptr, "legacy Color-pipe tangent_rotation (named painter) still parses" );
		if( job ) {
			GGXMaterial* ggx = dynamic_cast<GGXMaterial*>( job->GetMaterials()->GetItem( "base2" ) );
			Check( ggx != nullptr, "base2 registered as a live GGXMaterial" );
			if( ggx ) {
				Check( ggx->GetTangentRotationScalar() == nullptr,
					"tangent_rotation_scalar stays null when only the legacy field was authored" );
				Check( ggx->GetTangentRotation() != nullptr,
					"legacy tangent_rotation resolved to the named colour painter" );
			}
			job->release();
		}
	}

	// Test 3: preferring Scalar.  Both fields set to DIFFERENT values --
	// the Scalar one must win.
	{
		std::ostringstream o;
		o << CommonPreamble();
		o << "uniformcolor_painter\n{\n\tname\trot_col2\n\tcolor\t0.10 0.10 0.10\n}\n";
		o << "scalar_painter\n{\n\tname\trot_sc2\n\tvalue\t0.60\n}\n";
		o << "ggx_material\n{\n\tname\tbase3\n\trd\tgrey\n\trs\tf0\n"
		     "\talphax\t0.05\n\talphay\t0.30\n\tfresnel_mode\tschlick_f0\n"
		     "\ttangent_rotation\trot_col2\n\ttangent_rotation_scalar\trot_sc2\n}\n";
		Job* job = LoadScene( o.str(), "prefer" );
		Check( job != nullptr, "ggx_material with BOTH tangent_rotation and tangent_rotation_scalar parses" );
		if( job ) {
			GGXMaterial* ggx = dynamic_cast<GGXMaterial*>( job->GetMaterials()->GetItem( "base3" ) );
			if( ggx ) {
				const RayIntersectionGeometric probe = MakeProbe();
				const Vector3 wo = OutgoingProbeDir();
				IBSDF* bsdf = ggx->GetBSDF();
				const RISEPel valBoth = bsdf->value( wo, probe );

				// Compare against a scalar-ONLY material at 0.60 -- must match
				// (scalar wins over the 0.10 colour value).
				std::ostringstream o2;
				o2 << CommonPreamble();
				o2 << "scalar_painter\n{\n\tname\trot_sc2b\n\tvalue\t0.60\n}\n";
				o2 << "ggx_material\n{\n\tname\tbase3b\n\trd\tgrey\n\trs\tf0\n"
				      "\talphax\t0.05\n\talphay\t0.30\n\tfresnel_mode\tschlick_f0\n"
				      "\ttangent_rotation_scalar\trot_sc2b\n}\n";
				Job* job2 = LoadScene( o2.str(), "prefer_scalaronly" );
				Check( job2 != nullptr, "scalar-only 0.60 comparison material parses" );
				if( job2 ) {
					GGXMaterial* ggx2 = dynamic_cast<GGXMaterial*>( job2->GetMaterials()->GetItem( "base3b" ) );
					if( ggx2 ) {
						const RISEPel valScalarOnly = ggx2->GetBSDF()->value( wo, probe );
						Check( std::fabs( valBoth[0] - valScalarOnly[0] ) < 1e-9 &&
						       std::fabs( valBoth[1] - valScalarOnly[1] ) < 1e-9 &&
						       std::fabs( valBoth[2] - valScalarOnly[2] ) < 1e-9,
							"MONEY: tangent_rotation_scalar (0.60) WINS over tangent_rotation "
							"(0.10) when both are set on the same material" );
					}
					job2->release();
				}
			}
			job->release();
		}
	}
}

//! Test 4 MONEY: render-parity between the Color pipe and the new
//! Scalar pipe at an EQUAL numeric rotation.
static void TestRenderParity()
{
	std::printf( "-- DL-16 render-parity: Color pipe vs Scalar pipe at equal value --\n" );

	const double angle = 0.7123;	// radians; arbitrary non-special value

	std::ostringstream oColor;
	oColor << CommonPreamble();
	oColor << "uniformcolor_painter\n{\n\tname\trotc\n\tcolor\t" << angle << " " << angle << " " << angle << "\n}\n";
	oColor << "ggx_material\n{\n\tname\tggxc\n\trd\tgrey\n\trs\tf0\n"
	          "\talphax\t0.04\n\talphay\t0.35\n\tfresnel_mode\tschlick_f0\n"
	          "\ttangent_rotation\trotc\n}\n";

	std::ostringstream oScalar;
	oScalar << CommonPreamble();
	oScalar << "scalar_painter\n{\n\tname\trots\n\tvalue\t" << angle << "\n}\n";
	oScalar << "ggx_material\n{\n\tname\tggxs\n\trd\tgrey\n\trs\tf0\n"
	           "\talphax\t0.04\n\talphay\t0.35\n\tfresnel_mode\tschlick_f0\n"
	           "\ttangent_rotation_scalar\trots\n}\n";

	Job* jobC = LoadScene( oColor.str(), "parity_color" );
	Job* jobS = LoadScene( oScalar.str(), "parity_scalar" );
	Check( jobC != nullptr && jobS != nullptr, "both parity fixtures parse" );
	if( jobC && jobS ) {
		GGXMaterial* ggxC = dynamic_cast<GGXMaterial*>( jobC->GetMaterials()->GetItem( "ggxc" ) );
		GGXMaterial* ggxS = dynamic_cast<GGXMaterial*>( jobS->GetMaterials()->GetItem( "ggxs" ) );
		Check( ggxC != nullptr && ggxS != nullptr, "both GGX materials registered" );
		if( ggxC && ggxS ) {
			const RayIntersectionGeometric probe = MakeProbe();
			const Vector3 wo = OutgoingProbeDir();
			const RISEPel valC = ggxC->GetBSDF()->value( wo, probe );
			const RISEPel valS = ggxS->GetBSDF()->value( wo, probe );
			std::printf( "     colour-pipe value = (%.10f, %.10f, %.10f)\n", valC[0], valC[1], valC[2] );
			std::printf( "     scalar-pipe  value = (%.10f, %.10f, %.10f)\n", valS[0], valS[1], valS[2] );
			Check( std::fabs( valC[0] - valS[0] ) < 1e-9 &&
			       std::fabs( valC[1] - valS[1] ) < 1e-9 &&
			       std::fabs( valC[2] - valS[2] ) < 1e-9,
				"MONEY: identical numeric rotation via Color pipe vs Scalar pipe produces "
				"the SAME anisotropic GGX BSDF value (same rotated tangent frame either way)" );

			// Sanity: the rotation must actually MATTER at this probe -- an
			// unrotated (angle 0) anisotropic lobe must differ from both,
			// otherwise this whole test would pass vacuously regardless of
			// whether rotation is applied at all.
			std::ostringstream oZero;
			oZero << CommonPreamble();
			oZero << "ggx_material\n{\n\tname\tggxz\n\trd\tgrey\n\trs\tf0\n"
			         "\talphax\t0.04\n\talphay\t0.35\n\tfresnel_mode\tschlick_f0\n}\n";
			Job* jobZ = LoadScene( oZero.str(), "parity_zero" );
			if( jobZ ) {
				GGXMaterial* ggxZ = dynamic_cast<GGXMaterial*>( jobZ->GetMaterials()->GetItem( "ggxz" ) );
				if( ggxZ ) {
					const RISEPel valZ = ggxZ->GetBSDF()->value( wo, probe );
					Check( std::fabs( valZ[0] - valC[0] ) > 1e-6 || std::fabs( valZ[1] - valC[1] ) > 1e-6,
						"self-check: the rotated (0.7123 rad) lobe differs from the "
						"unrotated one at this probe -- the parity check above is not vacuous" );
				}
				jobZ->release();
			}
		}
	}
	if( jobC ) jobC->release();
	if( jobS ) jobS->release();
}

int main()
{
	std::printf( "===== DL-16 GGX tangent_rotation_scalar test =====\n" );
	TestSharedPainterAndPreference();
	TestRenderParity();
	std::printf( "\n%d passed, %d failed\n", s_pass, s_fail );
	return s_fail == 0 ? 0 : 1;
}
