//////////////////////////////////////////////////////////////////////
//
//  ThermalOxideExprTest.cpp - proves the in-scene thermal-oxide
//  expressions (ThermalOxideExpr.h, authored as expression_function2d
//  in the watch scene + temper_comparison_gen.py) reproduce the retired
//  GuillocheField oxide/thermal physics to golden precision:
//      BuildOxideDose      == (former) GuillocheField::OxideDose (torch 0)
//      BuildTemperThickness== (former) GuillocheField::AbsoluteThicknessNm
//      BuildSpall          == (former) GuillocheField::SpallMask
//      BuildOxideDoseTorch == (former) GuillocheField::OxideDose (torch != 0)
//
//  This is the oxide analogue of ExpressionFunction2DTest (which did the
//  same for the dial relief).  SELF-CONTAINED: the live GuillocheField
//  physics has been excised, so the per-metal activation energies, the
//  per-metal temper anchors, and every golden field value below are
//  HARDCODED snapshots captured from the live physics before deletion
//  (the DumpGoldens path that produced them).  The test compiles each
//  ThermalOxideExpr builder through the expression engine and checks
//  Eval == the golden to the same tolerances the live comparison used
//  (dose / spall / torch 1e-9, thickness 1e-6).  Self-checking: a wrong
//  golden makes the test fail.
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <string>
#include "../src/Library/Painters/ExpressionEval.h"
#include "ThermalOxideExpr.h"

using namespace RISE;
using namespace RISE::Implementation;

static int passCount = 0;
static int failCount = 0;

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

static const Scalar kR = 20.6;
static const Scalar kPTS[8][2] = {
	{ 0.31, 0.17 }, { 3.2, 1.1 }, { -5.7, 8.3 }, { 12.4, -3.9 },
	{ -15.2, -9.8 }, { 0.0, 18.9 }, { 7.07, 7.07 }, { -19.5, 2.0 }
};

// Metals, in the order { ti, nb, ta, steel } (the metal0 chars 'T''N''a''S').
static const char* kMName[4] = { "ti", "nb", "ta", "steel" };

//================================================================
//  HARDCODED live-physics snapshots (captured from the retired
//  GuillocheField before excision).  See the file header.
//================================================================

// Per-metal parabolic-oxidation activation energy Ea (J/mol), ti/nb/ta/steel.
static const Scalar kEa[4] = { 160000, 135000, 80000, 165000 };

// Per-metal temper anchors: onsetC, optLoC, optHiC, flakeC, dLoNm, dHiNm.
struct TemperAnchors { Scalar onsetC, optLoC, optHiC, flakeC, dLoNm, dHiNm; };
static const TemperAnchors kThermal[4] = {
	{ 250, 300, 580, 650, 10, 73 },		// ti  / TiO2
	{ 200, 250, 520, 580, 12, 88 },		// nb  / Nb2O5
	{ 250, 300, 560, 620, 10, 94 },		// ta  / Ta2O5
	{ 210, 230, 350, 420, 11, 93 },		// steel / Fe3O4
};

// dose[4 metals][3 falloffs][8 pts] == OxideDose(torch 0).
static const Scalar kGoldenDose[4][3][8] = {
  {
    { 0.0034320354595420424, 0.042142182746946588, 0.21713357454758894, 0.35438875185679941, 0.72946671040341127, 0.80999502121310929, 0.21439520965884948, 0.8847570465342729 },
    { 5.7257386743369233e-05, 0.0054854518435760802, 0.069595415137325478, 0.15195829384649251, 0.54173417953147418, 0.66132481084882466, 0.068240254533261102, 0.78488367207328957 },
    { 0.00016997184546663181, 0.015813767417340432, 0.2126429994003762, 0.42898450152559769, 0.90158839710347149, 0.95274960569631384, 0.20860473799459078, 0.98313157218325553 },
  },
  {
    { 0.0047873910227352393, 0.056064374796071827, 0.25834317431172737, 0.40126048169797723, 0.75932477728149672, 0.83216113639804246, 0.25538934725752571, 0.8988305876265501 },
    { 8.028580154277842e-05, 0.0076283964144672383, 0.090291746604680839, 0.18660850020729047, 0.58462666134036845, 0.69676939059482246, 0.088633599528161036, 0.80956762823226514 },
    { 0.00023829066485996547, 0.021680519756971259, 0.25349675677087108, 0.47559243406814794, 0.91372294764621143, 0.95874124579341446, 0.24912740808220399, 0.98530459021847261 },
  },
  {
    { 0.0094586360727529888, 0.10006910562013588, 0.36647462560909982, 0.51505235823511941, 0.82311849604764376, 0.87860271218588304, 0.36315977803137861, 0.92782947183932463 },
    { 0.00016044514232707997, 0.014971503793190833, 0.15291543763091892, 0.28227569824714399, 0.68138149980543672, 0.77388351810225753, 0.1504600141567089, 0.86159098910827214 },
    { 0.00047601986664647859, 0.041256834573455033, 0.36102965998236702, 0.58513201159247319, 0.93863720164588549, 0.97091439387094425, 0.35609316855241446, 0.98969305282105779 },
  },
  {
    { 0.0032060250548291368, 0.03974538405118478, 0.2095185068750969, 0.34546241668064642, 0.72348922479629418, 0.80552320486959184, 0.20682550487849025, 0.88189917270418294 },
    { 5.3431107181824136e-05, 0.0051273563881605129, 0.065973604809177119, 0.14567950849062455, 0.53332801631366911, 0.65427918092500281, 0.064674068491706024, 0.77991544960997961 },
    { 0.00015861895299632695, 0.014823839481309874, 0.20510277878364794, 0.41999547280464494, 0.89912079987640536, 0.9515261378966593, 0.20113384279645907, 0.98268680136157038 },
  },
};

