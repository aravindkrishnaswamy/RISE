//////////////////////////////////////////////////////////////////////
//
//  ExpressionPainter.h - Texture-expression VM surfaces on the COLOUR
//  pipe (ExpressionPainter, IPainter) and the PHYSICAL-SCALAR pipe
//  (ExpressionScalarPainter, IScalarPainter) -- S2 of doc 88 (P1).
//
//  Both classes wrap a compiled ExpressionProgram built with
//  EnableContextVars(true) (see ExpressionEval.h's doc comment on why
//  expression_function2d, the S1 (u,v)-only surface, does NOT do this):
//  the body sees the full 3D context (u, v, P, Po, N, fw, fwo, time), not
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
#include <cassert>	// DL-25 P3-7: the ctors' IsPainterRefsBound() contract
#include "ExpressionEval.h"
#include "ExpressionParamSpec.h"
#include "../Interfaces/IScalarPainter.h"
#include "../Utilities/GradedIndexMedium.h"	// DL-09: GradedIndexDemand, the graded-index cost gate
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
		//! DL-25 (docs/WETNESS_COAT_DESIGN.md sec 4(g), docs/DEBT_LEDGER.md):
		//! minimal name -> painter resolution contract for
		//! `sample(name)`/`sample_scalar(name)`.  Kept ABSTRACT here, rather
		//! than resolving directly against `IJobPriv`'s painter managers,
		//! so this header does not have to include IJobPriv.h (a heavy
		//! interface that pulls in IRasterizer.h/Cst.h) merely to resolve
		//! two chunk-reference names -- the concrete implementation
		//! (wrapping `IJobPriv::GetPainters()`/`GetScalarPainters()`, plus
		//! the self-reference diagnostic) lives at each of the two call
		//! sites that already have an `IJobPriv&` in scope: `Job::AddExpressionPainter`
		//! (Job.cpp) and `ScalarPainterAsciiChunkParser`'s `expression` form
		//! (ChunkParserRegistry.cpp).
		class IExpressionPainterRefResolver
		{
		public:
			virtual ~IExpressionPainterRefResolver() {}
			//! Resolve one `sample(name)` reference.  On success, returns
			//! true and fills `out` with a BORROWED pointer (already
			//! registered in the caller's painter manager, and therefore
			//! kept alive by it for the scene's lifetime) -- BindPainterRefs
			//! addref's it on top, so the caller's own reference is
			//! untouched.  On failure, returns false and fills `err` with a
			//! caller-ready diagnostic FRAGMENT (no leading "sample(...):"
			//! -- BuildExpressionProgramFromChunkFields adds that framing).
			virtual bool ResolveColorPainter( const std::string& name, IPainter*& out, std::string& err ) = 0;
			//! Same contract, for `sample_scalar(name)` against the
			//! IScalarPainter manager.
			virtual bool ResolveScalarPainter( const std::string& name, IScalarPainter*& out, std::string& err ) = 0;
		};

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
		//!
		//! `painterResolver` (DL-25, optional -- every pre-existing caller
		//! passes nullptr and is byte-identical): resolves any
		//! `sample(name)`/`sample_scalar(name)` references the compiled body
		//! contains, AFTER `Builder::Finalize` succeeds and BEFORE this
		//! function returns -- so a caller that receives `true` always gets
		//! back a program whose painter refs are already bound
		//! (`outProg.IsPainterRefsBound()`), never a partially-attached one.
		//! A program that calls neither builtin needs no resolver at all
		//! (`UsesPainterSample()` is checked before `painterResolver` is
		//! ever dereferenced), which is what keeps expression_function2d's
		//! call site (context vars off, so neither builtin can even compile)
		//! and any other nullptr-passing caller exactly as they were.  A
		//! program that DOES use one and receives a null resolver, or whose
		//! resolver refuses a specific name, fails here with a diagnostic
		//! naming the expression and the painter -- never a silent partial
		//! bind.
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
			std::string* outError = nullptr,
			IExpressionPainterRefResolver* painterResolver = nullptr )
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

			// DL-25: resolve every sample()/sample_scalar() reference the
			// compiled body contains, BEFORE returning success -- see this
			// function's own doc comment for the "never a partial bind"
			// contract, and IExpressionPainterRefResolver's for why the
			// resolution itself lives at the CALLER (the only layer with an
			// IJob painter manager to resolve a name against).
			if( outProg.UsesPainterSample() ) {
				if( !painterResolver ) {
					return fail( context + ": uses sample()/sample_scalar(), but this surface has no "
						"painter manager to resolve them against" );
				}
				const std::vector<std::string>& colorNames = outProg.ColorPainterRefNames();
				std::vector<IPainter*> colorPtrs;
				colorPtrs.reserve( colorNames.size() );
				for( std::size_t i = 0; i < colorNames.size(); ++i ) {
					IPainter* p = nullptr;
					std::string err;
					if( !painterResolver->ResolveColorPainter( colorNames[i], p, err ) ) {
						return fail( context + ": sample(`" + colorNames[i] + "`): " + err );
					}
					// DL-165 (docs/DEBT_LEDGER.md): sample() evaluates ONLY
					// IPainter::GetColor -- a painter that carries a genuine,
					// independently-authored SPECTRUM (spectral_painter,
					// blackbody_painter, a measured-SPD Function1D-backed
					// colour painter -- see IPainter::IsSpectrallyDefined's
					// own doc comment for the exact set) would have that
					// spectrum collapsed to RGB here and then RE-UPLIFTED
					// through the Jakob-Hanika LUT by whatever spectral
					// consumer this expression feeds -- a DIFFERENT curve,
					// silently, and `GetAlpha` dropped on top. Refuse
					// outright (DL-32's hard-fail-the-chunk convention)
					// rather than let the mismatch through: an author who
					// needs that painter's real spectrum binds it DIRECTLY
					// to a colour slot instead of sampling it.
					if( p->IsSpectrallyDefined() ) {
						return fail( context + ": sample(`" + colorNames[i] + "`) refused -- `" + colorNames[i] +
							"` is spectrally defined (a genuine SPD, not an RGB colour): sample() reads only "
							"GetColor() and would collapse that spectrum to RGB and re-uplift a DIFFERENT curve "
							"through the Jakob-Hanika LUT; bind `" + colorNames[i] + "` DIRECTLY to a colour "
							"slot instead of sampling it (see docs/DEBT_LEDGER.md DL-165)" );
					}
					colorPtrs.push_back( p );
				}
				const std::vector<std::string>& scalarNames = outProg.ScalarPainterRefNames();
				std::vector<IScalarPainter*> scalarPtrs;
				scalarPtrs.reserve( scalarNames.size() );
				for( std::size_t i = 0; i < scalarNames.size(); ++i ) {
					IScalarPainter* p = nullptr;
					std::string err;
					if( !painterResolver->ResolveScalarPainter( scalarNames[i], p, err ) ) {
						return fail( context + ": sample_scalar(`" + scalarNames[i] + "`): " + err );
					}
					scalarPtrs.push_back( p );
				}
				if( !outProg.BindPainterRefs( colorPtrs, scalarPtrs ) ) {
					// Unreachable from this call site (sizes are built
					// index-aligned above and every entry checked non-null
					// before being pushed) -- kept as a named failure rather
					// than an assert so a future refactor that breaks the
					// alignment fails loudly instead of silently mis-binding.
					return fail( context + ": internal error binding sample()/sample_scalar() references" );
				}
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
			//! SIGNAL DEMAND (docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md §14 item 11,
			//! CLOSED 2026-09-11 -- docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md).
			//! NOT diagnostic-only, despite the name: since S3 it gates real
			//! work (below).  Active iff this painter's compiled body calls
			//! `occlusion()` / `convexity()` / `thickness()` OR either
			//! cross-object signal (`proximity()` / `interior()`)
			//! anywhere -- it is built from `prog.UsesSurfaceSignals()`, which is
			//! `!m_signalCalls.empty()`, and `m_signalCalls` carries all five
			//! (ExpressionEval.h).  It does NOT gate the per-hit provider
			//! install -- see SurfaceSignalDemand's own doc comment
			//! (ISurfaceSignalProvider.h) for why that stays unconditional --
			//! but since 2026-09-11 it gates REAL WORK elsewhere: with
			//! `SurfaceCurvatureDemand` it gates
			//! `LightSampler::ProbeEmitterSurface`, the probe that makes a
			//! sampled emission point's record carry live signals
			//! (docs/SIGNALS_UNDER_BIDIRECTIONAL_TRANSPORT.md §5) -- so a
			//! painter that registers here is what makes an emissive material
			//! keyed on a signal read that signal under NEE, and counting the
			//! cross-object pair here is what opens that gate for a painter
			//! that calls only `proximity()` or only `interior()`.  (Slice S4
			//! deleted the one-time containment notice this counter used to
			//! also feed; the probe gate is its sole consumer now.)
			SurfaceSignalDemand::Registration m_signalDemand;

			//! COST gate, not a diagnostic one -- see ProximityDemand's doc
			//! comment (ISurfaceSignalProvider.h).  Active iff this
			//! painter's compiled body calls EITHER cross-object signal,
			//! `proximity()` or `interior()` (`prog.UsesCrossObject()`);
			//! both read the same world-AABB snapshot, so an
			//! `interior`-only scene must register too.  What it buys is
			//! that ObjectManager builds no snapshot and
			//! ObjectManager::IntersectRay checks for none when nothing in
			//! the process asks for either.
			ProximityDemand::Registration m_proximityDemand;

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
				m_signalDemand( prog.UsesSurfaceSignals() ),
				m_proximityDemand( prog.UsesCrossObject() )
			{
				// DL-25 round-3 review (P3-7): make the attach-time contract
				// STRUCTURAL rather than a convention.  A program that calls
				// `sample()` / `sample_scalar()` must have had
				// `BindPainterRefs` run on it BEFORE any painter is
				// constructed from it (the resolver lives one layer up, in
				// `Job::AddExpressionPainter` / `ScalarPainterAsciiChunkParser`
				// -- see BuildExpressionProgramFromChunkFields's own doc).  If
				// it has not, every `sample()` in the body silently reads the
				// honest-neutral 0/black for the life of the render instead of
				// failing anywhere a caller can see.  `IsPainterRefsBound()`
				// reports true trivially for a program with no sample calls,
				// so this costs nothing and constrains nothing for the
				// overwhelming majority of programs.
				assert( prog.IsPainterRefsBound() &&
					"ExpressionPainter built from a sample()-calling program whose painter refs were "
					"never bound -- call ExpressionProgram::BindPainterRefs at attach time" );
			}

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
			//! See ExpressionPainter::m_signalDemand -- same RAII gate (real
			//! work since S3, not diagnostic-only) on the physical-scalar
			//! pipe, active for the same five builtins (`occlusion()` /
			//! `thickness()` / `convexity()` / `proximity()` / `interior()`,
			//! via `prog.UsesSurfaceSignals()`).  `scalar_painter
			//! { expression "occlusion(0.1)" }` feeding a dirt/wear slot is at
			//! least as likely an authoring shape as the colour pipe's, so
			//! both must register or the `LightSampler::ProbeEmitterSurface`
			//! gate would miss it -- `proximity()` on this pipe is what a
			//! scalar wear/grime slot (`scalar_painter
			//! { expression "1-proximity(0.002)" }`) uses in practice, and
			//! since 2026-09-11 this is part of what opens that probe gate
			//! for a scalar emission slot keyed on a cross-object signal.
			SurfaceSignalDemand::Registration m_signalDemand;
			//! See ExpressionPainter::m_proximityDemand -- the same COST
			//! gate on the physical-scalar pipe, and it covers `interior()`
			//! as well through the same `UsesCrossObject()`.  Both pipes
			//! must register:
			//! `scalar_painter { expression "1-proximity(0.002)" }` feeding
			//! a roughness slot is exactly as likely an authoring shape as
			//! the colour pipe's, and a gate that missed it would skip the
			//! snapshot build for a scene that genuinely needs one (which
			//! the lazy path would then pay for under the lock).
			ProximityDemand::Registration m_proximityDemand;
			//! DL-09: the graded-index cost gate (GradedIndexMedium.h).
			//! Active iff this program is a world-position field, i.e.
			//! exactly when IsWorldPositionField() below answers true.
			GradedIndexDemand::Registration m_gradedDemand;
			virtual ~ExpressionScalarPainter() {}

			//! DL-09: the static test behind IsWorldPositionField() -- a
			//! SCALAR-typed body that reads the world position `P` and none
			//! of the surface-record inputs (`u`, `v`, `Po`, `N`, `fw`,
			//! `fwo`, `curv`, `curvR`), no surface signal and no `sample()`
			//! of another painter.  Resolved from the compiled program's
			//! context-variable mask, so it is a property of the program,
			//! not a per-hit check.  `time` is allowed: it is a fixed
			//! constant on this pipe (see the class comment).
			static bool IsWorldPositionProgram( const ExpressionProgram& prog )
			{
				if( prog.ResultType() != ExpressionProgram::kScalar ) return false;
				if( !prog.UsesContextVar( ExpressionProgram::kContextSlotP ) ) return false;
				if( prog.UsesContextVar( 0 ) || prog.UsesContextVar( 1 ) ) return false;				// u, v
				if( prog.UsesContextVar( ExpressionProgram::kContextSlotPo ) ) return false;
				if( prog.UsesContextVar( 8 ) ) return false;											// N
				if( prog.UsesContextVar( ExpressionProgram::kContextSlotFw ) ||
					prog.UsesContextVar( ExpressionProgram::kContextSlotFwo ) ) return false;
				if( prog.UsesSurfaceCurvature() ) return false;
				if( prog.UsesSurfaceSignals() || prog.UsesPainterSample() ) return false;
				return true;
			}

			static Scalar SafeComp( const Scalar v ) { return ExpressionProgram::IsFinite( v ) ? v : Scalar(0); }

			ExprEvalContext BuildContext( const RayIntersectionGeometric& ri ) const;

		public:
			ExpressionScalarPainter( const ExpressionProgram& prog, const std::vector<ParamSpec>& paramSpecs ) :
				m_prog( prog ), m_paramSpecs( paramSpecs ),
				m_curvatureDemand( prog.UsesSurfaceCurvature() ),
				m_signalDemand( prog.UsesSurfaceSignals() ),
				m_proximityDemand( prog.UsesCrossObject() ),
				m_gradedDemand( IsWorldPositionProgram( prog ) )
			{
				// DL-25 round-3 review (P3-7): make the attach-time contract
				// STRUCTURAL rather than a convention.  A program that calls
				// `sample()` / `sample_scalar()` must have had
				// `BindPainterRefs` run on it BEFORE any painter is
				// constructed from it (the resolver lives one layer up, in
				// `Job::AddExpressionPainter` / `ScalarPainterAsciiChunkParser`
				// -- see BuildExpressionProgramFromChunkFields's own doc).  If
				// it has not, every `sample()` in the body silently reads the
				// honest-neutral 0/black for the life of the render instead of
				// failing anywhere a caller can see.  `IsPainterRefsBound()`
				// reports true trivially for a program with no sample calls,
				// so this costs nothing and constrains nothing for the
				// overwhelming majority of programs.
				assert( prog.IsPainterRefsBound() &&
					"ExpressionScalarPainter built from a sample()-calling program whose painter refs were "
					"never bound -- call ExpressionProgram::BindPainterRefs at attach time" );
			}

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

			//! DL-09 (IScalarPainter::IsWorldPositionField): see
			//! IsWorldPositionProgram above.
			bool IsWorldPositionField() const override { return IsWorldPositionProgram( m_prog ); }
		};
	}
}

#endif
