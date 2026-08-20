//////////////////////////////////////////////////////////////////////
//
//  ExpressionPainter.cpp - Implementation of ExpressionPainter (colour
//  pipe) and ExpressionScalarPainter (physical-scalar pipe) -- S2 of
//  doc 88.  See ExpressionPainter.h for the design rationale.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ExpressionPainter.h"
#include "../Animation/KeyframableHelper.h"
#include "../Utilities/Color/RGBSpectra.h"
#include <cstdlib>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// ExpressionPainter
//////////////////////////////////////////////////////////////////////

ExprEvalContext ExpressionPainter::BuildContext( const RayIntersectionGeometric& ri ) const
{
	ExprEvalContext ctx;
	ctx.u = ri.ptCoord.x; ctx.v = ri.ptCoord.y;
	ctx.P  = Vector3( ri.ptIntersection.x, ri.ptIntersection.y, ri.ptIntersection.z );
	ctx.Po = Vector3( ri.ptObjIntersec.x, ri.ptObjIntersec.y, ri.ptObjIntersec.z );
	ctx.N  = ri.vNormal;
	ctx.fw = Scalar(0);		// Phase-2 footprint plumbing not landed yet (doc 88 decision 3)
	ctx.time = m_time;
	return ctx;
}

RISEPel ExpressionPainter::EvalRGB( const RayIntersectionGeometric& ri ) const
{
	const ExprEvalContext ctx = BuildContext( ri );
	const Vector3 v = m_prog.EvalVec3( ctx );	// scalar-typed programs broadcast to (s,s,s)
	return RISEPel( SafeComp( v.x ), SafeComp( v.y ), SafeComp( v.z ) );
}

RISEPel ExpressionPainter::GetColor( const RayIntersectionGeometric& ri ) const
{
	return EvalRGB( ri );
}

Scalar ExpressionPainter::GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Per-sample uplift -- same route TexturePainter::GetColorNM uses (see
	// its header comment for why sample-side uplift, not load-time): a
	// spatially-varying field's average reflectance is only correct if we
	// filter in RGB first and uplift last.
	const RISEPel rgb = EvalRGB( ri );
	const RGBToSpectrumTable& table = RGBToSpectrumTable::Get();

	if( m_kind == eSpectrumKind_Unbounded ) {
		return RGBUnboundedSpectrum::FromRGB( rgb, table ).Eval( nm );
	}
	if( m_kind == eSpectrumKind_Illuminant ) {
		return RGBIlluminantSpectrum::FromRGB( rgb, table ).Eval( nm );
	}
	return RGBAlbedoSpectrum::FromRGB( rgb, table ).Eval( nm );
}

SpectralPacket ExpressionPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	const Scalar lambda_begin = Scalar(380);
	const Scalar lambda_end   = Scalar(780);
	const unsigned int nbins  = 81;
	SpectralPacket sp( lambda_begin, lambda_end, nbins );

	const RISEPel rgb = EvalRGB( ri );
	const RGBToSpectrumTable& table = RGBToSpectrumTable::Get();
	const Scalar delta = ( lambda_end - lambda_begin ) / Scalar(nbins);

	if( m_kind == eSpectrumKind_Unbounded ) {
		const RGBUnboundedSpectrum s = RGBUnboundedSpectrum::FromRGB( rgb, table );
		for( unsigned int i = 0; i < nbins; ++i ) {
			sp.SetAtIndex( i, s.Eval( lambda_begin + Scalar(i) * delta ) );
		}
	} else if( m_kind == eSpectrumKind_Illuminant ) {
		const RGBIlluminantSpectrum s = RGBIlluminantSpectrum::FromRGB( rgb, table );
		for( unsigned int i = 0; i < nbins; ++i ) {
			sp.SetAtIndex( i, s.Eval( lambda_begin + Scalar(i) * delta ) );
		}
	} else {
		const RGBAlbedoSpectrum s = RGBAlbedoSpectrum::FromRGB( rgb, table );
		for( unsigned int i = 0; i < nbins; ++i ) {
			sp.SetAtIndex( i, s.Eval( lambda_begin + Scalar(i) * delta ) );
		}
	}
	return sp;
}

