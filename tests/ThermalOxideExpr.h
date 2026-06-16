//////////////////////////////////////////////////////////////////////
//
//  ThermalOxideExpr.h - The thermal-oxide heat-tint fields as in-scene
//  math expressions (expression_function2d), the analogue of
//  GuillocheDialExpr.h for the dial relief.
//
//  ONE source of truth, templated on the "builder" type, consumed two
//  ways:
//    1. ThermalOxideExprTest compiles each Build* through the engine and
//       proves Eval == the retired GuillocheField oxide/thermal physics
//       (OxideDose / AbsoluteThicknessNm / SpallMask) to golden precision.
//    2. The watch scene + temper_comparison_gen.py author the same
//       expression strings as expression_function2d chunks consumed by
//       scalar_painter { function2d <name> scale S bias B } -> film_thickness
//       (and function2d_painter for the spall matte mask).
//
//  Nothing here is guilloché-specific: these are a radial heat falloff,
//  an Arrhenius warp, and a piecewise-linear metal-oxidation model -- the
//  general thermal-tint of any torched/annealed metal.  The Builder API
//  matches GuillocheDialExpr.h:
//      void AddParam( const std::string& name, double value );
//      bool AddDef  ( const std::string& name, const std::string& expr );
//  Build*(B&) populate params + defs and RETURN the final expr string.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef THERMAL_OXIDE_EXPR_
#define THERMAL_OXIDE_EXPR_

#include <string>

namespace ThermalOxideExpr
{
	// The radial heat falloff centre->rim in [0,1] (matches
	// GuillocheField::HeatAt): 0 linear, 1 quadratic (default), 2 smooth.
	inline std::string Heat( int falloff )
	{
		return falloff == 0 ? "rho"
		     : falloff == 2 ? "rho*rho*(3-2*rho)"
		     :                "rho*rho";
	}

	// rho = clamp(|p|/R, 0, 1) from the disk's linear Cartesian UV
	// (u = (x+R)/2R), so (2u-1)*R = x.  Shared preamble.
	template<class B>
	void AddRho( B& b, double R )
	{
		b.AddParam( "R", R );
		b.AddDef( "rho", "clamp(hypot((2*u-1)*R,(2*v-1)*R)/R,0,1)" );
	}

	//================================================================
	//  Normalized oxide DOSE in [0,1] -- the hero heat-tint shape.
	//  Reproduces GuillocheField::OxideDose with torchAmount = 0: a radial
	//  heat falloff Arrhenius-warped by the metal's activation energy Ea
	//  (J/mol).  Closed form: dose = (g(T)-g0)/(g1-g0), g(T)=exp(-Ea/2RT),
	//  T = 700 + heat*200 K, g0=g(700), g1=g(900), R = 8.314 J/mol/K.
	//  Consume via scalar_painter { function2d <name> scale S bias B }.
	//================================================================
	template<class B>
	std::string BuildOxideDose( B& b, double R, double Ea, int falloff )
	{
		AddRho( b, R );
		b.AddParam( "Ea", Ea );
		b.AddDef( "heat", Heat( falloff ) );
		b.AddDef( "Tk", "700+heat*200" );
		b.AddDef( "g",  "exp(-Ea/(2*8.314*Tk))" );
		b.AddDef( "g0", "exp(-Ea/(2*8.314*700))" );
		b.AddDef( "g1", "exp(-Ea/(2*8.314*900))" );
		return "clamp((g-g0)/(g1-g0),0,1)";
	}

