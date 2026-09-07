//////////////////////////////////////////////////////////////////////
//
//  ExpressionPainter.h - Texture-expression VM surfaces on the COLOUR
//  pipe (ExpressionPainter, IPainter) and the PHYSICAL-SCALAR pipe
//  (ExpressionScalarPainter, IScalarPainter) -- S2 of doc 88 (P1).
//
//  Both classes wrap a compiled ExpressionProgram built with
//  EnableContextVars(true) (see ExpressionEval.h's doc comment on why
//  expression_function2d, the S1 (u,v)-only surface, does NOT do this):
//  the body sees the full 3D context (u, v, P, Po, N, fw, time), not
//  just UV.  ExpressionPainter is deliberately registered ONLY in the
//  colour-painter manager, never in the IFunction2D manager -- doing
//  so would resurrect the "silently zero" trap other 3D-context
//  painters carry today (Painter::Evaluate's default IFunction2D hook
//  builds a dummy RayIntersectionGeometric with P/Po/N all zero, so a
//  registered 3D-context painter evaluated via Evaluate(u,v) alone
//  would silently see a constant field).  See the expression_painter
//  chunk parser (ChunkParserRegistry.cpp) for the single-manager
//  registration.
//
//  BuildExpressionProgramFromChunkFields is the shared chunk-field ->
//  ExpressionProgram builder used by BOTH surfaces: expression_painter
//  (via Job::AddExpressionPainter) and scalar_painter { expression ... }
//  (built directly in ChunkParserRegistry.cpp's ScalarPainterAsciiChunkParser,
//  matching how its nine sibling forms construct their painter inline
//  rather than routing through IJob).  Centralising it here means the
//  `seed` auto-registration + ExpressionParamSpec::ParseParamSpecLine
//  metadata parsing + `def` wiring can't drift between the two pipes.
//
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef EXPRESSION_PAINTER_
#define EXPRESSION_PAINTER_

#include "Painter.h"
#include "ExpressionEval.h"
#include "ExpressionParamSpec.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Interfaces/ILog.h"
#include "../Utilities/Reference.h"
#include "../Utilities/SurfaceCurvature.h"
#include "../Intersection/RayIntersectionGeometric.h"
#include <string>
#include <vector>

