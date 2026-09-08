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
#include "../Utilities/SurfaceCurvature.h"
#include "../Utilities/ExpressionMemo.h"
#include <cstdlib>

using namespace RISE;
using namespace RISE::Implementation;

//////////////////////////////////////////////////////////////////////
// ExpressionPainter
//////////////////////////////////////////////////////////////////////

namespace
{
	//! Fills ExprEvalContext::curv / curvR from the hit record -- SHARED by
	//! ExpressionPainter::BuildContext and ExpressionScalarPainter::BuildContext
	//! so the colour pipe and the physical-scalar pipe cannot drift (the two
	//! functions are twins; every other line in them is already duplicated
	//! deliberately, but this one has real logic in it).
	//!
	//! Preference order, per docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 5.4:
	//!
	//!   1. the DIRECT per-hit value, when the geometry computed one -- the
	//!      SDF family, whose implicit surface has no natural (u,v) for which
	//!      dndu/dndv would mean anything, but whose div n_hat is a few field
	//!      evaluations away and is genuinely smooth and resolution-free;
	//!   2. otherwise the shape operator over the record's derivatives --
	//!      triangle meshes and (since Phase 1) the analytic curved
	//!      primitives, which carry exact closed-form Weingarten maps;
	//!   3. otherwise 0 -- the `fw` convention: a valid-flag gate with a
	//!      documented zero fallback, not a fabricated value.  Planar
	//!      primitives read 0 because they ARE flat; the patch stubs read 0
	//!      because they report nothing (they are genuinely curved and
	//!      deliberately kept on the honest-absence path until their
	//!      derivative stubs are fixed -- design doc 14 item 2).  An
	//!      expression cannot tell those two zeros apart, which is the same
	//!      bargain `fw` already makes.
	//!
	//! `curv` is the dimensionless one: H x the hit geometry's WORLD
	//! bounding-box diagonal (`scaleHint`, already folded through the
	//! object's |det M|^(1/3) at the transform layer), so clamp(curv,0,1)
	//! behaves the same on a 0.05-unit creature feature and a 50-unit wall.
	//! `curvR` is the raw 1/world-length value for physically-scaled work.
	inline void PopulateCurvature( const RayIntersectionGeometric& ri, ExprEvalContext& ctx )
	{
		Scalar H = Scalar( 0 );
		bool haveH = false;

		if( ri.derivatives.curvatureValid ) {
			H = ri.derivatives.curvature;
			haveH = true;
		} else if( ri.derivatives.valid ) {
			haveH = SurfaceCurvature::MeanCurvatureFromDerivatives(
				ri.derivatives.dpdu, ri.derivatives.dpdv,
				ri.derivatives.dndu, ri.derivatives.dndv, H );
		}

		if( !haveH ) {
			ctx.curv  = Scalar( 0 );
			ctx.curvR = Scalar( 0 );
			return;
		}

		ctx.curvR = H;
		ctx.curv  = H * ri.derivatives.scaleHint;
	}

	//! Hands the expression VM the hit's geometry-signal channel -- the
	//! provider back-pointer plus the object-space (point, normal) to query
	//! it at (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §6.1).  SHARED by both
	//! BuildContext twins for the same anti-drift reason PopulateCurvature
	//! is.
	//!
	//! A straight copy, and deliberately so: NOTHING is computed here.
	//! `occlusion()` / `convexity()` / `thickness()` are arg-taking builtins evaluated only
	//! if the body calls them, so the expensive part (the SDF estimators)
	//! stays behind the call, not in front of the painter -- which is the
	//! whole reason these three need no consumption gate while `curv` does
	//! (design doc §6.2).  On geometry that publishes no provider the copied
	//! channel is empty, and the builtins return their neutral values.
	inline void PopulateSignals( const RayIntersectionGeometric& ri, ExprEvalContext& ctx )
	{
		ctx.signals = ri.signals;
	}
}