	//================================================================
	//  Oxide dose WITH the signed torch term (ridges hotter/cooler):
	//  clamp(doseRaw + torch*petal, 0, 1), petal = the uniform-pattern petal
	//  lens TorchMask = smoothstep(petalE0,petalE1,|cos(arms*psi)|), psi =
	//  theta + swirl*rho + jag, jag = seamJag*triWave(seamJagFreq*rho).
	//  Reproduces GuillocheField::OxideDose with torchAmount != 0 (pattern
	//  uniform).  doseRaw is the UNclamped Arrhenius dose (the sum is clamped).
	//================================================================
	template<class B>
	std::string BuildOxideDoseTorch( B& b, double R, double Ea, int falloff, double torch,
		int numArms, double swirl, double seamJag, double seamJagFreq, double petalE0, double petalE1 )
	{
		AddRho( b, R );
		b.AddParam( "Ea", Ea );        b.AddParam( "torch", torch );
		b.AddParam( "arms", (double)numArms ); b.AddParam( "swirl", swirl );
		b.AddParam( "seamJag", seamJag ); b.AddParam( "seamJagFreq", seamJagFreq );
		b.AddParam( "petalE0", petalE0 ); b.AddParam( "petalE1", petalE1 );
		b.AddDef( "heat", Heat( falloff ) );
		b.AddDef( "Tk", "700+heat*200" );
		b.AddDef( "g",  "exp(-Ea/(2*8.314*Tk))" );
		b.AddDef( "g0", "exp(-Ea/(2*8.314*700))" );
		b.AddDef( "g1", "exp(-Ea/(2*8.314*900))" );
		b.AddDef( "doseRaw", "(g-g0)/(g1-g0)" );
		b.AddDef( "theta", "atan2((2*v-1)*R,(2*u-1)*R)" );
		b.AddDef( "jag", "seamJag*(2*abs(2*frac(seamJagFreq*rho)-1)-1)" );
		b.AddDef( "psi", "theta+swirl*rho+jag" );
		b.AddDef( "petal", "smoothstep(petalE0,petalE1,abs(cos(arms*psi)))" );
		return "clamp(doseRaw+torch*petal,0,1)";
	}

	//================================================================
	//  Absolute oxide thickness (nm) for an ABSOLUTE radial temperature
	//  ramp tC -> tR (deg C) -- reproduces GuillocheField::AbsoluteThicknessNm.
	//  Piecewise-LINEAR through the metal's temper anchors (onsetC, optLoC,
	//  optHiC, flakeC, dLoNm, dHiNm): 0 below onset, dLo..dHi across the
	//  optimal window, dHi->2*dHi to the flake temperature, then a 3.5*dHi
	//  desaturated-high-order cap.  Feed film_thickness directly.
	//================================================================
	template<class B>
	std::string BuildTemperThickness( B& b, double R, int falloff, double tC, double tR,
		double onsetC, double optLoC, double optHiC, double flakeC, double dLoNm, double dHiNm )
	{
		AddRho( b, R );
		b.AddParam( "tC", tC ); b.AddParam( "tR", tR );
		b.AddParam( "onsetC", onsetC ); b.AddParam( "optLoC", optLoC );
		b.AddParam( "optHiC", optHiC ); b.AddParam( "flakeC", flakeC );
		b.AddParam( "dLoNm", dLoNm );   b.AddParam( "dHiNm", dHiNm );
		b.AddDef( "heat", Heat( falloff ) );
		b.AddDef( "T", "tC+heat*(tR-tC)" );
		b.AddDef( "s1", "dLoNm*(T-onsetC)/max(1,optLoC-onsetC)" );
		b.AddDef( "s2", "dLoNm+(dHiNm-dLoNm)*(T-optLoC)/max(1,optHiC-optLoC)" );
		b.AddDef( "s3", "dHiNm+dHiNm*(T-optHiC)/max(1,flakeC-optHiC)" );
		b.AddDef( "s4", "min(2*dHiNm+dHiNm*(T-flakeC)/100,3.5*dHiNm)" );
		return "select(T<onsetC,0,select(T<optLoC,s1,select(T<=optHiC,s2,select(T<=flakeC,s3,s4))))";
	}

	//================================================================
	//  Spall fraction in [0,1] -- reproduces GuillocheField::SpallMask: a
	//  smoothstep through the metal's flake temperature (+/-22 C) on the
	//  same absolute radial ramp.  Drives the matte oxide-scale blend.
	//================================================================
	template<class B>
	std::string BuildSpall( B& b, double R, int falloff, double tC, double tR, double flakeC )
	{
		AddRho( b, R );
		b.AddParam( "tC", tC ); b.AddParam( "tR", tR ); b.AddParam( "flakeC", flakeC );
		b.AddDef( "heat", Heat( falloff ) );
		b.AddDef( "T", "tC+heat*(tR-tC)" );
		return "smoothstep(flakeC-22,flakeC+22,T)";
	}
}

#endif