// thickness[4 metals][8 pts] == AbsoluteThicknessNm (falloff 1, 200->1000 C).
static const Scalar kGoldenThickness[4][8] = {
  { 0, 0, 30.502167970591024, 59.171693844848718, 255.5, 255.5, 29.904006032613825, 255.5 },
  { 0.056555754548023737, 5.1805071165991121, 51.722876802714701, 87.589279542526796, 308, 308, 50.974558987302892, 308 },
  { 0, 0, 39.439010419310186, 80.605509110552021, 329, 329, 38.580111226317285, 329 },
  { 0, 6.3719954755396335, 147.63184896920413, 277.64300122537469, 325.5, 325.5, 144.09984514495781, 325.5 },
};

// spall[4 metals][8 pts] == SpallMask (falloff 1, 200->1000 C).
static const Scalar kGoldenSpall[4][8] = {
  { 0, 0, 0, 0, 1, 1, 0, 1 },
  { 0, 0, 0, 0, 1, 1, 0, 1 },
  { 0, 0, 0, 0, 1, 1, 0, 1 },
  { 0, 0, 0, 1, 1, 1, 0, 1 },
};

// dosetorch[2 torches][8 pts] == OxideDose (Ti, falloff 1, uniform petal
// defaults, torch +0.40 then -0.40).
static const Scalar kGoldenDoseTorch[2][8] = {
  { 0.12505424471821838, 0.27873453844697782, 0.46127018888187332, 0.55195829384649253, 0.94173417953147409, 1, 0.068493911668546834, 1 },
  { 0, 0, 0, 0, 0.14173417953147405, 0.26132481084882464, 0.067986597397975371, 0.38488367207328955 },
};

// The uniform-pattern petal defaults the torch goldens were captured with
// (former GuillocheParams defaults): numArms 12, swirl 0, seamJag 0.16,
// seamJagFreq 3, petalE0 0, petalE1 0.82.
static const int    kTorchArms        = 12;
static const double kTorchSwirl       = 0.0;
static const double kTorchSeamJag     = 0.16;
static const double kTorchSeamJagFreq = 3.0;
static const double kTorchPetalE0     = 0.0;
static const double kTorchPetalE1     = 0.82;

static ExpressionProgram CompileDose( double Ea, int fo )
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.Finalize( ThermalOxideExpr::BuildOxideDose( b, (double)kR, Ea, fo ), p );
	return p;
}
static ExpressionProgram CompileThickness( int fo, double tC, double tR, const TemperAnchors& m )
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.Finalize( ThermalOxideExpr::BuildTemperThickness( b, (double)kR, fo, tC, tR,
		m.onsetC, m.optLoC, m.optHiC, m.flakeC, m.dLoNm, m.dHiNm ), p );
	return p;
}
static ExpressionProgram CompileSpall( int fo, double tC, double tR, double flakeC )
{
	ExpressionProgram p = ExpressionProgram::Invalid();
	ExpressionProgram::Builder b;
	b.Finalize( ThermalOxideExpr::BuildSpall( b, (double)kR, fo, tC, tR, flakeC ), p );
	return p;
}

