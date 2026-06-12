//////////////////////////////////////////////////////////////////////
//
//  GuillocheDialExpr.h - the six guilloché dial patterns authored as
//  in-scene math expressions (ExpressionEval / expression_function2d).
//
//  ONE source of truth, templated on the "builder" type, consumed two
//  ways:
//    * ExpressionFunction2DTest compiles them via ExpressionProgram::
//      Builder and proves Eval == the retired GuillocheField::Height.
//    * the scene-chunk emitter (tests/_emit harness) replays them through
//      a recorder to print the exact `expression_function2d` chunk text
//      pasted into watch_dial.RISEscene -- so the rendered dial provably
//      uses the same expression the test validates.
//
//  A builder B must provide:
//      void AddParam( const std::string& name, double value );
//      bool AddDef  ( const std::string& name, const std::string& expr );
//  Build*(B&) populate params + defs and RETURN the final expr string
//  (the caller Finalizes / emits it).
//
//  Tabs: 4
//
//////////////////////////////////////////////////////////////////////

#ifndef GUILLOCHE_DIAL_EXPR_H_
#define GUILLOCHE_DIAL_EXPR_H_

#include <string>

namespace GuillocheDialExpr
{
	// A periodic groove/land stripe: smoothstep(gridE0,gridE1,|cos(2pi*arg)|).
	inline std::string Stripe( const std::string& arg )
	{
		return "smoothstep(gridE0,gridE1,abs(cos(tau*(" + arg + "))))";
	}

	// The brick-offset woven grid (GuillocheField::Woven) at frequency
	// `freq`, in the rotated-sector frame (xr, yr).  Adds suffixed defs and
	// returns the grid def name.
	template<class B>
	std::string AddWoven( B& b, const std::string& suf, const std::string& freq )
	{
		b.AddDef( "wax" + suf, "(" + freq + ")*xr" );
		b.AddDef( "way0" + suf, "(" + freq + ")*yr" );
		b.AddDef( "wrp" + suf, "floor(2*wax" + suf + ")" );
		b.AddDef( "way" + suf, "way0" + suf + "+0.25*mod(wrp" + suf + ",2)" );
		b.AddDef( "wg" + suf, Stripe( "wax" + suf ) + "*" + Stripe( "way" + suf ) );
		return "wg" + suf;
	}

	// The general radial Frame (GuillocheField::Frame): petal lens + the
	// rotated-sector axes (xr, yr).  Needs x,y,r,rho,theta + params
	// arms,swirl,seamJag,seamJagFreq,petalE0,petalE1.
	template<class B>
	void AddFrameGeneral( B& b )
	{
		b.AddDef( "jag", "seamJag*(2*abs(2*frac(seamJagFreq*rho)-1)-1)" );
		b.AddDef( "psi", "theta+swirl*rho+jag" );
		b.AddDef( "petal", "smoothstep(petalE0,petalE1,abs(cos(arms*psi)))" );
		b.AddDef( "wsec", "tau/arms" );
		b.AddDef( "q", "psi/wsec" );
		b.AddDef( "sector", "sign(q)*floor(abs(q)+0.5)" );
		b.AddDef( "thetaC", "sector*wsec-swirl*rho-jag" );
		b.AddDef( "cc", "cos(thetaC)" );
		b.AddDef( "ss", "sin(thetaC)" );
		b.AddDef( "xr", "cc*x+ss*y" );
		b.AddDef( "yr", "-ss*x+cc*y" );
	}

	// GuillocheField::FinishWithBounds: gamma squeeze + relief depth + flush
	// hub, normalizing `raw` into [mn, mx].  Needs params landLevel,
	// reliefDepth, centerRadius, R and defs r, raw.  Returns the final expr.
	template<class B>
	std::string AddFinish( B& b, double mn, double mx )
	{
		char smn[64], srng[64];
		snprintf( smn, sizeof(smn), "%.17g", mn );
		snprintf( srng, sizeof(srng), "%.17g", mx - mn );
		b.AddDef( "h0", std::string("clamp((raw-(") + smn + "))/(" + srng + "),0,1)" );
		b.AddDef( "h1", "pow(h0,log(landLevel)/log(0.5))" );
		b.AddDef( "h2", "0.5+(h1-0.5)*reliefDepth" );
		b.AddDef( "hub", "0.5*(1-reliefDepth)" );
		b.AddDef( "rin", "max(centerRadius*R,0.000001)" );
		b.AddDef( "whub", "clamp(r/rin,0,1)" );
		b.AddDef( "hfin", "(1-whub*whub)*hub+whub*whub*h2" );
		return "clamp(hfin,0,1)";
	}