namespace RISE
{
	namespace Implementation
	{
		//! Parses `paramLines` (each `<name> <value> [min][max][step][label]`,
		//! ExpressionParamSpec grammar) and `defLines` (each `<name> <expr>`),
		//! auto-registers a named scalar constant `seed` BEFORE any of the
		//! author's own `param` lines run (so an explicit `param seed <v>`
		//! line -- if present -- wins via ExpressionEval's same-type
		//! last-wins duplicate rule; see ExpressionProgram::Builder::
		//! RejectIfDuplicate) IF `autoRegisterSeed` is true, then compiles
		//! `finalExpr` with context vars set per `enableContextVars`.  On
		//! success returns true, fills `outProg`, and fills `outSpecs`
		//! with the full parsed ParamSpec list (metadata included) in
		//! `param` line order -- retrievable later via GetParamSpecs()
		//! for the S4 introspection slice.  On failure logs a diagnostic
		//! prefixed with `context` (e.g. "expression_painter `marble`")
		//! via GlobalLog() and returns false.
		//!
		//! `enableContextVars`/`autoRegisterSeed` (review-round unification,
		//! doc 88 sect. 7 decision 5): the ONE thing that differs between
		//! the full-3D-context surfaces (expression_painter, scalar_painter
		//! {expression}, both pass true/true, preserving this function's
		//! ORIGINAL behavior exactly) and the UV-only expression_function2d
		//! surface (passes false/false -- see
		//! ExpressionFunction2DPainterAsciiChunkParser::Finalize,
		//! ChunkParserRegistry.cpp).  expression_function2d's UV-only
		//! contract is DELIBERATE and part of its FROZEN surface
		//! (ExpressionEval.h's own EnableContextVars doc comment: enabling
		//! context vars there would let e.g. `fbm(P*4,...)` compile and
		//! silently evaluate to a constant, since IFunction2D::Evaluate(u,v)
		//! never supplies P/Po/N/fw/time) -- `seed` is likewise withheld
		//! because expression_function2d never had one and adding it now
		//! would be a NEW capability on a surface doc 88 explicitly froze
		//! ("a later refactor may fold it into/behind the new VM surface"
		//! -- folding the PARSING glue, not growing the authoring surface).
		//! `seed` is unused (never read) when `autoRegisterSeed` is false;
		//! a caller with no seed of its own passes 0.
		//!
		//! `outError` (optional, defaults to nullptr -- every pre-existing
		//! caller is byte-identical): on failure, filled with the EXACT same
		//! text this function logs via GlobalLog() (built once, used for
		//! both), so a caller that needs the SPECIFIC compiler diagnostic
		//! (e.g. to thread it into RISE::g_cstFinalizeDiagSink, see
		//! GenericManager.h) doesn't have to reconstruct it from the log.
		//! Left untouched (whatever the caller passed in) when this function
		//! returns true.
		inline bool BuildExpressionProgramFromChunkFields(
			const std::string& context,
			const std::vector<std::string>& paramLines,
			const std::vector<std::string>& defLines,
			const Scalar seed,
			const std::string& finalExpr,
			ExpressionProgram& outProg,
			std::vector<ParamSpec>& outSpecs,
			bool enableContextVars,
			bool autoRegisterSeed,
			std::string* outError = nullptr )
		{
			outSpecs.clear();

			// Builds `msg`, logs it (identical text/format to the pre-outError
			// code), fills `*outError` if the caller wants it, and returns
			// false -- the single choke point every failure below goes
			// through so the logged text and the returned diagnostic can
			// never drift apart.
			auto fail = [&]( const std::string& msg ) -> bool {
				GlobalLog()->PrintEx( eLog_Error, "%s", msg.c_str() );
				if( outError ) *outError = msg;
				return false;
			};

			if( finalExpr.empty() ) {
				return fail( context + ": missing the final value expression" );
			}

			ExpressionProgram::Builder builder;
			builder.EnableContextVars( enableContextVars );

			if( autoRegisterSeed ) {
				if( !builder.AddParam( "seed", seed ) ) {
					return fail( context + ": internal error registering `seed`: " + builder.Error() );
				}
			}

			for( std::size_t i = 0; i < paramLines.size(); ++i ) {
				ParamSpec spec;
				std::string err;
				ptrdiff_t errOff = -1;
				if( !ParseParamSpecLine( paramLines[i], spec, err, errOff ) ) {
					return fail( context + ": param " + std::to_string( i ) + " (`" + paramLines[i] + "`): " + err );
				}
				if( !ExpressionProgram::IsFinite( spec.value ) ) {
					return fail( context + ": param `" + spec.name + "` must be finite (nan/inf rejected)" );
				}
				// P1-A parity: a duplicate name of a DIFFERENT type (incl.
				// colliding with `seed`, a scalar) is a hard compile error;
				// same-type is last-wins.  See BuildExpressionProgramFromChunkFields's
				// doc comment above for why `seed` is registered first.
				if( !builder.AddParam( spec.name, spec.value ) ) {
					return fail( context + ": param `" + spec.name + "`: " + builder.Error() );
				}
				outSpecs.push_back( spec );
			}

			for( std::size_t i = 0; i < defLines.size(); ++i ) {
				const std::string& line = defLines[i];
				const std::size_t sp = line.find_first_of( " \t" );
				if( sp == std::string::npos ) {
					return fail( context + ": def " + std::to_string( i ) + " (`" + line + "`) must be `<name> <expression>`" );
				}
				const std::string dname = line.substr( 0, sp );
				const std::string dexpr = line.substr( sp + 1 );
				if( !builder.AddDef( dname, dexpr ) ) {
					return fail( context + ": def `" + dname + "`: " + builder.Error() );
				}
			}

			outProg = ExpressionProgram::Invalid();
			if( !builder.Finalize( finalExpr, outProg ) ) {
				return fail( context + ": expr: " + builder.Error() );
			}
			return true;
		}