ExprEvalContext ExpressionPainter::BuildContext( const RayIntersectionGeometric& ri ) const
{
	ExprEvalContext ctx;
	ctx.u = ri.ptCoord.x; ctx.v = ri.ptCoord.y;
	ctx.P  = Vector3( ri.ptIntersection.x, ri.ptIntersection.y, ri.ptIntersection.z );
	ctx.Po = Vector3( ri.ptObjIntersec.x, ri.ptObjIntersec.y, ri.ptObjIntersec.z );
	ctx.N  = ri.vNormal;
	// doc 88 S9: honest world-space filter-width estimate on primary hits
	// against ANY geometry.  Keyed on `widthValid`, NOT `valid`: `valid` is
	// the UV-Jacobian flag, and a UV-free hit (SDF, box, disk, plane, hair)
	// has a perfectly good footprint width with no Jacobian to go with it
	// (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md -- the arc that retired
	// this comment's former "triangle-mesh geometry, the only geometry that
	// currently populates txFootprint" claim).  Stays 0 where there is
	// genuinely no filter information: secondary/diffuse bounces spawn a
	// fresh Ray with hasDifferentials=false, and the thin-lens /
	// orthographic / fisheye cameras never set differentials at all -- that
	// is the honest "point sample, no filter info" answer, not a bug.
	ctx.fw = ri.txFootprint.widthValid ? ri.txFootprint.worldWidth : Scalar(0);
	// 2026-09-06: the SAME footprint in the frame `Po` is written in, so an
	// object-space noise domain (`fbm(Po*62, ...)`) can be filtered too.
	// It rides the same `widthValid` gate for the same reason, and it is
	// non-zero on exactly the hits `fw` is: `Object::IntersectRay` stamps
	// the two from the same footprint inside the same `widthValid` block
	// (a CSG composite included -- it inherits the winning child's, which
	// is the frame `ptObjIntersec` is in).  So a Po-domain body loses its
	// fade only where a P-domain body would lose its own, never on its
	// own.
	ctx.fwo = ri.txFootprint.widthValid ? ri.txFootprint.objectWidth : Scalar(0);
	ctx.time = m_time;
	PopulateCurvature( ri, ctx );
	PopulateSignals( ri, ctx );
	return ctx;
}

//! THE L2 MEMO SITE for the colour pipe (../Utilities/ExpressionMemo.h).
//!
//! One shading event routinely runs one program many times at the SAME
//! context.  On `plank_closeup` a single `expression_painter` feeds a
//! ramp, two GGX anisotropy slots and a relief modifier, and the
//! measured repeat rate here is 82.7 % of 320 M evaluations.  The
//! spectral pipe adds its own: GetColorNM calls this once per wavelength
//! sample with a byte-identical `ri`.
//!
//! The memo is keyed on the program's process-unique id, WHICH PIPE is
//! asking (ExpressionMemo::ePipeColour here), and every field of the
//! context, compared exactly, so a hit returns the value the recompute
//! would have produced bit for bit -- and it wraps a CALL to `EvalVec3`,
//! whose body is untouched, so the VM's own arithmetic (a contract; see
//! ExpressionProgram::MakeMemoKey's comment, and ExpressionMemo.h for
//! exactly how much of it TextureExpressionVMTest actually compares
//! exactly) cannot move.
RISEPel ExpressionPainter::EvalRGB( const RayIntersectionGeometric& ri ) const
{
	const ExprEvalContext ctx = BuildContext( ri );

	ExpressionMemo::ProgramKey key;
	const bool memoable = m_prog.MemoWorthy() && m_prog.ProgramId() != 0;
	if( memoable ) {
		m_prog.MakeMemoKey( ctx, ExpressionMemo::ePipeColour, key );
		Vector3 hit;
		if( ExpressionMemo::L2Find( key, hit ) ) {
			return RISEPel( SafeComp( hit.x ), SafeComp( hit.y ), SafeComp( hit.z ) );
		}
	}

	const Vector3 v = m_prog.EvalVec3( ctx );	// scalar-typed programs broadcast to (s,s,s)
	if( memoable ) ExpressionMemo::L2Insert( key, v );
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
	RISEPel rgb = EvalRGB( ri );
	// Unbounded/Illuminant FromRGB derive `scale` from the max channel
	// OUTSIDE the maxc>1e-9 branch, so an all-negative triple (a subtraction
	// or other rgb_expression that goes negative) yields sigmoid 0.5 times a
	// NEGATIVE scale instead of the intended clamp-to-zero.  Clamp once, up
	// front, so all three kinds see a non-negative input (Albedo already
	// clamps internally, so this is a no-op for it).
	ColorMath::EnsurePositve( rgb );
	const RGBToSpectrumTable& table = RGBToSpectrumTable::Get();

	if( m_kind == eSpectrumKind_Unbounded ) {
		return RGBUnboundedSpectrum::FromRGB( rgb, table ).Eval( nm );
	}
	if( m_kind == eSpectrumKind_Illuminant ) {
		return RGBIlluminantSpectrum::FromRGB( rgb, table ).Eval( nm );
	}
	return RGBAlbedoSpectrum::FromRGB( rgb, table ).Eval( nm );
}