	// Common preamble: dial (u,v) -> dial-space (x,y), r, rho, theta.
	template<class B>
	void AddPreamble( B& b )
	{
		b.AddDef( "x", "(2*u-1)*R" );
		b.AddDef( "y", "(2*v-1)*R" );
		b.AddDef( "r", "hypot(x,y)" );
		b.AddDef( "rho", "clamp(r/R,0,1)" );
		b.AddDef( "theta", "atan2(y,x)" );
	}

	//================================================================
	//  The six dial-pattern scene expressions.  Each returns the final
	//  expr string; the blessed params match watch_dial.RISEscene.
	//================================================================

	template<class B>
	std::string BuildUniform( B& b )
	{
		b.AddParam( "R", 20.6 ); b.AddParam( "arms", 12 ); b.AddParam( "swirl", 0 );
		b.AddParam( "seamJag", 0.16 ); b.AddParam( "seamJagFreq", 3.0 );
		b.AddParam( "cell", 0.9 ); b.AddParam( "gridAmp", 0.85 ); b.AddParam( "petalAmp", 0.30 );
		b.AddParam( "gridE0", 0.12 ); b.AddParam( "gridE1", 0.5 );
		b.AddParam( "petalE0", 0.0 ); b.AddParam( "petalE1", 0.82 );
		b.AddParam( "base", 0.15 ); b.AddParam( "landLevel", 0.45 );
		b.AddParam( "reliefDepth", 0.85 ); b.AddParam( "centerRadius", 0.03 );
		AddPreamble( b );
		AddFrameGeneral( b );
		const std::string g = AddWoven( b, "U", "0.5/cell" );
		b.AddDef( "raw", "base+petalAmp*petal+gridAmp*" + g );
		return AddFinish( b, 0.15, 0.15 + 0.30 + 0.85 );
	}

	template<class B>
	std::string BuildRadial( B& b )
	{
		b.AddParam( "R", 20.6 ); b.AddParam( "arms", 12 ); b.AddParam( "swirl", 0 );
		b.AddParam( "seamJag", 0.16 ); b.AddParam( "seamJagFreq", 3.0 );
		b.AddParam( "cell", 0.9 ); b.AddParam( "gridAmp", 0.85 ); b.AddParam( "petalAmp", 0.30 );
		b.AddParam( "gridE0", 0.12 ); b.AddParam( "gridE1", 0.5 );
		b.AddParam( "petalE0", 0.0 ); b.AddParam( "petalE1", 0.82 );
		b.AddParam( "base", 0.15 ); b.AddParam( "landLevel", 0.45 );
		b.AddParam( "reliefDepth", 0.85 ); b.AddParam( "centerRadius", 0.03 );
		b.AddParam( "lightningLo", 0.25 ); b.AddParam( "lightningHi", 0.65 );
		b.AddParam( "cellScale", 1.8 );
		AddPreamble( b );
		AddFrameGeneral( b );
		b.AddDef( "mask", "smoothstep(lightningLo,lightningHi,petal)" );
		const std::string gF = AddWoven( b, "F", "0.5/cell" );
		const std::string gB = AddWoven( b, "B", "0.5/(cell*cellScale)" );
		b.AddDef( "grid", gF + "*(1-mask)+" + gB + "*mask" );
		b.AddDef( "raw", "base+petalAmp*petal+gridAmp*grid" );
		return AddFinish( b, 0.15, 0.15 + 0.30 + 0.85 );
	}

