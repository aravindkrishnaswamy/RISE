//////////////////////////////////////////////////////////////////////
//
//  CstRenderEquivalence.h - the render-equivalence harness (the pre-P0
//  regression oracle for the agentic-redesign migration).
//
//  The migration changes the parse -> Job mapping (legacy AsciiSceneParser ->
//  Job, vs the new CST -> derive -> Job). The same Job renders the same image,
//  so the deterministic, precise oracle for "did the CST path produce the same
//  scene?" is STRUCTURAL Job equivalence: parse via each path, dump the Job to a
//  canonical string, and compare. (Image equivalence adds RNG noise and catches
//  nothing the migration changes beyond what the Job already determines.)
//
//  This header is the reusable primitive: `DumpJob(job)` is the canonical
//  structural equivalence metric.  Two parse/derive paths that yield the same
//  dump produce the same scene (hence the same render).  The surviving CST
//  tests use it to pin CST-derive state (against the committed golden, and
//  against a CST re-derive of an edited/serialized Document).
//
//  NOTE (Model-B P5 Slice 6c-3b): the legacy `ParseLegacy(text, job)` entry
//  point was REMOVED when the legacy-vs-CST equivalence oracle was retired --
//  the golden (CstDeriveGoldenTest) is now the CST-derive safety net.  Do not
//  reintroduce a legacy-parser call here; the legacy streaming parser
//  (AsciiSceneParser::ParseAndLoadScene) is being deleted (6c-3c).
//
//////////////////////////////////////////////////////////////////////
#ifndef RISE_TESTS_CST_RENDER_EQUIVALENCE_H
#define RISE_TESTS_CST_RENDER_EQUIVALENCE_H

#include <fstream>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <set>

#include "../src/Library/Job.h"
#include "../src/Library/RISE_API.h"
#include "../src/Library/Interfaces/IGeometry.h"
#include "../src/Library/Interfaces/IGeometryManager.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
#include "../src/Library/Interfaces/IEmitter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
#include "../src/Library/Interfaces/IScalarPainter.h"
#include "../src/Library/Interfaces/IScalarPainterManager.h"
#include "../src/Library/Interfaces/IRayIntersectionModifier.h"
#include "../src/Library/Interfaces/IModifierManager.h"
#include "../src/Library/Interfaces/IObject.h"
#include "../src/Library/Interfaces/IObjectManager.h"
#include "../src/Library/Interfaces/ICamera.h"
#include "../src/Library/Interfaces/ICameraManager.h"
#include "../src/Library/Interfaces/ILightManager.h"
#include "../src/Library/Interfaces/IMedium.h"
#include "../src/Library/Interfaces/IPhaseFunction.h"
#include "../src/Library/Interfaces/IScene.h"
#include "../src/Library/Interfaces/IScenePriv.h"
#include "../src/Library/Interfaces/IRadianceMap.h"
#include "../src/Library/Interfaces/IFilm.h"
#include "../src/Library/Interfaces/IEnumCallback.h"
#include "../src/Library/Utilities/BoundingBox.h"

// Concrete painter / scalar-painter / modifier classes -- needed for the
// composition-digest dynamic_cast chain below (DumpScalarPainterComposition,
// DumpPainterComposition, DumpModifierComposition).  Test-only dependency on
// concrete Implementation:: classes (not just interfaces) is deliberate here:
// the whole point of the digest is to tell two DIFFERENT concrete classes
// bound to the same painter NAME apart (see the file-header comment below).
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "../src/Library/Painters/AddScalarPainter.h"
#include "../src/Library/Painters/MultiplyScalarPainter.h"
#include "../src/Library/Painters/ScaledScalarPainter.h"
#include "../src/Library/Painters/PainterChannelScalarPainter.h"
#include "../src/Library/Painters/TextureScalarPainter.h"
#include "../src/Library/Painters/PiecewiseLinearScalarPainter.h"
#include "../src/Library/Painters/PolynomialScalarPainter.h"
#include "../src/Library/Painters/RGBScalarPainter.h"
#include "../src/Library/Painters/SellmeierScalarPainter.h"
#include "../src/Library/Painters/ExpressionPainter.h"		// ExpressionPainter + ExpressionScalarPainter
#include "../src/Library/Painters/BlendPainter.h"
#include "../src/Library/Painters/RampPainter.h"
#include "../src/Library/Modifiers/ModifierStack.h"
#include "../src/Library/Modifiers/NormalMap.h"
#include "../src/Library/Modifiers/GlintModifier.h"
#include "../src/Library/Modifiers/ReliefModifier.h"