namespace
{
	const unsigned int kExpressionPainterTimeID = 300;
}

IKeyframeParameter* ExpressionPainter::KeyframeFromParameters( const String& name, const String& value )
{
	IKeyframeParameter* p = 0;

	if( name == "time" ) {
		Scalar v = Scalar( atof( value.c_str() ) );
		p = new Parameter<Scalar>( v, kExpressionPainterTimeID );
	} else {
		return 0;
	}

	GlobalLog()->PrintNew( p, __FILE__, __LINE__, "keyframe parameter" );
	return p;
}

void ExpressionPainter::SetIntermediateValue( const IKeyframeParameter& val )
{
	switch( val.getID() )
	{
	case kExpressionPainterTimeID:
		m_time = *(Scalar*)val.getValue();
		break;
	}

	// See GerstnerWavePainter::SetIntermediateValue: any consumer that
	// baked derived state off a prior Eval (e.g. a DisplacedGeometry, were
	// this painter ever used as a displacement) needs to know `time` moved.
	NotifyObservers();
}

//////////////////////////////////////////////////////////////////////
// ExpressionScalarPainter
//////////////////////////////////////////////////////////////////////

ExprEvalContext ExpressionScalarPainter::BuildContext( const RayIntersectionGeometric& ri ) const
{
	ExprEvalContext ctx;
	ctx.u = ri.ptCoord.x; ctx.v = ri.ptCoord.y;
	ctx.P  = Vector3( ri.ptIntersection.x, ri.ptIntersection.y, ri.ptIntersection.z );
	ctx.Po = Vector3( ri.ptObjIntersec.x, ri.ptObjIntersec.y, ri.ptObjIntersec.z );
	ctx.N  = ri.vNormal;
	ctx.fw = Scalar(0);		// Phase-2 footprint plumbing not landed yet (doc 88 decision 3)
	ctx.time = Scalar(0);		// not exposed on this pipe -- see class doc comment
	return ctx;
}

ScalarTriple ExpressionScalarPainter::GetValuesAt( const RayIntersectionGeometric& ri ) const
{
	const ExprEvalContext ctx = BuildContext( ri );
	if( m_prog.ResultType() == ExpressionProgram::kVec3 ) {
		const Vector3 v = m_prog.EvalVec3( ctx );
		// x->R, y->G, z->B -- the same triple ordering RGBScalarPainter's
		// constructor (r, g, b) and ScalarTriple's own (r, g, b) doc use.
		return ScalarTriple( SafeComp( v.x ), SafeComp( v.y ), SafeComp( v.z ) );
	}
	const Scalar v = SafeComp( m_prog.Eval( ctx ) );
	return ScalarTriple( v );
}

Scalar ExpressionScalarPainter::GetValueAtNM( const RayIntersectionGeometric& ri, Scalar nm ) const
{
	// Mirrors RGBScalarPainter::GetValueAtNM exactly: nominal wavelengths
	// R=650, G=550, B=450nm; piecewise-linear; clamped outside [450,650].
	const ScalarTriple t = GetValuesAt( ri );
	static constexpr Scalar kNmR = Scalar( 650.0 );
	static constexpr Scalar kNmG = Scalar( 550.0 );
	static constexpr Scalar kNmB = Scalar( 450.0 );

	if( nm <= kNmB ) return t.v[2];
	if( nm >= kNmR ) return t.v[0];
	if( nm <= kNmG ) {
		const Scalar frac = ( nm - kNmB ) / ( kNmG - kNmB );
		return t.v[2] + frac * ( t.v[1] - t.v[2] );
	}
	const Scalar frac = ( nm - kNmG ) / ( kNmR - kNmG );
	return t.v[1] + frac * ( t.v[0] - t.v[1] );
}