		//! expression_painter -- the COLOUR pipe.  vec3-typed programs are
		//! Rec.709 linear RGB; scalar-typed programs broadcast to grayscale
		//! (r=g=b=value), matching ExpressionFunction2DPainter's convention.
		//! GetColorNM/GetSpectrum uplift the evaluated RGB PER-SAMPLE via
		//! the same RGBToSpectrumTable route TexturePainter uses (no baked/
		//! cached spectrum -- the field varies with the hit, exactly like a
		//! texture's per-texel value, so caching at construction is wrong).
		class ExpressionPainter : public Painter
		{
		protected:
			ExpressionProgram      m_prog;
			std::vector<ParamSpec> m_paramSpecs;	// S4 introspection; not consulted by Eval
			Scalar                 m_time;			// keyframeable
			SpectrumKind            m_kind;
			//! CURVATURE DEMAND (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md 5.4).
			//! Active iff this painter's compiled body reads `curv` / `curvR`.
			//! Its whole job is to let the SDF family skip ~18 extra field
			//! evaluations per hit on the overwhelming majority of scenes,
			//! whose expressions never mention curvature -- `curv` is a
			//! CONTEXT VARIABLE, computed before the painter runs, so unlike a
			//! lazily-called builtin it needs an up-front consumption
			//! predicate.  RAII: constructed with the painter, dropped with
			//! it, so a geometry edit that replaces the material stops paying
			//! the moment the old painter's last reference goes.  See
			//! SurfaceCurvatureDemand for the mechanism and its documented
			//! process-wide conservatism.
			SurfaceCurvatureDemand::Registration m_curvatureDemand;
			//! SIGNAL DEMAND, diagnostic-only (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md
			//! §14 item 11).  Active iff this painter's compiled body calls
			//! `occlusion()` / `convexity()` / `thickness()` anywhere.  Unlike
			//! m_curvatureDemand above, this does NOT gate any per-hit work --
			//! see SurfaceSignalDemand's own doc comment (ISurfaceSignalProvider.h)
			//! for why the provider install stays unconditional.  Its only job is
			//! to let a BDPT/VCM/MLT rasterizer ask "is anyone using a signal
			//! this integrator family evaluates as neutral in parts of its
			//! transport" at render start, for the one-time containment warning.
			SurfaceSignalDemand::Registration m_signalDemand;

			virtual ~ExpressionPainter() {}

			ExprEvalContext BuildContext( const RayIntersectionGeometric& ri ) const;

			// NaN/Inf -> 0 (ExpressionProgram::IsFinite is volatile-hardened;
			// see ExpressionFunction2DPainter::Safe).  Negative or >1
			// components are passed through UNCLAMPED -- matching
			// ExpressionFunction2DPainter / Function2DColorPainter
			// precedent (neither clamps); an author who wants a bounded
			// albedo remaps explicitly (clamp()/ramp()) in the body.
			static Scalar SafeComp( const Scalar v ) { return ExpressionProgram::IsFinite( v ) ? v : Scalar(0); }

			RISEPel EvalRGB( const RayIntersectionGeometric& ri ) const;

		public:
			ExpressionPainter( const ExpressionProgram& prog, const std::vector<ParamSpec>& paramSpecs,
				const Scalar time, const SpectrumKind kind = eSpectrumKind_Albedo ) :
				m_prog( prog ), m_paramSpecs( paramSpecs ), m_time( time ), m_kind( kind ),
				m_curvatureDemand( prog.UsesSurfaceCurvature() ),
				m_signalDemand( prog.UsesSurfaceSignals() )
			{}

			//! S4 introspection: full param metadata (min/max/step/label),
			//! in `param` line order.
			const std::vector<ParamSpec>& GetParamSpecs() const { return m_paramSpecs; }

			//! S10 (doc 88): read-only access to the compiled program, for
			//! the def-stage preview engine (PainterPreview.cpp) to call
			//! ExpressionProgram::EvalDefStage / DefCount directly.  Not
			//! consulted by GetColor/GetColorNM/GetSpectrum -- eval stays on
			//! m_prog exactly as before.
			const ExpressionProgram& GetProgram() const { return m_prog; }

			RISEPel        GetColor( const RayIntersectionGeometric& ri ) const override;
			Scalar         GetColorNM( const RayIntersectionGeometric& ri, const Scalar nm ) const override;
			//! Source-term sample (Stage C slice 2): the evaluated RGB
			//! uplifted as an ILLUMINANT, whatever `m_kind` is.
			Scalar         GetRadianceNM( const RayIntersectionGeometric& ri, const Scalar nm ) const override;
			SpectralPacket GetSpectrum( const RayIntersectionGeometric& ri ) const override;

			// IFunction2D::Evaluate is inherited from Painter's default
			// (samples GetColor through a dummy ri with P/Po/N held at
			// zero) -- unreachable in practice because this painter is
			// deliberately NEVER registered in the IFunction2D manager
			// (see the file header comment); kept only to satisfy the
			// interface, exactly like every other 3D-context painter.