Scalar ExpressionPainter::GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const
{
	// Stage C slice 2: as a SOURCE, an expression's output carries the
	// reference illuminant's shape regardless of `m_kind` (which only
	// describes how the same value behaves as a multiplier).  Overriding
	// the IPainter default skips a redundant GetColor virtual so the
	// program is evaluated exactly once, matching GetColorNM's cost.
	RISEPel rgb = EvalRGB( ri );
	ColorMath::EnsurePositve( rgb );
	return RGBIlluminantSpectrum::FromRGB( rgb, RGBToSpectrumTable::Get() ).Eval( nm );
}

SpectralPacket ExpressionPainter::GetSpectrum( const RayIntersectionGeometric& ri ) const
{
	const Scalar lambda_begin = Scalar(380);
	const Scalar lambda_end   = Scalar(780);
	const unsigned int nbins  = 81;
	SpectralPacket sp( lambda_begin, lambda_end, nbins );

	RISEPel rgb = EvalRGB( ri );
	// See GetColorNM above: clamp before the kind switch so Unbounded/
	// Illuminant FromRGB never see an all-negative triple (would derive a
	// negative `scale` from the max-channel outside the maxc>1e-9 guard).
	ColorMath::EnsurePositve( rgb );
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
	// doc 88 S9: honest world-space filter-width estimate on primary hits
	// against ANY geometry.  Keyed on `widthValid`, NOT `valid`: `valid` is
	// the UV-Jacobian flag, and a UV-free hit (SDF, box, disk, plane, hair)
	// has a perfectly good footprint width with no Jacobian to go with it
	// (docs/TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md -- the arc that retired
	// this comment's former "triangle-mesh geometry, the only geometry that
	// currently populates txFootprint" claim).  Stays 0 where there is
	// genuinely no filter information: secondary/diffuse bounces spawn a
	// fresh Ray with hasDifferentials=false, and the thin-lens /
	// orthographic / fisheye cameras never set differentials at all -- that
	// is the honest "point sample, no filter info" answer, not a bug.
	ctx.fw = ri.txFootprint.widthValid ? ri.txFootprint.worldWidth : Scalar(0);
	// 2026-09-06: the SAME footprint in the frame `Po` is written in, so an
	// object-space noise domain (`fbm(Po*62, ...)`) can be filtered too.
	// It rides the same `widthValid` gate for the same reason, and it is
	// non-zero on exactly the hits `fw` is: `Object::IntersectRay` stamps
	// the two from the same footprint inside the same `widthValid` block
	// (a CSG composite included -- it inherits the winning child's, which
	// is the frame `ptObjIntersec` is in).  So a Po-domain body loses its
	// fade only where a P-domain body would lose its own, never on its
	// own.
	ctx.fwo = ri.txFootprint.widthValid ? ri.txFootprint.objectWidth : Scalar(0);
	ctx.time = Scalar(0);		// not exposed on this pipe -- see class doc comment
	PopulateCurvature( ri, ctx );
	PopulateSignals( ri, ctx );
	return ctx;
}