	template<class B>
	std::string BuildLightning( B& b )
	{
		b.AddParam( "R", 20.6 ); b.AddParam( "arms", 11 );
		b.AddParam( "cell", 0.9 ); b.AddParam( "gridAmp", 0.85 ); b.AddParam( "petalAmp", 0.30 );
		b.AddParam( "gridE0", 0.12 ); b.AddParam( "gridE1", 0.5 );
		b.AddParam( "base", 0.15 ); b.AddParam( "landLevel", 0.45 );
		b.AddParam( "reliefDepth", 0.85 ); b.AddParam( "centerRadius", 0.015 );
		b.AddParam( "lightningLo", 0.45 ); b.AddParam( "lightningHi", 0.65 );
		b.AddParam( "lightningRelief", 0.6 ); b.AddParam( "cellScale", 0.6 );
		b.AddParam( "zigzagAmp", 0.16 ); b.AddParam( "zigzagFreq", 3.0 );
		b.AddParam( "fieldCell", 0.50 ); b.AddParam( "rungLen", 0.65 ); b.AddParam( "rungWidth", 0.82 );
		AddPreamble( b );
		b.AddDef( "zig", "zigzagAmp*(2*abs(2*frac(zigzagFreq*rho)-1)-1)" );
		b.AddDef( "rayc", "0.5+0.5*cos(arms*(theta+zig))" );
		b.AddDef( "mask", "smoothstep(lightningLo,lightningHi,rayc)" );
		b.AddDef( "wsec", "tau/arms" );
		b.AddDef( "q", "theta/wsec" );
		b.AddDef( "sector", "sign(q)*floor(abs(q)+0.5)" );
		b.AddDef( "thetaC", "sector*wsec" );
		b.AddDef( "cc", "cos(thetaC)" );
		b.AddDef( "ss", "sin(thetaC)" );
		b.AddDef( "xr", "cc*x+ss*y" );
		b.AddDef( "yr", "-ss*x+cc*y" );
		b.AddDef( "inside", Stripe( "(0.5/fieldCell)*x" ) + "*" + Stripe( "(0.5/fieldCell)*y" ) );
		b.AddDef( "between", Stripe( "(0.5/rungLen)*xr" ) + "*" + Stripe( "(0.5/rungWidth)*yr" ) );
		b.AddDef( "grid", "between*(1-mask)+inside*mask" );
		b.AddDef( "raw", "base+petalAmp*mask+gridAmp*grid+lightningRelief*mask" );
		return AddFinish( b, 0.15, 0.15 + 0.30 + 0.85 + 0.6 );
	}

	template<class B>
	std::string BuildIris( B& b )
	{
		b.AddParam( "R", 20.6 ); b.AddParam( "arms", 8 );
		b.AddParam( "cell", 0.8 ); b.AddParam( "gridAmp", 0.95 ); b.AddParam( "petalAmp", 0.30 );
		b.AddParam( "gridE0", 0.12 ); b.AddParam( "gridE1", 0.5 );
		b.AddParam( "base", 0.15 ); b.AddParam( "landLevel", 0.45 );
		b.AddParam( "reliefDepth", 0.85 ); b.AddParam( "centerRadius", 0.0 );
		b.AddParam( "irisAperture", 0.13 ); b.AddParam( "irisSwirl", 0.6 ); b.AddParam( "irisEdge", 0.6 );
		AddPreamble( b );
		b.AddDef( "a", "irisAperture*R" );
		for( int k = 0; k < 8; ++k ) {
			const std::string K = std::to_string( k );
			b.AddDef( "tk" + K, "tau*" + K + "/8+irisSwirl*rho" );
			b.AddDef( "ck" + K, "cos(tk" + K + ")" );
			b.AddDef( "sk" + K, "sin(tk" + K + ")" );
			b.AddDef( "d" + K, "(x*ck" + K + "+y*sk" + K + ")-a" );
		}
		b.AddDef( "ed0", "abs(d0)" );
		for( int k = 1; k < 8; ++k ) {
			const std::string K = std::to_string( k ), P = std::to_string( k - 1 );
			b.AddDef( "ed" + K, "min(ed" + P + ",abs(d" + K + "))" );
		}
		b.AddDef( "edgeD", "ed7" );
		b.AddDef( "bd0", "d0" ); b.AddDef( "bc0", "ck0" ); b.AddDef( "bs0", "sk0" );
		for( int k = 1; k < 8; ++k ) {
			const std::string K = std::to_string( k ), P = std::to_string( k - 1 );
			b.AddDef( "bd" + K, "max(bd" + P + ",d" + K + ")" );
			b.AddDef( "bc" + K, "select(d" + K + ">bd" + P + ",ck" + K + ",bc" + P + ")" );
			b.AddDef( "bs" + K, "select(d" + K + ">bd" + P + ",sk" + K + ",bs" + P + ")" );
		}
		b.AddDef( "ownck", "bc7" ); b.AddDef( "ownsk", "bs7" );
		b.AddDef( "groove", "smoothstep(0,irisEdge,edgeD)" );
		b.AddDef( "along", "x*(-ownsk)+y*ownck" );
		b.AddDef( "across", "x*ownck+y*ownsk" );
		b.AddDef( "cube", Stripe( "(0.5/cell)*along" ) + "*" + Stripe( "(0.5/cell)*across" ) );
		b.AddDef( "raw", "base+petalAmp*groove+gridAmp*cube*groove" );
		return AddFinish( b, 0.15, 0.15 + 0.30 + 0.95 );
	}

