//////////////////////////////////////////////////////////////////////
//
//  GuillocheFieldTest.cpp - Fidelity tests for the pointwise C++ port
//  of the Python guilloché bakers (dial_mesh_gen.py /
//  dial_variants_gen.py / thermal_oxide_sim.py).  Golden values were
//  generated from the Python sources at the gen_dials.sh blessed
//  parameter sets; the production grid-normalized bake agrees with
//  the analytic normalization used here to <= 1.7e-8 across all six
//  patterns (verified 2026-06-11), so a 1e-6 tolerance is decisive.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include "../src/Library/Painters/GuillochePainter.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

static void Check( bool cond, const char* name )
{
	if( cond ) { ++passCount; }
	else { ++failCount; std::cout << "  FAIL: " << name << std::endl; }
}

static void CheckClose( Scalar got, Scalar want, Scalar tol, const char* name )
{
	if( std::fabs( got - want ) <= tol ) { ++passCount; }
	else {
		++failCount;
		std::cout.precision( 12 );
		std::cout << "  FAIL: " << name << "  got " << got << "  want " << want
			<< "  |d| " << std::fabs( got - want ) << std::endl;
	}
}

// The shared sample points (world dial coordinates, R = 20.6).
static const Scalar PTS[8][2] = {
	{ 0.31, 0.17 }, { 3.2, 1.1 }, { -5.7, 8.3 }, { 12.4, -3.9 },
	{ -15.2, -9.8 }, { 0.0, 18.9 }, { 7.07, 7.07 }, { -19.5, 2.0 }
};

// gen_dials.sh blessed configurations expressed through the public descriptor.
static GuillocheDiskDescriptor DescUniform()
{
	return GuillocheDiskDescriptor();
}
static GuillocheDiskDescriptor DescLightning()
{
	GuillocheDiskDescriptor d;
	d.pattern = eGuillochePatternLightning;
	d.numArms = 11; d.cellMode = 1;
	d.lightningRelief = 0.6; d.lightningLo = 0.45; d.lightningHi = 0.65;
	d.centerRadius = 0.015; d.zigzagAmp = 0.16; d.zigzagFreq = 3.0;
	d.boltStyle = 0; d.rungLen = 0.65; d.rungWidth = 0.82;
	d.fieldCell = 0.50; d.fieldFrame = 0;
	return d;
}
static void TestOxideDoseGoldens()
{
	std::cout << "Test 1: OxideDose radial profile vs thermal_oxide_sim (4 metals + 3 falloffs)" << std::endl;
	// dose(rho) at rho in {0, 0.35, 0.7, 1}, torch 0.  Golden values are the
	// formula-exact doubles (verified == build_thickness_profile to 2.2e-16).
	struct Row { const char* name; int falloff; Scalar ea; Scalar want[4]; };
	const Row rows[] = {
		{ "Ti quadratic",    1, 160.0e3, { 0.0, 0.029273880, 0.218122733, 1.0 } },
		{ "Nb quadratic",    1, 135.0e3, { 0.0, 0.039484976, 0.259408995, 1.0 } },
		{ "Ta quadratic",    1,  80.0e3, { 0.0, 0.072561267, 0.367667834, 1.0 } },
		{ "Steel quadratic", 1, 165.0e3, { 0.0, 0.027531936, 0.210491492, 1.0 } },
		{ "Ti smooth",       2, 160.0e3, { 0.0, 0.088284095, 0.562679125, 1.0 } },
		{ "Ti linear",       0, 160.0e3, { 0.0, 0.123133150, 0.439531652, 1.0 } },
	};
	const Scalar rhos[4] = { 0.0, 0.35, 0.7, 1.0 };
	const GuillocheDiskDescriptor d;	// defaults; pattern irrelevant at torch 0
	const GuillocheField field( GuillocheParamsFromDescriptor( d ) );
	char label[128];
	for( size_t r = 0; r < sizeof(rows)/sizeof(rows[0]); ++r ) {
		for( int i = 0; i < 4; ++i ) {
			// evaluate on the +x axis at rho * R
			const Scalar x = rhos[i] * d.radius;
			snprintf( label, sizeof(label), "%s rho=%.2f", rows[r].name, (double)rhos[i] );
			CheckClose( field.OxideDose( x, 0.0, rows[r].falloff, rows[r].ea, 0.0 ),
				rows[r].want[i], Scalar(1e-6), label );
		}
	}
	// MetalEa presets
	CheckClose( GuillocheField::MetalEa('T'), 160.0e3, 1e-9, "MetalEa Ti" );
	CheckClose( GuillocheField::MetalEa('N'), 135.0e3, 1e-9, "MetalEa Nb" );
	CheckClose( GuillocheField::MetalEa('a'),  80.0e3, 1e-9, "MetalEa Ta" );
	CheckClose( GuillocheField::MetalEa('S'), 165.0e3, 1e-9, "MetalEa Steel" );
}