//! THE L2 MEMO SITE for the physical-scalar pipe -- see
//! ExpressionPainter::EvalRGB above for the argument; this is its twin,
//! and both must be wired or a scene whose field feeds roughness rather
//! than colour would get none of it.
//!
//! EACH RESULT TYPE KEEPS THE ENTRY POINT IT HAD BEFORE THE MEMO: a
//! vec3-typed program calls `EvalVec3`, a scalar-typed one calls
//! `Eval`, and the memo only wraps the call.  That is deliberate and it
//! is the one contract this file is not allowed to move -- under
//! `-ffast-math` merely changing WHICH entry point this hot consumer
//! inlines has already been observed to move an ulp (see
//! ExpressionProgram::MakeMemoKey's comment).  The scalar branch stores
//! the broadcast `(s,s,s)` so both branches share ONE entry shape, and
//! reads `.x` back out of it.
//!
//! THE TWO PIPES DO NOT SHARE ENTRIES -- because the key SAYS SO, not
//! because sharing is unreachable.  An earlier draft of this comment
//! argued the latter ("each compiles its own program, so they key apart
//! on the id"), and that argument is WRONG: RISE_API_CreateExpressionPainter
//! and RISE_API_CreateExpressionScalarPainter both take an
//! `ExpressionProgram` by const reference, each painter holds a COPY, and
//! a copy KEEPS its source's id -- so one compiled scalar-typed program
//! handed to both factories would land on one entry, and this function
//! would then return the colour pipe's `EvalVec3(...).x` where the
//! paragraph above requires `Eval`.  ExpressionMemo::ProgramKey therefore
//! carries an EvalPipe tag (ePipeScalar here), which is what actually
//! keeps them apart; ExpressionMemoTest's cross-pipe check (m) shares
//! exactly such a program through the API and pins the contract.
//!
//! The SUBSTITUTION is certain; the DIVERGENCE is not observed on this
//! toolchain (removing the tag leaves (m) green, because the two entry
//! points reach one `RunAny` and produce the same bits today).  The tag
//! guards a compiler freedom that has already bitten this code once, at a
//! cost of one int compare -- see ExpressionMemo::EvalPipe for the full
//! disclosure, and do not delete it on the strength of (m) staying green.
//!
//! (Two painters of the SAME kind do still share entries when built from
//! one compiled program -- the copied-program case ExpressionMemoTest (c)
//! pins -- and that is correct: same function, same pipe.)  In
//! `plank_closeup`, for instance, the roughness slots that read the
//! plank's own field are `scalar_painter { painter expr_plank }` -- a
//! PainterToScalarAdapter over the COLOUR painter, so those hits land on
//! EvalRGB's entry above and never reach this function -- while
//! `sp_pore` is a genuine `scalar_painter { expression ... }` over its
//! OWN program, memoised here under its own id.
ScalarTriple ExpressionScalarPainter::GetValuesAt( const RayIntersectionGeometric& ri ) const
{
	const ExprEvalContext ctx = BuildContext( ri );
	const bool isVec3 = ( m_prog.ResultType() == ExpressionProgram::kVec3 );

	ExpressionMemo::ProgramKey key;
	const bool memoable = m_prog.MemoWorthy() && m_prog.ProgramId() != 0;
	if( memoable ) {
		m_prog.MakeMemoKey( ctx, ExpressionMemo::ePipeScalar, key );
		Vector3 hit;
		if( ExpressionMemo::L2Find( key, hit ) ) {
			if( isVec3 ) return ScalarTriple( SafeComp( hit.x ), SafeComp( hit.y ), SafeComp( hit.z ) );
			return ScalarTriple( SafeComp( hit.x ) );
		}
	}

	Vector3 v;
	if( isVec3 ) {
		v = m_prog.EvalVec3( ctx );
	} else {
		const Scalar s = m_prog.Eval( ctx );	// the pre-memo entry point, unchanged
		v = Vector3( s, s, s );
	}
	if( memoable ) ExpressionMemo::L2Insert( key, v );
	if( isVec3 ) {
		// x->R, y->G, z->B -- the same triple ordering RGBScalarPainter's
		// constructor (r, g, b) and ScalarTriple's own (r, g, b) doc use.
		return ScalarTriple( SafeComp( v.x ), SafeComp( v.y ), SafeComp( v.z ) );
	}
	return ScalarTriple( SafeComp( v.x ) );
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