int main( int, char** )
{
	std::cout << "ThermalOxideExprTest -- oxide heat-tint AS SCENE EXPRESSIONS == captured GuillocheField physics" << std::endl << std::endl;

	// ---- (1) DOSE == OxideDose (torch 0), per metal, falloffs 0/1/2 ----
	std::cout << "Test 1: BuildOxideDose == OxideDose golden (4 metals x 3 falloffs)" << std::endl;
	for( int mi = 0; mi < 4; ++mi ) {
		const Scalar Ea = kEa[mi];
		for( int fo = 0; fo < 3; ++fo ) {
			const ExpressionProgram prog = CompileDose( Ea, fo );
			char label[96]; snprintf( label, sizeof(label), "dose %s falloff%d compiles", kMName[mi], fo );
			if( !prog.IsValid() ) { ++failCount; std::cout << "  FAIL: " << label << std::endl; continue; }
			for( int i = 0; i < 8; ++i ) {
				const Scalar x = kPTS[i][0], y = kPTS[i][1];
				const Scalar u = ( x + kR ) / ( 2 * kR ), v = ( y + kR ) / ( 2 * kR );
				snprintf( label, sizeof(label), "dose %s fo%d pt%d", kMName[mi], fo, i );
				CheckClose( prog.Eval( u, v ), kGoldenDose[mi][fo][i], Scalar(1e-9), label );
			}
		}
	}

	// ---- (2) THICKNESS_NM == AbsoluteThicknessNm, per metal (falloff 1, 200->1000 C) ----
	std::cout << "Test 2: BuildTemperThickness == AbsoluteThicknessNm golden (4 metals)" << std::endl;
	const Scalar tC = 200.0, tR = 1000.0; const int fo = 1;
	for( int mi = 0; mi < 4; ++mi ) {
		const TemperAnchors& m = kThermal[mi];
		const ExpressionProgram prog = CompileThickness( fo, tC, tR, m );
		char label[96]; snprintf( label, sizeof(label), "thickness %s compiles", kMName[mi] );
		if( !prog.IsValid() ) { ++failCount; std::cout << "  FAIL: " << label << std::endl; continue; }
		for( int i = 0; i < 8; ++i ) {
			const Scalar x = kPTS[i][0], y = kPTS[i][1];
			const Scalar u = ( x + kR ) / ( 2 * kR ), v = ( y + kR ) / ( 2 * kR );
			snprintf( label, sizeof(label), "thickness %s pt%d", kMName[mi], i );
			CheckClose( prog.Eval( u, v ), kGoldenThickness[mi][i], Scalar(1e-6), label );
		}
	}

	// ---- (3) SPALL_MASK == SpallMask, per metal ----
	std::cout << "Test 3: BuildSpall == SpallMask golden (4 metals)" << std::endl;
	for( int mi = 0; mi < 4; ++mi ) {
		const TemperAnchors& m = kThermal[mi];
		const ExpressionProgram prog = CompileSpall( fo, tC, tR, m.flakeC );
		char label[96]; snprintf( label, sizeof(label), "spall %s compiles", kMName[mi] );
		if( !prog.IsValid() ) { ++failCount; std::cout << "  FAIL: " << label << std::endl; continue; }
		for( int i = 0; i < 8; ++i ) {
			const Scalar x = kPTS[i][0], y = kPTS[i][1];
			const Scalar u = ( x + kR ) / ( 2 * kR ), v = ( y + kR ) / ( 2 * kR );
			snprintf( label, sizeof(label), "spall %s pt%d", kMName[mi], i );
			CheckClose( prog.Eval( u, v ), kGoldenSpall[mi][i], Scalar(1e-9), label );
		}
	}

	// ---- (4) DOSE WITH TORCH == OxideDose(torchAmount), pattern uniform defaults ----
	std::cout << "Test 4: BuildOxideDoseTorch == OxideDose golden (torch +/-0.40, uniform petal)" << std::endl;
	const Scalar EaTi = kEa[0];
	const double torches[2] = { 0.40, -0.40 };
	for( int ti = 0; ti < 2; ++ti ) {
		ExpressionProgram prog = ExpressionProgram::Invalid();
		ExpressionProgram::Builder b;
		b.Finalize( ThermalOxideExpr::BuildOxideDoseTorch( b, (double)kR, EaTi, 1, torches[ti],
			kTorchArms, kTorchSwirl, kTorchSeamJag, kTorchSeamJagFreq, kTorchPetalE0, kTorchPetalE1 ), prog );
		char label[96]; snprintf( label, sizeof(label), "dose torch %.2f compiles", torches[ti] );
		if( !prog.IsValid() ) { ++failCount; std::cout << "  FAIL: " << label << std::endl; continue; }
		for( int i = 0; i < 8; ++i ) {
			const Scalar x = kPTS[i][0], y = kPTS[i][1];
			const Scalar u = ( x + kR ) / ( 2 * kR ), v = ( y + kR ) / ( 2 * kR );
			snprintf( label, sizeof(label), "dose torch %.2f pt%d", torches[ti], i );
			CheckClose( prog.Eval( u, v ), kGoldenDoseTorch[ti][i], Scalar(1e-9), label );
		}
	}

	std::cout << std::endl << "Results: " << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount > 0 ? 1 : 0;
}