namespace risequiv {

using namespace RISE;

// Collects item names from a manager's EnumerateItemNames callback.
struct NameCollector : public IEnumCallback<const char*> {
	std::vector<std::string> names;
	bool operator()( const char* const& n ) override { names.push_back( n ? n : "" ); return true; }
};
template <typename Mgr>
inline std::vector<std::string> SortedNames( Mgr* m )
{
	NameCollector c;
	if( m ) m->EnumerateItemNames( c );
	std::sort( c.names.begin(), c.names.end() );
	return c.names;
}

// Reverse-lookup an item's registered name in its manager (managers map
// name->item; GetItem borrows -- no addref, see GenericManager::GetItem). Used
// to dump an object's REFERENCE WIRING (which geometry / material it points at)
// as discriminating state: a CST derive that binds the wrong reference shows up
// as a changed name here.
template <typename Mgr, typename Item>
inline std::string ReverseName( Mgr* m, const Item* ptr )
{
	if( !ptr ) return "(none)";
	if( !m )   return "(unknown)";
	NameCollector c; m->EnumerateItemNames( c );
	for( const auto& n : c.names )
		if( static_cast<const Item*>( m->GetItem( n.c_str() ) ) == ptr ) return n;
	return "(unknown)";
}

// Dump a medium's discriminating, cheaply-readable state: coefficients sampled at an ASYMMETRIC interior point
// of the bbox for a bounded/heterogeneous medium (so the sample is neither vacuum nor a lattice tie -- see the
// probe-point comment below; homogeneous media ignore the point), the phase asymmetry g (GetMeanCosine),
// homogeneity, and the world bbox when bounded (placement). The spatial FIELD of a heterogeneous medium beyond
// that one sample is still by-construction (a render spot-check covers it).
inline void DumpMedium( std::ostream& o, const IMedium* m )
{
	Point3 bbMin, bbMax; const bool bounded = m->GetBoundingBox( bbMin, bbMax );
	// PROBE POINT: an ASYMMETRIC interior fraction, deliberately NOT the bbox
	// centre.  The centre is a symmetry point, and for scenes with round-numbered
	// geometry it lands ON the procedural lattice: pt_painter_simplex3d_grid's
	// med_r1c3 (bbox centre 150,60,0 x scale 0.05) probes simplex noise at exactly
	// (7.5, 3, 0), where the gradient contributions cancel to EXACTLY 0 in strict
	// IEEE -- density exactly 0.5 -- but not under macOS's -ffast-math, which
	// reassociates the sum.  That made this one scene's digest platform-dependent
	// (the 2026-08-21 Mac-vs-Windows golden DRIFT), and no print precision can fix
	// a knife-edge tie.  These fractions are irrational-ish and share no common
	// factor, so the probe misses lattice/simplex ties generically -- which also
	// makes it a STRONGER discriminator: a probe sitting where noise is
	// identically zero cannot tell two different density painters apart.
	const Point3 sp = bounded
		? Point3( bbMin.x + ( bbMax.x - bbMin.x ) * 0.4703,
		          bbMin.y + ( bbMax.y - bbMin.y ) * 0.5279,
		          bbMin.z + ( bbMax.z - bbMin.z ) * 0.4391 )
		: Point3( 0, 0, 0 );
	const MediumCoefficients c = m->GetCoefficients( sp ); const IPhaseFunction* pf = m->GetPhaseFunction();
	char b[480];
	std::snprintf( b, sizeof(b), " sigma_t=[%.9g %.9g %.9g] sigma_s=[%.9g %.9g %.9g] emission=[%.9g %.9g %.9g] g=%.9g homog=%d",
		(double)c.sigma_t.r,(double)c.sigma_t.g,(double)c.sigma_t.b, (double)c.sigma_s.r,(double)c.sigma_s.g,(double)c.sigma_s.b,
		(double)c.emission.r,(double)c.emission.g,(double)c.emission.b, (double)( pf ? pf->GetMeanCosine() : 0 ), m->IsHomogeneous()?1:0 );
	o << b;
	if( bounded ) {
		char bb[224]; std::snprintf( bb, sizeof(bb), " bbox=[%.9g %.9g %.9g .. %.9g %.9g %.9g]",
			(double)bbMin.x,(double)bbMin.y,(double)bbMin.z, (double)bbMax.x,(double)bbMax.y,(double)bbMax.z ); o << bb;
	}
}

// Reverse-lookup a medium pointer to its registered name (media use Job::GetMedium/EnumerateMediumNames,
// not a GenericManager, so this mirrors ReverseName for that surface). Used by global= and interior_medium.
inline std::string ReverseMediumName( Job& job, const IMedium* m )
{
	if( !m ) return "(none)";
	NameCollector c; job.EnumerateMediumNames( c ); std::sort( c.names.begin(), c.names.end() );
	for( const auto& n : c.names ) if( job.GetMedium( n.c_str() ) == m ) return n;
	return "(unnamed)";
}
// Dump a radiance map's cheaply-readable state: bound painter (reverse-named), scale, and the full 4x4
// transform (encodes radiance_orient). Shared by the scene global_rmap and per-object radiance_map.
inline void DumpRadianceMap( std::ostream& o, const IRadianceMap* rm, Job& job )
{
	o << "painter=" << ReverseName( job.GetPainters(), &rm->GetPainter() );
	const Matrix4& t = rm->GetTransform(); char b[640];
	std::snprintf( b, sizeof(b), " scale=%.9g xform=[%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g]",
		(double)rm->GetScale(), (double)t._00,(double)t._01,(double)t._02,(double)t._03, (double)t._10,(double)t._11,(double)t._12,(double)t._13,
		(double)t._20,(double)t._21,(double)t._22,(double)t._23, (double)t._30,(double)t._31,(double)t._32,(double)t._33 ); o << b;
}

// ---------------------------------------------------------------------
// COMPOSITION DIGEST (2026-09-06).  A painter/scalar-painter/modifier NAME
// used to be the ENTIRE discriminator DumpJob recorded for that slot --
// swapping the CONCRETE OBJECT bound to a name (e.g. `scalar_painter {
// multiply a b }` -> `scalar_painter { add a b weight_a 1 weight_b 0.5 }`,
// same name, different composition and different weights) changed nothing
// in the dump.  Verified: scenes/FeatureBased/Textures/plank_closeup.RISEscene
// commit 85a6da5f, 0 DRIFT despite `sp_plank_rough`'s definition changing
// from `multiply` to `add ... weight_b 0.5`.  Worse: `IScalarPainter`s were
// not enumerated AT ALL (no `scalar_painters:` section existed), so a named
// scalar painter reachable only through a material slot -- `sp_plank_rough`
// bound to a GGX material's `alphax`/`alphay` -- had NO representation in
// the dump whatsoever, name or otherwise.
//
// Fix: every IScalarPainter / IPainter / IRayIntersectionModifier reachable
// from a material/modifier slot (or simply registered by name -- the new
// `scalar_painters:` / `modifiers:` top-level sections below enumerate the
// full named registry, independent of which material/object currently
// binds a given name) is digested by its CONCRETE COMPOSITION, recursively,
// in two complementary parts:
//
//   1. A KIND LABEL for the composition operators this pass knows about
//      (scalar side: uniform, rgb, sellmeier, polynomial, piecewise,
//      add + weights, multiply, scaled, painter-channel, texture,
//      expression + params; colour side: blend + mode, ramp + stops +
//      interpolation, expression + params), found via a dynamic_cast
//      chain, with operand(s) recursively digested the same way.  These
//      are HAND-PICKED LABEL STRINGS, not typeid/RTTI names: the golden is
//      a COMMITTED, CROSS-PLATFORM file (see the %.9g precision comment
//      below for the same cross-platform concern) and typeid(*p).name()
//      is Itanium-mangled on clang/gcc but MSVC-decorated on Windows --
//      using it here would make every painter's digest drift across
//      platforms independent of any real derive difference.
//
//   2. A PROBE FINGERPRINT: the composed VALUE at one fixed, deliberately
//      ASYMMETRIC point (same rationale as DumpMedium's probe point above
//      -- avoids landing on a procedural field's symmetric lattice tie,
//      see that comment for the concrete failure mode).  Printed for
//      EVERY painter/scalar-painter regardless of whether part 1 above
//      recognizes its concrete type, so a kind this dynamic_cast chain
//      does not (yet) special-case -- Function1D/Function2DScalarPainter's
//      wrapped IFunction1D/IFunction2D, and every IPainter kind besides
//      Blend/Ramp/Expression -- still gets a REAL discriminator instead of
//      silently staying name-only.  The red-proof this change ships with
//      (plank_closeup's `weight_b` 0.5 -> 0.6) is caught by THIS half even
//      in isolation, since it changes AddScalarPainter's composed output
//      at the probe point; part 1 additionally makes the digest READABLE
//      (a reviewer can see "add(weight_a=1 weight_b=0.6 ...)" rather than
//      inferring a composition change from a changed float).
//
// CYCLE PROTECTION is defensive-only.  Modifier/painter names resolve
// through their manager at PARSE time (Job::AddModifierStack, the
// scalar_painter/painter chunk parsers), so a name cannot reference itself
// or a not-yet-registered later name -- ModifierStack.h's file header
// spells this out for modifiers ("SELF-REFERENCE IS IMPOSSIBLE ... there
// is no cycle to detect at runtime") and the same construction-order
// argument holds for painters.  The visited-pointer set below is kept
// anyway, matching this codebase's belt-and-suspenders convention
// elsewhere (e.g. the NaN-sentinel ban outliving the -ffast-math fix that
// made it currently unreachable): it makes a future authoring path that
// DOES allow forward references fail safely (prints "(cycle)") instead of
// stack-overflowing the test.
// ---------------------------------------------------------------------

// A stable, fixed, deliberately ASYMMETRIC probe point -- the SAME
// fractions DumpMedium uses for its bbox-interior probe (0.4703 / 0.5279 /
// 0.4391), reused here as raw UV / world / object-space coordinates
// (painters have no bbox to take a fraction of).  The probe ray arrives
// along -Z at that point with a +Z normal, a plausible "front-facing hit"
// a painter's GetColor/GetColorNM/GetAlpha (or a scalar painter's
// GetValuesAt/GetValueAtNM) can be evaluated at with no null/NaN traps:
// every RayIntersectionGeometric field a painter might read (ptCoord,
// ptIntersection, ptObjIntersec, vNormal, vGeomNormal, onb, signals) is
// either explicitly set here or safely default-constructed (onb defaults
// to the world-axis identity basis; signals defaults to its documented
// neutral values).
inline RayIntersectionGeometric MakePainterProbeRi()
{
	const Point3 pt( 0.4703, 0.5279, 0.4391 );
	RayIntersectionGeometric ri( Ray( pt, Vector3( 0, 0, -1 ) ), nullRasterizerState );
	ri.ptCoord        = Point2( 0.4703, 0.5279 );
	ri.ptIntersection = pt;
	ri.ptObjIntersec  = pt;
	ri.vNormal        = Vector3( 0, 0, 1 );
	ri.vGeomNormal    = ri.vNormal;
	return ri;
}

// Mutually-recursive forward declarations: PainterChannelScalarPainter
// wraps an IPainter (scalar -> needs to call the painter dumper);
// nothing currently makes an IPainter wrap an IScalarPainter, but the
// forward declaration costs nothing and keeps the pair callable either
// way if that ever changes.
inline void DumpScalarPainterComposition( std::ostream& o, const IScalarPainter* p, std::set<const void*>& visited, const RayIntersectionGeometric& probe );
inline void DumpPainterComposition( std::ostream& o, const IPainter* p, std::set<const void*>& visited, const RayIntersectionGeometric& probe );

inline void DumpScalarPainterComposition( std::ostream& o, const IScalarPainter* p, std::set<const void*>& visited, const RayIntersectionGeometric& probe )
{
	if( !p ) { o << "(none)"; return; }
	if( !visited.insert( p ).second ) { o << "(cycle)"; return; }

	// Part 2: probe fingerprint, unconditional (see file comment above).
	const ScalarTriple t = p->GetValuesAt( probe );
	char b[256];
	std::snprintf( b, sizeof(b), "probe=[%.9g %.9g %.9g @450=%.9g @550=%.9g @650=%.9g]",
		(double)t.v[0], (double)t.v[1], (double)t.v[2],
		(double)p->GetValueAtNM( probe, 450 ), (double)p->GetValueAtNM( probe, 550 ), (double)p->GetValueAtNM( probe, 650 ) );
	o << b;

	// Part 1: kind label + structural fields, for the composition
	// operators this pass knows about.
	using namespace RISE::Implementation;
	if( const UniformScalarPainter* u = dynamic_cast<const UniformScalarPainter*>( p ) ) {
		char v[64]; std::snprintf( v, sizeof(v), " uniform(value=%.9g)", (double)u->GetValue() ); o << v;
	} else if( const RGBScalarPainter* rgb = dynamic_cast<const RGBScalarPainter*>( p ) ) {
		char v[96]; std::snprintf( v, sizeof(v), " rgb(r=%.9g g=%.9g b=%.9g)", (double)rgb->GetR(), (double)rgb->GetG(), (double)rgb->GetB() ); o << v;
	} else if( const SellmeierScalarPainter* s = dynamic_cast<const SellmeierScalarPainter*>( p ) ) {
		char v[192]; std::snprintf( v, sizeof(v), " sellmeier(B1=%.9g B2=%.9g B3=%.9g C1=%.9g C2=%.9g C3=%.9g)",
			(double)s->GetB1(), (double)s->GetB2(), (double)s->GetB3(), (double)s->GetC1(), (double)s->GetC2(), (double)s->GetC3() ); o << v;
	} else if( const PolynomialScalarPainter* poly = dynamic_cast<const PolynomialScalarPainter*>( p ) ) {
		o << " polynomial(coeffs=[";
		const std::vector<Scalar>& c = poly->GetCoeffs();
		for( size_t i = 0; i < c.size(); ++i ) { char v[32]; std::snprintf( v, sizeof(v), "%s%.9g", i?" ":"", (double)c[i] ); o << v; }
		o << "])";
	} else if( const PiecewiseLinearScalarPainter* pw = dynamic_cast<const PiecewiseLinearScalarPainter*>( p ) ) {
		o << " piecewise(samples=[";
		const std::vector<PiecewiseLinearScalarPainter::Sample>& s = pw->GetSamples();
		for( size_t i = 0; i < s.size(); ++i ) { char v[48]; std::snprintf( v, sizeof(v), "%s(%.9g,%.9g)", i?" ":"", (double)s[i].nm, (double)s[i].value ); o << v; }
		o << "])";
	} else if( const AddScalarPainter* add = dynamic_cast<const AddScalarPainter*>( p ) ) {
		char v[64]; std::snprintf( v, sizeof(v), " add(weight_a=%.9g weight_b=%.9g a=", (double)add->GetWeightA(), (double)add->GetWeightB() ); o << v;
		DumpScalarPainterComposition( o, add->GetA(), visited, probe );
		o << " b="; DumpScalarPainterComposition( o, add->GetB(), visited, probe ); o << ")";
	} else if( const MultiplyScalarPainter* mul = dynamic_cast<const MultiplyScalarPainter*>( p ) ) {
		o << " multiply(a="; DumpScalarPainterComposition( o, mul->GetA(), visited, probe );
		o << " b="; DumpScalarPainterComposition( o, mul->GetB(), visited, probe ); o << ")";
	} else if( const ScaledScalarPainter* sc = dynamic_cast<const ScaledScalarPainter*>( p ) ) {
		char v[32]; std::snprintf( v, sizeof(v), " scaled(scale=%.9g child=", (double)sc->GetScale() ); o << v;
		DumpScalarPainterComposition( o, sc->GetChild(), visited, probe ); o << ")";
	} else if( const PainterChannelScalarPainter* pc = dynamic_cast<const PainterChannelScalarPainter*>( p ) ) {
		char v[96]; std::snprintf( v, sizeof(v), " painterchannel(channel=%d scale=%.9g bias=%.9g source=",
			(int)pc->GetChannel(), (double)pc->GetScale(), (double)pc->GetBias() ); o << v;
		DumpPainterComposition( o, &pc->GetSource(), visited, probe ); o << ")";
	} else if( const TextureScalarPainter* tex = dynamic_cast<const TextureScalarPainter*>( p ) ) {
		IRasterImageAccessor* ria = tex->GetAccessor();
		char v[128]; std::snprintf( v, sizeof(v), " texture(channel=%d scale=%.9g bias=%.9g dims=%ux%u)",
			(int)tex->GetChannel(), (double)tex->GetScale(), (double)tex->GetBias(),
			ria ? ria->GetWidth() : 0u, ria ? ria->GetHeight() : 0u ); o << v;
	} else if( const ExpressionScalarPainter* ex = dynamic_cast<const ExpressionScalarPainter*>( p ) ) {
		o << " expression(params=[";
		const std::vector<ParamSpec>& specs = ex->GetParamSpecs();
		for( size_t i = 0; i < specs.size(); ++i ) {
			char v[160]; std::snprintf( v, sizeof(v), "%s%s=%.9g", i?" ":"", specs[i].name.c_str(), (double)specs[i].value ); o << v;
		}
		o << "])";
	} else {
		// Function1D/Function2DScalarPainter (opaque IFunction1D/IFunction2D
		// wrapper -- no concrete-type audit here, see file comment) and any
		// future kind this chain hasn't been extended for yet: the probe
		// fingerprint above is still a real, non-name-only discriminator.
		o << " opaque";
	}
}

inline void DumpPainterComposition( std::ostream& o, const IPainter* p, std::set<const void*>& visited, const RayIntersectionGeometric& probe )
{
	if( !p ) { o << "(none)"; return; }
	if( !visited.insert( p ).second ) { o << "(cycle)"; return; }

	// Part 2: probe fingerprint, unconditional (see file comment above).
	const RISEPel c = p->GetColor( probe );
	char b[256];
	std::snprintf( b, sizeof(b), "probe=[%.9g %.9g %.9g @450=%.9g @550=%.9g @650=%.9g alpha=%.9g]",
		(double)c.r, (double)c.g, (double)c.b,
		(double)p->GetColorNM( probe, 450 ), (double)p->GetColorNM( probe, 550 ), (double)p->GetColorNM( probe, 650 ),
		(double)p->GetAlpha( probe ) );
	o << b;

	// Part 1: kind label + structural fields -- "ramp stops" and "blend
	// painters" are the two IPainter forms the review named explicitly;
	// expression_painter mirrors the scalar side's expression coverage.
	using namespace RISE::Implementation;
	if( const BlendPainter* bl = dynamic_cast<const BlendPainter*>( p ) ) {
		char v[32]; std::snprintf( v, sizeof(v), " blend(mode=%d a=", (int)bl->GetMode() ); o << v;
		DumpPainterComposition( o, &bl->GetA(), visited, probe );
		o << " b="; DumpPainterComposition( o, &bl->GetB(), visited, probe );
		o << " mask="; DumpPainterComposition( o, &bl->GetMask(), visited, probe );
		o << ")";
	} else if( const RampPainter* rp = dynamic_cast<const RampPainter*>( p ) ) {
		char v[64]; std::snprintf( v, sizeof(v), " ramp(channel=%d interp=%d stops=[", (int)rp->GetChannel(), (int)rp->GetInterpolation() ); o << v;
		for( std::size_t i = 0; i < rp->StopCount(); ++i ) {
			const RISEPel sc = rp->StopColor(i);
			char sv[96]; std::snprintf( sv, sizeof(sv), "%s(%.9g:%.9g,%.9g,%.9g)", i?" ":"", (double)rp->StopPos(i), (double)sc.r, (double)sc.g, (double)sc.b ); o << sv;
		}
		o << "] input="; DumpPainterComposition( o, &rp->GetInput(), visited, probe ); o << ")";
	} else if( const ExpressionPainter* ex = dynamic_cast<const ExpressionPainter*>( p ) ) {
		o << " expression(params=[";
		const std::vector<ParamSpec>& specs = ex->GetParamSpecs();
		for( size_t i = 0; i < specs.size(); ++i ) {
			char v[160]; std::snprintf( v, sizeof(v), "%s%s=%.9g", i?" ":"", specs[i].name.c_str(), (double)specs[i].value ); o << v;
		}
		o << "])";
	} else {
		// Every other IPainter kind (procedural noise fields, checker/
		// mapping/texture/uniform-color and the rest of the ~30-strong
		// family): no concrete-type audit here yet, see file comment --
		// the probe fingerprint above is still a real discriminator.
		o << " opaque";
	}
}

// Dump a modifier's (IRayIntersectionModifier) composition: kind label +
// structural fields, recursing into a ModifierStack's members and into
// any painter/scalar-painter field the same way the painter dumpers above
// do.  Covers the review's explicit "modifiers on objects, modifier_stack
// members" call-out.
inline void DumpModifierComposition( std::ostream& o, const IRayIntersectionModifier* m, std::set<const void*>& visited, const RayIntersectionGeometric& probe )
{
	if( !m ) { o << "(none)"; return; }
	if( !visited.insert( m ).second ) { o << "(cycle)"; return; }

	using namespace RISE::Implementation;
	if( const ModifierStack* st = dynamic_cast<const ModifierStack*>( m ) ) {
		char v[32]; std::snprintf( v, sizeof(v), "stack(count=%u members=[", st->MemberCount() ); o << v;
		for( unsigned int i = 0; i < st->MemberCount(); ++i ) {
			if( i ) o << " ";
			DumpModifierComposition( o, st->Member(i), visited, probe );
		}
		o << "])";
	} else if( const NormalMap* nm = dynamic_cast<const NormalMap*>( m ) ) {
		char v[32]; std::snprintf( v, sizeof(v), "normalmap(scale=%.9g painter=", (double)nm->GetScale() ); o << v;
		DumpPainterComposition( o, &nm->GetPainter(), visited, probe ); o << ")";
	} else if( const GlintModifier* gm = dynamic_cast<const GlintModifier*>( m ) ) {
		char v[320]; std::snprintf( v, sizeof(v),
			"glint(density=%.9g coverage=%.9g fill=%.9g spread=%.9g vscale=[%.9g %.9g %.9g] vshift=[%.9g %.9g %.9g] seed=%u)",
			(double)gm->GetDensity(), (double)gm->GetCoverage(), (double)gm->GetFill(), (double)gm->GetSpreadRad(),
			(double)gm->GetVScale().x, (double)gm->GetVScale().y, (double)gm->GetVScale().z,
			(double)gm->GetVShift().x, (double)gm->GetVShift().y, (double)gm->GetVShift().z,
			gm->GetSeed() ); o << v;
	} else if( const ReliefModifier* rm = dynamic_cast<const ReliefModifier*>( m ) ) {
		char v[160]; std::snprintf( v, sizeof(v), "relief(scale=%.9g domain=%d step=%.9g maxslope=%.9g height=",
			(double)rm->GetScale(), (int)rm->GetDomain(), (double)rm->GetStep(), (double)rm->GetMaxSlope() ); o << v;
		DumpScalarPainterComposition( o, &rm->GetHeight(), visited, probe ); o << ")";
	} else {
		o << "opaque";
	}
}

// Canonical structural dump of a Job -- the equivalence metric. Two parse paths
// that yield the same dump produce the same scene (hence the same render). Sorted
// per manager for stability. Numeric fields use %.9g -- 9 significant digits,
// NOT the lossless %.17g this harness originally used (changed 2026-08-18).
// The change makes CstDeriveGoldenTest's committed digests CROSS-PLATFORM:
// macOS builds with -ffast-math (Config.OSX) while MSVC/Linux are strict IEEE,
// so DERIVED values (bounding-sphere radii via sqrt, world bboxes through
// transform chains) differ between platforms in the last 1-2 ulps -- at %.17g
// that ULP noise drifted 178 of 383 golden digests on the first Windows run.
// 9 digits sits ~5 orders of magnitude above that noise floor and still far
// below anything a real derive regression produces.  Trade-off, accepted
// deliberately: two values that differ by less than ~1e-9 RELATIVE now
// collide into an equal dump -- fine for the in-process equivalence tests,
// which compare parses of the SAME ASCII decimals (differences are
// exact-equal or gross).  Residual risk: a derived value sitting within ulps
// of a 9-digit rounding boundary can still round differently across
// platforms (expected << 1 occurrence corpus-wide).  If a single-scene
// cross-platform DRIFT ever appears with matching line counts and a ~0 byte
// delta, THIS is the cause -- review + regenerate, don't debug the derive.
//
// GATE-MAINTENANCE RULE (per the item-2 review): as each new DERIVED type lands,
// this dump MUST gain that type's discriminating state (reference bindings,
// parameter values, transforms, ordering) in the SAME change -- otherwise the
// oracle silently certifies different scenes as equal.
//
// Item 5 derives ALL registry chunk types through the SAME parser Finalize the
// legacy path uses, so a CST-vs-legacy Job can differ ONLY in (a) the param
// VALUES fed to Finalize or (b) the chunk set/order. This dump discriminates a
// value divergence WHEREVER the value reaches a dumped field -- geometry
// bounding-sphere radius; OBJECT reference wiring (geometry + material names) and
// world-space bounding box (encodes position / scale / geometry size) -- and the
// chunk set/order always. (A value that reaches no cheaply-readable interface field -- material IOR, camera
// intrinsics, the accelerator choice, the light-RR threshold -- is not surfaced here; those stay covered by
// the by-construction argument below + a Phase-B render spot-check (docs/agentic-redesign/61-...). The
// cheaply-readable, render-discriminating values ARE now surfaced DIRECTLY (caught here, not merely argued):
// DELTA-light (ILight) power/photons; AREA-light luminaire-material emitter exitance (IMaterial::GetEmitter
// ->averageRadiantExitance -- now DETERMINISTIC since the RefreshAverages stratified-grid fix; it formerly
// sampled the painter via GlobalRNG at construction and falsely flagged the gltf-import scenes); medium
// coefficients + phase-g + placement; per-object radiance-map (painter/scale/transform) + interior-medium
// name; the scene global radiance map + film dims. What STAYS by-construction (no cheaply + DETERMINISTICALLY
// readable Job field -> the render spot-check): material IOR, camera intrinsics, the accelerator, the RR
// threshold, rasterizer flags (radiance_background), a heterogeneous medium's spatial field beyond the
// bbox-centre sample. A Hosek/procedural SKY global-rmap is a partial case: its
// painter/scale/transform ARE dumped, but the painter is an internal unregistered adapter (reverse-names to
// (unknown)) and its dome params (solar elevation/azimuth, turbidity, ground albedo) need an eval context, so only those
// dome params stay by-construction. The multi-token value path that feeds
// painter colour / material scalar state is covered END-TO-END here too:
// an object's `position`/`scale` are multi-token DoubleVec3 values, so a
// multi-token mis-capture moves the world bbox and fails the CST-vs-legacy
// comparison (CstDescriptorBindTest [equiv]); its [multitoken] ParamValue
// assertions additionally pin the CST-side capture.
//
// UPDATED 2026-09-06: painters and scalar painters no longer stay
// names-only.  The prior paragraph's "buys nothing over the position/bbox
// check" argument assumed the only failure mode was a mis-CAPTURED param
// value reaching an otherwise-identical Finalize call -- it did not cover
// a scene author (or an agent) swapping WHICH COMPOSITION a name is bound
// to (`scalar_painter { multiply a b }` -> `{ add a b weight_a 1 weight_b
// 0.5 }`), which is a real derive-affecting difference no CST param-capture
// test catches, since both forms are validly-parsed, differently-shaped
// chunks.  See the composition-digest block above DumpJob
// (DumpScalarPainterComposition / DumpPainterComposition /
// DumpModifierComposition) and the new `scalar_painters:` / `modifiers:`
// sections below -- `painters:` now also dumps each painter's composition,
// not just its name.  Material slots that hold an IScalarPainter (GGX
// alphax/alphay, IOR, etc.) are still not dumped AT the material entry
// (each material family exposes different fields with no generic
// interface to read them from) but the scalar painter itself, by NAME, now
// is -- fully covering the reported blind spot (plank_closeup's
// `sp_plank_rough`) since a material always binds a scalar slot to a
// NAMED scalar_painter, never an anonymous one.
inline std::string DumpJob( Job& job )
{
	std::ostringstream o;
	o << "geometries:\n";
	for( const auto& n : SortedNames( job.GetGeometries() ) ) {
		o << "  " << n;
		IGeometry* g = job.GetGeometries() ? job.GetGeometries()->GetItem( n.c_str() ) : 0;
		if( g ) { Point3 c; Scalar r = 0; g->GenerateBoundingSphere( c, r ); char b[64]; std::snprintf( b, sizeof(b), " bsphere=%.9g", (double)r ); o << b; }
		o << "\n";
	}
	o << "materials:\n";
	for( const auto& n : SortedNames( job.GetMaterials() ) ) {
		o << "  " << n; IMaterial* mat = job.GetMaterials() ? job.GetMaterials()->GetItem( n.c_str() ) : 0;
		if( mat && mat->GetEmitter() ) { const RISEPel e = mat->GetEmitter()->averageRadiantExitance(); char eb[96]; std::snprintf( eb, sizeof(eb), " emitter=[%.9g %.9g %.9g]", (double)e.r,(double)e.g,(double)e.b ); o << eb; }
		o << "\n";
	}
	// Composition digest probe point, shared by every painter/scalar-painter/
	// modifier dump below (see the file comment above DumpScalarPainterComposition).
	const RayIntersectionGeometric probeRi = MakePainterProbeRi();
	o << "painters:\n";
	for( const auto& n : SortedNames( job.GetPainters() ) ) {
		o << "  " << n << " ";
		IPainter* pt = job.GetPainters() ? job.GetPainters()->GetItem( n.c_str() ) : 0;
		std::set<const void*> visited;
		DumpPainterComposition( o, pt, visited, probeRi );
		o << "\n";
	}
	// Scalar painters (IScalarPainter) -- their OWN named registry, separate
	// from IPainter's (see IScalarPainterManager.h).  Previously not
	// enumerated by DumpJob at all: a scalar painter reachable only through
	// a material slot (e.g. GGX `alphax`/`alphay`) had zero representation
	// in the dump.  See the composition-digest file comment above.
	o << "scalar_painters:\n";
	for( const auto& n : SortedNames( job.GetScalarPainters() ) ) {
		o << "  " << n << " ";
		IScalarPainter* sp = job.GetScalarPainters() ? job.GetScalarPainters()->GetItem( n.c_str() ) : 0;
		std::set<const void*> visited;
		DumpScalarPainterComposition( o, sp, visited, probeRi );
		o << "\n";
	}
	// Modifiers (IRayIntersectionModifier) -- named registry, mirrors
	// painters/scalar_painters above.  Covers modifier_stack members too:
	// each stack member is ALSO enumerated here by its own name (a member
	// must be a previously-registered name -- see ModifierStack.h's
	// "SELF-REFERENCE IS IMPOSSIBLE" comment), and the stack's own dump
	// recurses into every member's composition inline as well.
	o << "modifiers:\n";
	for( const auto& n : SortedNames( job.GetModifiers() ) ) {
		o << "  " << n << " ";
		IRayIntersectionModifier* md = job.GetModifiers() ? job.GetModifiers()->GetItem( n.c_str() ) : 0;
		std::set<const void*> visited;
		DumpModifierComposition( o, md, visited, probeRi );
		o << "\n";
	}
	o << "objects:\n";
	for( const auto& n : SortedNames( job.GetObjects() ) ) {
		o << "  " << n;
		IObject* ob = job.GetObjects() ? job.GetObjects()->GetItem( n.c_str() ) : 0;
		if( ob ) {
			o << " geometry=" << ReverseName( job.GetGeometries(), ob->GetGeometry() );
			o << " material=" << ReverseName( job.GetMaterials(),  ob->GetMaterial() );
			o << " modifier="; { std::set<const void*> visited; DumpModifierComposition( o, ob->GetModifier(), visited, probeRi ); }
			o << " shader=" << ReverseName( job.GetShaders(), ob->GetShader() );
			o << " radiance_map="; if( ob->GetRadianceMap() ) DumpRadianceMap( o, ob->GetRadianceMap(), job ); else o << "(none)";
			o << " interior_medium=" << ReverseMediumName( job, ob->GetInteriorMedium() );
			o << " visible=" << ( ob->IsWorldVisible() ? "1" : "0" );
			BoundingBox bb = ob->getBoundingBox();
			char b[160];
			std::snprintf( b, sizeof(b), " bbox=[%.9g %.9g %.9g .. %.9g %.9g %.9g]",
				(double)bb.ll.x, (double)bb.ll.y, (double)bb.ll.z,
				(double)bb.ur.x, (double)bb.ur.y, (double)bb.ur.z );
			o << b;
		}
		o << "\n";
	}
	// Cameras: name + world location. Names surface the camera-name dedup state
	// (an unnamed camera auto-names default / default_1 / ...; a cross-parse leak
	// of that state renames it), and location surfaces extrinsic/unit leaks --
	// the cross-derive parse-state vectors DumpJob would otherwise miss (it has
	// no camera section). Intrinsics (sensor/focal/fstop) are not on the ICamera
	// contract, so a pure-FOV leak is not surfaced here; the derive clears ALL
	// parser state regardless (ClearChunkParserState). So the cross-derive
	// leak vectors with a Job-observable manifestation -- painter-colour
	// (spurious energy-auto-scaled painters) AND camera-name dedup (an
	// unnamed camera renamed default -> default_1) -- are BOTH surfaced here
	// (what CstDeriveDifferentialTest's cross-derive cases assert).
	o << "cameras:\n";
	for( const auto& n : SortedNames( job.GetCameras() ) ) {
		o << "  " << n;
		ICamera* c = job.GetCameras() ? job.GetCameras()->GetItem( n.c_str() ) : 0;
		if( c ) { Point3 p = c->GetLocation(); char b[96]; std::snprintf( b, sizeof(b), " loc=[%.9g %.9g %.9g]", (double)p.x, (double)p.y, (double)p.z ); o << b; }
		o << "\n";
	}
	// --- scene singletons reachable via job.GetScene() (audit-by-bug-pattern: the global-medium sibling set).
	o << "scene:\n";
	{
		IScenePriv* scp = job.GetScene(); const IFilm* fl = scp ? scp->GetFilm() : 0; char fb[96];
		if( fl ) { std::snprintf( fb, sizeof(fb), "  film=[%u x %u par=%.9g]", fl->GetWidth(), fl->GetHeight(), (double)fl->GetPixelAR() ); o << fb << "\n"; } else o << "  film=(none)\n";
		const IRadianceMap* grm = scp ? scp->GetGlobalRadianceMap() : 0;
		o << "  global_rmap="; if( grm ) DumpRadianceMap( o, grm, job ); else o << "(none)"; o << "\n";
		o << "  active_camera=" << job.GetActiveCameraName() << "\n";   // last-add-wins; catches a camera-ORDER divergence the sorted cameras: section cannot
		{ char rrb[64]; std::snprintf( rrb, sizeof(rrb), "  light_rr_threshold=%.9g\n", job.GetLightSampleRRThreshold() ); o << rrb; }   // render-affecting light-sample RR (v7 light_rr_threshold chunk)
	}
	// --- lights + media (Phase B / 0b: close the F1 verification gap for the CHEAPLY-READABLE blind values).
	// Lights have no manager names, so dump a value-tuple per light and SORT for a stable canonical order; a
	// CST divergence in any light value (power/colour/position/cone) reorders or changes a row. Media dump the
	// global medium identity + per-medium state via DumpMedium (coefficients at the bbox centre, phase-g, placement).
	o << "lights:\n";
	{
		std::vector<std::string> rows;
		ILightManager* lm = job.GetLights();
		if( lm ) {
			const ILightManager::LightsList& ls = lm->getLights();
			for( const ILightPriv* lp : ls ) {
				const ILight* l = lp; if( !l ) continue;
				const RISEPel col = l->emissionColor(); const RISEPel rx = l->radiantExitance();
				const Point3 p = l->position(), tg = l->emissionTarget(); const Vector3 d = l->emissionDirection();
				char b[768];
				std::snprintf( b, sizeof(b),
					"  type=%d energy=%.9g col=[%.9g %.9g %.9g] exitance=[%.9g %.9g %.9g] pos=[%.9g %.9g %.9g] dir=[%.9g %.9g %.9g] cone=%.9g inner=%.9g outer=%.9g target=[%.9g %.9g %.9g] photons=%d",
					(int)l->lightType(), (double)l->emissionEnergy(),
					(double)col.r,(double)col.g,(double)col.b, (double)rx.r,(double)rx.g,(double)rx.b,
					(double)p.x,(double)p.y,(double)p.z, (double)d.x,(double)d.y,(double)d.z,
					(double)l->emissionConeHalfAngle(), (double)l->emissionInnerAngle(), (double)l->emissionOuterAngle(),
					(double)tg.x,(double)tg.y,(double)tg.z, (int)l->CanGeneratePhotons() );
				rows.push_back( b );
			}
		}
		std::sort( rows.begin(), rows.end() );
		for( const auto& r : rows ) o << r << "\n";
	}
	o << "media:\n";
	{
		IScenePriv* sc = job.GetScene(); const IMedium* gm = sc ? sc->GetGlobalMedium() : 0;
		o << "  global=" << ReverseMediumName( job, gm ); if( gm ) DumpMedium( o, gm ); o << "\n";
		NameCollector mns; job.EnumerateMediumNames( mns ); std::sort( mns.names.begin(), mns.names.end() );
		for( const auto& n : mns.names ) {
			o << "  " << n; const IMedium* m = job.GetMedium( n.c_str() ); if( m ) DumpMedium( o, m ); o << "\n";
		}
	}
	return o.str();
}

} // namespace risequiv

#endif // RISE_TESTS_CST_RENDER_EQUIVALENCE_H