			// Keyframable: `time` only (Gerstner precedent).
			IKeyframeParameter* KeyframeFromParameters( const String& name, const String& value ) override;
			void SetIntermediateValue( const IKeyframeParameter& val ) override;
			void RegenerateData() override {}
		};

		//! scalar_painter { expression ... } -- the PHYSICAL-SCALAR pipe.
		//! No colorspace, no JH uplift, by construction (IScalarPainter
		//! never goes through GetColorNM's uplift path).  A scalar-typed
		//! program yields a uniform ScalarTriple; a vec3-typed program
		//! yields a genuine per-channel triple (x->R, y->G, z->B) with
		//! HasPerChannelVariation() == true -- spatially-varying,
		//! per-channel physical scalars (e.g. RGB-dispersive IOR driven by
		//! a 3D field) are a deliberate feature of this surface, not an
		//! accident of the type system.
		//!
		//! `time` is NOT exposed on this pipe: IScalarPainter does not
		//! derive IKeyframable (unlike IPainter), so there is no keyframe
		//! hook to animate it through; a body that references `time`
		//! evaluates it as the fixed constant 0.  (Pre-S9, `fw` was also
		//! always 0 for the same "no plumbing yet" reason `time` is fixed
		//! here -- since doc 88 S9, `fw` is live on both pipes: see
		//! BuildContext below.)
		class ExpressionScalarPainter :
			public virtual IScalarPainter,
			public virtual Reference
		{
		protected:
			ExpressionProgram      m_prog;
			std::vector<ParamSpec> m_paramSpecs;
			//! See ExpressionPainter::m_curvatureDemand -- same RAII gate on
			//! the physical-scalar pipe.  Both pipes must register or the gate
			//! would miss the single most likely authoring shape for this
			//! signal: `scalar_painter { expression "clamp(-curv,0,1)" }`
			//! feeding a roughness slot.
			SurfaceCurvatureDemand::Registration m_curvatureDemand;
			//! See ExpressionPainter::m_signalDemand -- same diagnostic-only
			//! RAII gate on the physical-scalar pipe.  `scalar_painter {
			//! expression "occlusion(0.1)" }` feeding a dirt/wear slot is at
			//! least as likely an authoring shape as the colour pipe's, so
			//! both must register or the containment diagnostic would miss it.
			SurfaceSignalDemand::Registration m_signalDemand;
			virtual ~ExpressionScalarPainter() {}

			static Scalar SafeComp( const Scalar v ) { return ExpressionProgram::IsFinite( v ) ? v : Scalar(0); }

			ExprEvalContext BuildContext( const RayIntersectionGeometric& ri ) const;

		public:
			ExpressionScalarPainter( const ExpressionProgram& prog, const std::vector<ParamSpec>& paramSpecs ) :
				m_prog( prog ), m_paramSpecs( paramSpecs ),
				m_curvatureDemand( prog.UsesSurfaceCurvature() ),
				m_signalDemand( prog.UsesSurfaceSignals() )
			{}

			//! S4 introspection: full param metadata, in `param` line order.
			const std::vector<ParamSpec>& GetParamSpecs() const { return m_paramSpecs; }

			//! S10 (doc 88): see ExpressionPainter::GetProgram's doc comment
			//! -- same additive accessor on the scalar pipe.
			const ExpressionProgram& GetProgram() const { return m_prog; }

			ScalarTriple GetValuesAt( const RayIntersectionGeometric& ri ) const override;

			//! Mirrors RGBScalarPainter's triple -> wavelength mapping
			//! exactly (nominal R=650/G=550/B=450nm, piecewise-linear,
			//! clamped outside [450,650]).  For a scalar-typed program the
			//! triple is uniform, so this collapses to that single value
			//! for every nm -- upholding the IScalarPainter contract for
			//! wavelength-independent painters without a separate branch.
			Scalar GetValueAtNM( const RayIntersectionGeometric& ri, Scalar nm ) const override;

			//! Static per doc: true whenever the compiled program is
			//! vec3-typed (the per-channel triple is a standing feature
			//! of a vec3 body, not a per-hit check of whether the three
			//! components happen to differ at any one point).
			bool HasPerChannelVariation() const override { return m_prog.ResultType() == ExpressionProgram::kVec3; }
		};
	}
}

#endif