static void TestTorchMaskGoldens()
{
	std::cout << "Test 2: TorchMask vs dial_mesh_gen.lightning_mask (12-arm petal) + lightning zeros" << std::endl;
	// Default (uniform) pattern mask == the production petal mask that bakes
	// the oxide hot/cool variants.  Goldens from lightning_mask's math.
	{
		const GuillocheField field( GuillocheParamsFromDescriptor( DescUniform() ) );
		CheckClose( field.TorchMask( 3.2, 1.1 ),    Scalar(0.683122717), Scalar(1e-6), "petal mask pt0" );
		CheckClose( field.TorchMask( -5.7, 8.3 ),   Scalar(0.979186934), Scalar(1e-6), "petal mask pt1" );
		CheckClose( field.TorchMask( 12.4, -3.9 ),  Scalar(1.0),         Scalar(1e-6), "petal mask pt2" );
	}
	// Blessed lightning mask: those three points are off-bolt -> exactly 0.
	{
		const GuillocheField field( GuillocheParamsFromDescriptor( DescLightning() ) );
		CheckClose( field.TorchMask( 3.2, 1.1 ),   0.0, Scalar(1e-9), "lightning mask pt0" );
		CheckClose( field.TorchMask( -5.7, 8.3 ),  0.0, Scalar(1e-9), "lightning mask pt1" );
		CheckClose( field.TorchMask( 12.4, -3.9 ), 0.0, Scalar(1e-9), "lightning mask pt2" );
		// and it is bounded + non-degenerate somewhere on the dial
		bool inBounds = true;
		Scalar maxSeen = 0.0;
		for( int i = 0; i < 64; ++i ) {
			for( int j = 0; j < 64; ++j ) {
				const Scalar x = -20.0 + 40.0 * i / 63.0;
				const Scalar y = -20.0 + 40.0 * j / 63.0;
				const Scalar m = field.TorchMask( x, y );
				if( m < 0.0 || m > 1.0 ) inBounds = false;
				if( m > maxSeen ) maxSeen = m;
			}
		}
		Check( inBounds, "lightning mask bounded [0,1]" );
		Check( maxSeen > Scalar(0.5), "lightning mask fires on the bolts" );
	}
}

static void TestOxidePainterUVMapping()
{
	std::cout << "Test 3: GuillocheOxidePainter::Evaluate UV == OxideDose(world)" << std::endl;
	const GuillocheDiskDescriptor d = DescUniform();
	const GuillocheParams p = GuillocheParamsFromDescriptor( d );
	const GuillocheField field( p );
	GuillocheOxidePainter* painter = new GuillocheOxidePainter( p, 1, 160.0e3, 0.40 );
	char label[64];
	for( int i = 0; i < 8; ++i ) {
		const Scalar x = PTS[i][0], y = PTS[i][1];
		const Scalar u = ( x + d.radius ) / ( 2.0 * d.radius );
		const Scalar v = ( y + d.radius ) / ( 2.0 * d.radius );
		snprintf( label, sizeof(label), "painter uv pt%d", i );
		CheckClose( painter->Evaluate( u, v ),
			field.OxideDose( x, y, 1, 160.0e3, 0.40 ), Scalar(1e-12), label );
	}
	// torch term shifts the dose where the mask fires, clipped to [0,1]
	const Scalar base = field.OxideDose( 12.4, -3.9, 1, 160.0e3, 0.0 );
	const Scalar hot  = field.OxideDose( 12.4, -3.9, 1, 160.0e3, 0.40 );
	CheckClose( hot - base, Scalar(0.40), Scalar(1e-6), "torch +0.40 at mask=1" );	// mask=1 there (Test 3)
	painter->release();
}

// The absolute-temperature temper model (the comparison renders): a flat
// ramp tempCenter == tempRim == T makes T(rho) constant, so the model
// evaluates at exactly T regardless of radius.  Goldens from the piecewise
// MetalThermalModel map (matches the Python de-risk).
static void TestThermalModelGoldens()
{
	std::cout << "Test 4: absolute-temperature thermal model (thickness nm + spall) vs goldens" << std::endl;
	const GuillocheField field( GuillocheParamsFromDescriptor( GuillocheDiskDescriptor() ) );
	struct Row { char m; const char* name; Scalar T; Scalar d; Scalar spall; };
	const Row rows[] = {
		{ 'T', "Ti",    150, 0.0,        0.0 },
		{ 'T', "Ti",    300, 10.0,       0.0 },
		{ 'T', "Ti",    440, 41.5,       0.0 },
		{ 'T', "Ti",    580, 73.0,       0.0 },
		{ 'T', "Ti",    650, 146.0,      0.5 },
		{ 'T', "Ti",    900, 255.5,      1.0 },
		{ 'N', "Nb",    250, 12.0,       0.0 },
		{ 'N', "Nb",    580, 176.0,      0.5 },
		{ 'a', "Ta",    440, 55.230769,  0.0 },
		{ 'a', "Ta",    650, 216.2,      1.0 },
		{ 'S', "Steel", 250, 24.666667,  0.0 },
		{ 'S', "Steel", 440, 204.6,      0.993989 },
	};
	char label[80];
	for( size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); ++i ) {
		const GuillocheField::MetalThermal mt = GuillocheField::MetalThermalModel( rows[i].m );
		snprintf( label, sizeof(label), "%s thickness @ %dC", rows[i].name, (int)rows[i].T );
		CheckClose( field.AbsoluteThicknessNm( 0, 0, 0, rows[i].T, rows[i].T, mt ), rows[i].d, Scalar(1e-4), label );
		snprintf( label, sizeof(label), "%s spall @ %dC", rows[i].name, (int)rows[i].T );
		CheckClose( field.SpallMask( 0, 0, 0, rows[i].T, rows[i].T, mt ), rows[i].spall, Scalar(1e-4), label );
	}
	// preset spot-checks
	CheckClose( GuillocheField::MetalThermalModel('T').flakeC, 650, Scalar(1e-9), "Ti flakeC preset" );
	CheckClose( GuillocheField::MetalThermalModel('S').optHiC, 350, Scalar(1e-9), "Steel optHiC preset" );
	CheckClose( GuillocheField::MetalThermalModel('a').dHiNm,  94,  Scalar(1e-9), "Ta dHiNm preset" );
}

int main( int, char** )
{
	std::cout << "GuillocheFieldTest -- C++ field port vs Python baker goldens" << std::endl << std::endl;
	TestOxideDoseGoldens();
	TestTorchMaskGoldens();
	TestOxidePainterUVMapping();
	TestThermalModelGoldens();
	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