	template<class B>
	std::string BuildSwirl( B& b )
	{
		b.AddParam( "R", 20.6 ); b.AddParam( "arms", 6 );
		b.AddParam( "cell", 0.8 ); b.AddParam( "gridAmp", 0.95 );
		b.AddParam( "gridE0", 0.12 ); b.AddParam( "gridE1", 0.5 );
		b.AddParam( "base", 0.15 ); b.AddParam( "landLevel", 0.45 );
		b.AddParam( "reliefDepth", 0.85 ); b.AddParam( "centerRadius", 0.02 );
		b.AddParam( "swirlTurns", 7.0 );
		AddPreamble( b );
		b.AddDef( "lr", "log(max(r,0.001))" );
		b.AddDef( "su", "(arms*theta+swirlTurns*lr)/tau" );
		b.AddDef( "sv", "r/cell" );
		b.AddDef( "grid", Stripe( "su" ) + "*" + Stripe( "sv" ) );
		b.AddDef( "raw", "base+gridAmp*grid" );
		return AddFinish( b, 0.15, 0.15 + 0.95 );
	}

	template<class B>
	std::string BuildVarwidth( B& b )
	{
		b.AddParam( "R", 20.6 ); b.AddParam( "arms", 8 );
		b.AddParam( "cell", 0.6 ); b.AddParam( "gridAmp", 0.95 );
		b.AddParam( "gridE0", 0.12 ); b.AddParam( "gridE1", 0.5 );
		b.AddParam( "base", 0.15 ); b.AddParam( "landLevel", 0.45 );
		b.AddParam( "reliefDepth", 0.85 ); b.AddParam( "centerRadius", 0.02 );
		b.AddParam( "cellScale", 2.6 );
		AddPreamble( b );
		b.AddDef( "wsec", "tau/arms" );
		b.AddDef( "q", "theta/wsec" );
		b.AddDef( "sector", "sign(q)*floor(abs(q)+0.5)" );
		b.AddDef( "tc", "sector*wsec" );
		b.AddDef( "cc", "cos(tc)" );
		b.AddDef( "ss", "sin(tc)" );
		b.AddDef( "xr", "cc*x+ss*y" );
		b.AddDef( "yr", "-ss*x+cc*y" );
		b.AddDef( "cellLocal", "select(mod(sector,2)>0.5,cell*cellScale,cell)" );
		b.AddDef( "fr", "0.5/cellLocal" );
		b.AddDef( "grid", Stripe( "fr*xr" ) + "*" + Stripe( "fr*yr" ) );
		b.AddDef( "raw", "base+gridAmp*grid" );
		return AddFinish( b, 0.15, 0.15 + 0.95 );
	}
}

#endif
