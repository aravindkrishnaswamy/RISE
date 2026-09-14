//////////////////////////////////////////////////////////////////////
//
//  PTGuidedSelectProbTest.cpp - Red-proof for DL-42 (docs/DEBT_LEDGER.md):
//    PathTracingIntegrator's trained-guiding "kept BSDF direction"
//    one-sample branch drops the multi-lobe selectProb compensation.
//
//  THE BUG
//
//    PathTracingIntegrator.cpp's main scatter loop stochastically
//    selects ONE lobe from a multi-lobe material's ScatteredRayContainer
//    (PTRandomlySelect, weighted by max(kray) per lobe -- PTScatterSelectWeight)
//    and initializes:
//
//        selectProb = PTScatterSelectWeight(*pS) / sum_i PTScatterSelectWeight(scattered[i])
//        scatterThroughput = PTScatterKray(*pS) * (1 / selectProb)
//
//    kray is already "BSDF*cos/pdf" FOR THAT LOBE (ISPF.h's documented
//    contract), so this is the standard unbiased multi-lobe estimator:
//    the lobe-selection step is compensated by dividing by its own
//    selection probability.
//
//    When trained one-sample guiding is active but the per-vertex coin
//    flip (ShouldUseGuidedSample) decides to KEEP the BSDF's own sampled
//    direction (guiding only re-weights its pdf via one-sample MIS here,
//    it doesn't replace the direction), the code instead does:
//
//        combinedPdf = GuidingCombinedPdf(alpha, guidePdfForBsdf, pS->pdf)
//        scatterThroughput = PTScatterKray(*pS) * (pS->pdf / combinedPdf)
//
//    Since kray == f_lobe*cos/pS->pdf already, this simplifies to
//    f_lobe*cos/combinedPdf -- a valid MIS reweight of THAT LOBE's own
//    contribution from its native pdf to the guide-blended combinedPdf,
//    but the selectProb factor from the original initialization has been
//    silently DISCARDED. The correct expression is:
//
//        scatterThroughput = kray * pS->pdf / (selectProb * combinedPdf)
//
//  WHAT THIS FIX DOES **NOT** CLAIM ABOUT THE OTHER TWO PTMulDiv() SITES
//  (documented here because a plain-English summary of this row can read
//  as implicating all three trained-guiding overwrite sites; corrected
//  after a later derivation (DL-67) showed the original claim below was
//  itself wrong -- see the debt-cleanup rules' "re-verify the row first"
//  step, applied a second time to this comment):
//
//    The RIS-accepted-candidate branch (scatterThroughput =
//    PTMulDiv(candidates[sel].bsdfEval, cosTheta, risEffectivePdf)) and
//    the one-sample GUIDED-direction-accepted branch (scatterThroughput =
//    PTMulDiv(fGuided, cosTheta, combinedPdf)) both evaluate the BSDF and
//    PDF through PTEvalBSDFAtSurface / PTEvalPdfAtSurface, which forward
//    to IBSDF::value() and ISPF::Pdf() -- the material's AGGREGATE,
//    all-lobes-summed response (confirmed against GGXBRDF::value(),
//    "return diffuse + specular;", and GGXSPF::Pdf(), "3-lobe mixture
//    PDF weighted by painter albedos"), evaluated FRESH at whatever
//    direction is being priced. THIS DOES NOT MAKE THEM COMPLETE,
//    SELF-CONTAINED ESTIMATORS IMMUNE TO selectProb, as an earlier
//    version of this comment (and the DL-42 ledger row / DL02 doc) wrongly
//    claimed. GuidingSupportsSurfaceSampling (~line 552) admits only
//    non-delta eRayDiffuse/eRayReflection lobes: at a multi-lobe surface
//    where some lobes are guiding-INeligible (e.g. TranslucentSPF's
//    eRayTranslucent backscatter), these two branches price the
//    material's FULL aggregate value() -- which covers every lobe,
//    eligible or not -- while the guiding machinery only ever
//    proposed/weighted the eligible subset, and the ineligible lobes are
//    then priced AGAIN whenever PTRandomlySelect happens to pick them
//    (via the ordinary kray/selectProb path, unaffected by any of this).
//    Separately, even restricted to an all-lobes-eligible material, this
//    branch's own single-lobe proposal density (this file's fixed branch:
//    pS->pdf) and the RIS-accepted/guided-accepted branches' aggregate
//    proposal density do not partition to a consistent total probability
//    across the three branches unless every lobe shares one pdf (i.e. a
//    single-lobe SPF) -- see DL-67 for the numeric counterexample and the
//    correct, consistent construction (one f and one denominator: the
//    true mixture density actually being sampled from, in all three
//    branches). RIS's candidate-0 slot additionally sets c.bsdfPdf =
//    pS->pdf (the single selected lobe's own density) rather than the
//    material's aggregate PTEvalPdfAtSurface density like candidate 1
//    uses -- also part of DL-67, not a separate bug.
//
//  WHY TranslucentSPF, NOT ggx_material/coated_material
//
//    A genuinely multi-CANDIDATE ScatteredRayContainer (needed for
//    selectProb < 1) requires a material whose Scatter() call emits more
//    than one ScatteredRay non-exclusively.  GGXSPF::Scatter and
//    CoatedSPF::ScatterImpl each do their OWN internal stochastic lobe
//    selection and emit exactly ONE ScatteredRay per call (confirmed by
//    reading both), so PTRandomlySelect's outer selectProb is trivially 1
//    for either -- not a useful red-proof vertex.  TranslucentSPF's EXIT
//    branch (ior_stack.containsCurrent()==true, "coming out the other
//    side") DOES add two non-exclusive, non-delta rays to the SAME
//    container when `scattering` > 0: a translucent backscatter lobe
//    (eRayTranslucent, stays inside) and, unconditionally afterward, the
//    diffuse exit lobe (eRayDiffuse, pops the IOR stack) -- see
//    TranslucentSPF.cpp's "Coming out the other side" branch. Only the
//    diffuse-exit lobe is guiding-eligible (GuidingSupportsSurfaceSampling
//    admits eRayDiffuse/eRayReflection, not eRayTranslucent), so this test
//    only asserts on trials where PTRandomlySelect happens to pick that
//    lobe -- exactly the scenario the recipe describes as "an ordinary
//    entry reflection on a material with several nonzero lobes," here
//    realized as an ordinary EXIT diffuse lobe alongside a backscatter
//    lobe with unequal weight.
//
//  RED-PROOF STRATEGY
//
//    extinction=0 and scattering=s (isotropic Phong N so all three RGB
//    channels use the single-ray, not per-channel, code path) gives
//    front.kray == (1-s) and trans.kray == s, both isotropic -- so
//    selectProb, when the diffuse-exit lobe is chosen, is EXACTLY (1-s)
//    (PTScatterSelectWeight is MaxValue(kray) and the two lobes' weights
//    sum to exactly 1). The unguided baseline throughput
//    kray/selectProb == (1-s)/(1-s) == 1 exactly -- a clean, independently
//    verifiable energy-conservation sanity check for this degenerate
//    (zero-extinction) configuration.
//
//    A real, trained PathGuidingField is wired into a real
//    PathTracingIntegrator::IntegrateFromHit call with guidingAlpha set to
//    a vanishingly small but nonzero value (1e-6): small enough that
//    ShouldUseGuidedSample's coin flip (xi < alpha) takes the "keep the
//    BSDF direction" branch on essentially every trial (P(guided) ~ 1e-6)
//    while remaining > NEARZERO so the trained-guiding code path (and its
//    bug) actually engages -- alpha == 0 would skip the whole block. It
//    also makes combinedPdf == alpha*guidePdf + (1-alpha)*pS->pdf
//    numerically indistinguishable from pS->pdf alone (the guide term
//    contributes at most ~1e-6 relative), so the correct/buggy predicted
//    throughputs below need no direct measurement of the live OpenPGL
//    density.
//
//    The scattered vertex is wired to a constant-radiance "sky" object
//    (LambertianLuminaireMaterial over a material with no BSDF/SPF, so
//    the path necessarily terminates there) via a real ObjectManager, and
//    NO LightManager/LightSampler is attached to either the Scene or the
//    RayCaster -- IntegrateFromHit's only radiance source is therefore
//    the BSDF-sampled continuation hitting the sky, with no NEE
//    double-count possible, and (since the sky object carries no
//    IGeometry, IObject::GetGeometry() defaults to null so
//    CanBeAreaLight() reads false) no area-light MIS re-weighting either
//    -- the sky's emission is added at full, unweighted value. The
//    returned RISEPel/Scalar is therefore exactly:
//
//        result = initialImportance(1) * scatterThroughput * skyRadiance
//
//    A thin ISPF decorator around the real TranslucentSPF captures every
//    emitted ScatteredRay's (direction, kray, krayNM, pdf, type) from the
//    ONE production Scatter()/ScatterNM() call per trial, and a thin
//    ObjectManager decorator on the sky records the actual traced
//    continuation direction -- together these identify, from OUTSIDE
//    production code, which lobe PTRandomlySelect picked (by matching the
//    traced direction against each captured lobe's own direction) without
//    reimplementing any selection logic. Comparing the measured
//    throughput (result / skyRadiance) against kray*pdf/(selectProb*combinedPdf)
//    (correct) and kray*pdf/combinedPdf (buggy, pre-fix) over many
//    independently-seeded trials red-proves the drop and green-proves the
//    fix, using only the real, unmodified production Scatter() output and
//    the real returned integrator radiance.
//
//////////////////////////////////////////////////////////////////////

#include <iostream>
#include <cmath>
#include <vector>

#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/PathGuidingField.h"
#include "../src/Library/Utilities/IndependentSampler.h"
#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Materials/LambertianLuminaireMaterial.h"
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Painters/UniformScalarPainter.h"
#include "TestStubObject.h"

using namespace RISE;
using namespace RISE::Implementation;

static int failed = 0;

#define EXPECT( cond, msg ) do { \
	if( !(cond) ) { \
		std::cout << "FAIL: " << __FILE__ << ":" << __LINE__ << " " << msg << std::endl; \
		failed++; \
	} \
} while(0)

#ifdef RISE_ENABLE_OPENPGL

//////////////////////////////////////////////////////////////////////
// NullMaterial - the sky's base material.  IMaterial has exactly three
// pure virtuals (GetBSDF/GetSPF/GetEmitter); a material with no BSDF/SPF
// cannot scatter further, which is what stops the walk at the sky hit
// without needing to fight over exact maxPathDepth semantics.
//////////////////////////////////////////////////////////////////////
class NullMaterial : public virtual IMaterial, public virtual Reference
{
protected:
	~NullMaterial() override {}
public:
	IBSDF* GetBSDF() const override { return 0; }
	ISPF* GetSPF() const override { return 0; }
	IEmitter* GetEmitter() const override { return 0; }
};

//////////////////////////////////////////////////////////////////////
// Captured per-lobe state from the ONE real Scatter()/ScatterNM() call
// each trial makes.  Deliberately NOT copying ScatteredRay itself --
// its destructor conditionally deletes ior_stack (delete_stack), and
// copying that ownership into a second container risks a double-free
// once both the original ScatteredRayContainer and this vector are
// destroyed independently.
//////////////////////////////////////////////////////////////////////
struct CapturedLobe
{
	Vector3 direction;
	RISEPel kray;
	Scalar krayNM;
	Scalar pdf;
	ScatteredRay::ScatRayType type;
};

struct Observation
{
	unsigned int scatterCalls = 0;
	std::vector<CapturedLobe> lobes;
	bool reachedSky = false;
	Vector3 tracedDirection;
};

class ObservingSPF : public virtual ISPF, public virtual Reference
{
	const ISPF& real;
	Observation& obs;
	void Capture( const ScatteredRayContainer& rays ) const
	{
		++obs.scatterCalls;
		obs.lobes.clear();
		for( unsigned int i = 0; i < rays.Count(); ++i ) {
			CapturedLobe c;
			c.direction = rays[i].ray.Dir();
			c.kray = rays[i].kray;
			c.krayNM = rays[i].krayNM;
			c.pdf = rays[i].pdf;
			c.type = rays[i].type;
			obs.lobes.push_back( c );
		}
	}
public:
	ObservingSPF( const ISPF& delegate, Observation& o ) : real( delegate ), obs( o ) {}
	void Scatter( const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& rays, const IORStack& stack ) const override
	{
		real.Scatter( ri, sampler, rays, stack );
		Capture( rays );
	}
	void ScatterNM( const RayIntersectionGeometric& ri, ISampler& sampler, Scalar nm,
		ScatteredRayContainer& rays, const IORStack& stack ) const override
	{
		real.ScatterNM( ri, sampler, nm, rays, stack );
		Capture( rays );
	}
	Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo,
		const IORStack& stack ) const override { return real.Pdf( ri, wo, stack ); }
	Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, Scalar nm,
		const IORStack& stack ) const override { return real.PdfNM( ri, wo, nm, stack ); }
};

class ObservingTranslucentMaterial : public TranslucentMaterial
{
	ObservingSPF* obsSPF;
public:
	ObservingTranslucentMaterial(
		const IPainter& front, const IPainter& trans,
		const IScalarPainter& extinction, const IScalarPainter& exponent,
		const IScalarPainter& scattering, Observation& obs ) :
		TranslucentMaterial( front, trans, extinction, exponent, scattering ),
		obsSPF( new ObservingSPF( *pSPF, obs ) ) {}
	~ObservingTranslucentMaterial() override { obsSPF->release(); }
	ISPF* GetSPF() const override { return obsSPF; }
};

//////////////////////////////////////////////////////////////////////
// SkyManager - every intersection (used only for the post-vertex
// continuation ray, since the material vertex itself is supplied
// directly as firstHit) hits a constant-radiance emissive object with
// no geometry (so IObject::GetGeometry() defaults to null ->
// CanBeAreaLight() reads false -> the emission is added at full,
// unweighted value: no NEE competes for it because no
// LightManager/LightSampler exists in this test at all).
//////////////////////////////////////////////////////////////////////
class SkyManager : public ObjectManager
{
	const IObject& skyObject;
	const IMaterial& skyMaterial;
	Observation& obs;
public:
	SkyManager( const IObject& obj, const IMaterial& mat, Observation& o ) :
		ObjectManager( false, false, 4, 8 ), skyObject( obj ), skyMaterial( mat ), obs( o ) {}
	void IntersectRay( RayIntersection& ri, bool, bool, bool ) const override
	{
		ri.geometric.bHit = true;
		ri.geometric.range = 1;
		ri.geometric.ptIntersection = ri.geometric.ray.PointAtLength( 1 );
		ri.geometric.vNormal = -ri.geometric.ray.Dir();
		ri.geometric.vGeomNormal = ri.geometric.vNormal;
		ri.geometric.onb.CreateFromW( ri.geometric.vNormal );
		ri.pObject = &skyObject;
		ri.pMaterial = &skyMaterial;
		// The actual continuation direction traced by the integrator --
		// whichever lobe PTRandomlySelect picked, whether or not guiding
		// substituted it.
		obs.reachedSky = true;
		obs.tracedDirection = ri.geometric.ray.Dir();
	}
};

static Scalar SelectWeightPel( const CapturedLobe& l ) { return ColorMath::MaxValue( l.kray ); }
static Scalar SelectWeightNM( const CapturedLobe& l ) { return l.krayNM; }

static void RunOneMode( bool spectral, Scalar skyRadianceValue, Scalar scatFactor )
{
	std::cout << "DL-42: PT trained-guiding kept-BSDF-direction throughput ("
		<< (spectral ? "NM" : "RGB") << ", scattering=" << scatFactor << ")" << std::endl;

	UniformColorPainter* frontP = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  // unused (entry-only)
	UniformColorPainter* transP = new UniformColorPainter( RISEPel( 0.5, 0.5, 0.5 ) );  // unused (entry-only)
	UniformScalarPainter* extinctionP = new UniformScalarPainter( 0.0 );
	UniformScalarPainter* exponentP = new UniformScalarPainter( 1.0 );	// isotropic N -> single-ray exit path
	UniformScalarPainter* scatteringP = new UniformScalarPainter( scatFactor );

	Observation obs;
	ObservingTranslucentMaterial* material = new ObservingTranslucentMaterial(
		*frontP, *transP, *extinctionP, *exponentP, *scatteringP, obs );

	UniformColorPainter* skyRadEx = new UniformColorPainter(
		RISEPel( skyRadianceValue, skyRadianceValue, skyRadianceValue ) );
	NullMaterial* nullBase = new NullMaterial();
	LambertianLuminaireMaterial* skyMaterial =
		new LambertianLuminaireMaterial( *skyRadEx, 1.0, *nullBase );
	StubObject* skyObject = new StubObject();
	StubObject* vertexObject = new StubObject();

	SkyManager* manager = new SkyManager( *skyObject, *skyMaterial, obs );
	Scene* scene = new Scene();
	scene->SetObjectManager( manager );
	// Deliberately NO SetLightManager: pLS == caster.GetLightSampler() == 0.

	StandardShader* shader = new StandardShader( std::vector<IShaderOp*>() );
	RayCaster* caster = new RayCaster( false, 8, *shader, false );

	PathGuidingConfig config;
	config.enabled = true;
	PathGuidingField* guide = new PathGuidingField( config, Point3( -2, -2, -2 ), Point3( 2, 2, 2 ) );
	guide->BeginTrainingIteration();
	for( unsigned int i = 0; i < 4096; ++i ) {
		const Scalar z = -1 + 2 * (i + 0.5) / 4096;
		const Scalar phi = i * 2.399963229728653;
		const Scalar r = std::sqrt( 1 - z * z );
		guide->AddSample( Point3( 0, 0, 0 ), Vector3( r * std::cos(phi), r * std::sin(phi), z ),
			1, 1 / (4 * PI), 1, false );
	}
	guide->EndTrainingIteration();
	EXPECT( guide->IsTrained(), "DL-42 OpenPGL field is actually trained" );

	PathTracingIntegrator* integrator = new PathTracingIntegrator( ManifoldSolverConfig(), StabilityConfig() );
	integrator->SetMaxPathDepth( 4 );

	const RasterizerState rast{};
	const Scalar nm = 550.0;

	// Query the sky's actual emitted radiance directly through the real
	// IEmitter interface rather than assuming a formula (RGB divides the
	// exitance painter by pi; NM additionally routes through GetRadianceNM's
	// spectral-illuminant machinery, which is not exactly radEx*INV_PI at
	// a single wavelength). LambertianEmitter's output is direction/position
	// independent (a UniformColorPainter, gated only by Dot(out,N)>0), so
	// any valid ri/out/N combination gives the exact constant used in
	// every trial below.
	IEmitter* skyEmitter = skyMaterial->GetEmitter();
	RayIntersectionGeometric skyRI( Ray( Point3(0,0,0), Vector3(0,0,1) ), rast );
	skyRI.vNormal = Vector3( 0, 0, 1 );
	const Vector3 skyOut( 0, 0, 1 );
	const RISEPel skyRadiancePel = skyEmitter->emittedRadiance( skyRI, skyOut, skyRI.vNormal );
	const Scalar skyRadianceNM = skyEmitter->emittedRadianceNM( skyRI, skyOut, skyRI.vNormal, nm );
	EXPECT( ColorMath::MaxValue( skyRadiancePel ) > 0, "DL-42 sky RGB radiance probe is positive" );
	EXPECT( skyRadianceNM > 0, "DL-42 sky NM radiance probe is positive" );

	const unsigned int kTrials = 4000;
	unsigned int diffuseSelected = 0, translucentSelected = 0, guidedSubstituted = 0, unusable = 0;
	unsigned int matchesCorrect = 0, matchesBuggy = 0, matchesNeither = 0;

	for( unsigned int trial = 0; trial < kTrials; ++trial ) {
		RandomNumberGenerator rng( 71000 + trial );
		IndependentSampler sampler( rng );
		RuntimeContext rc( rng, RuntimeContext::PASS_NORMAL, false );
		rc.pGuidingField = guide;
		// Vanishingly small but nonzero: engages the trained-guiding code
		// path (and therefore the bug) while making ShouldUseGuidedSample
		// pick the guided direction with probability ~1e-6 per trial --
		// negligible over kTrials=4000 samples.
		rc.guidingAlpha = 1e-6;
		rc.guidingLearnedAlpha = false;
		rc.maxGuidingDepth = 4;
		rc.guidingSamplingType = eGuidingOneSampleMIS;

		// "Coming out the other side": containsCurrent()==true so
		// TranslucentSPF::Scatter takes the exit branch directly,
		// matching TranslucentIORStackTest.cpp's MakeEnteringStack +
		// extra push idiom (both media are air here, so no eta^2
		// scaling confounds the throughput measurement regardless of
		// which lobe -- front or trans -- ends up selected).
		IORStack stack( 1.0 );
		stack.SetCurrentObject( vertexObject );
		stack.push( 1.0 );

		RayIntersection hit( Ray( Point3( 0, 0, -1 ), Vector3( 0, 0, 1 ) ), rast );
		hit.geometric.bHit = true;
		hit.geometric.range = 1;
		hit.geometric.ptIntersection = Point3( 0, 0, 0 );
		hit.geometric.vNormal = Vector3( 0, 0, 1 );
		hit.geometric.vGeomNormal = Vector3( 0, 0, 1 );
		hit.geometric.onb.CreateFromW( hit.geometric.vNormal );
		hit.pObject = vertexObject;
		hit.pMaterial = material;

		obs.scatterCalls = 0;
		obs.lobes.clear();
		obs.reachedSky = false;

		RISEPel resultPel( 0, 0, 0 );
		Scalar resultNM = 0;
		// Seed a diffuse arrival explicitly, as the DL-03 fixture does:
		// ordinary PT translucent entry/backscatter arrives specular and
		// cannot prove a guiding-eligible branch; the exit under audit
		// here is reached by construction (stack pre-set to "inside").
		if( spectral ) {
			resultNM = integrator->IntegrateFromHitNM( rc, rast, hit, nm, *scene, *caster, sampler, 0,
				0, stack, 0, 1.0, true, 1, IRayCaster::RAY_STATE::eRayDiffuse,
				0, 0, 0, 0, 0, 0, false, false );
		} else {
			resultPel = integrator->IntegrateFromHit( rc, rast, hit, *scene, *caster, sampler, 0,
				0, stack, 0, RISEPel( 1, 1, 1 ), true, 1, IRayCaster::RAY_STATE::eRayDiffuse,
				0, 0, 0, 0, 0, 0, false, false );
		}

		if( obs.scatterCalls == 0 || obs.lobes.empty() || !obs.reachedSky ) { ++unusable; continue; }

		Scalar totalWeight = 0;
		for( const auto& l : obs.lobes ) {
			totalWeight += spectral ? SelectWeightNM( l ) : SelectWeightPel( l );
		}
		if( totalWeight <= 0 ) { ++unusable; continue; }

		// Identify the selected lobe by matching the ACTUAL traced
		// continuation direction (captured at the sky hit) against each
		// emitted lobe's own direction -- doubles as a guided-substitution
		// detector: on the ~1e-6-probability trial where guiding actually
		// replaces the direction, no captured lobe matches.
		int selIdx = -1;
		for( unsigned int i = 0; i < obs.lobes.size(); ++i ) {
			if( Vector3Ops::Magnitude( obs.lobes[i].direction - obs.tracedDirection ) < 1e-9 ) {
				selIdx = (int)i;
				break;
			}
		}
		if( selIdx < 0 ) { ++guidedSubstituted; continue; }
		const CapturedLobe& sel = obs.lobes[selIdx];

		if( sel.type == ScatteredRay::eRayTranslucent ) {
			// Not guiding-eligible (GuidingSupportsSurfaceSampling admits
			// only eRayDiffuse/eRayReflection) -- guiding never engaged
			// for this trial, so the bug cannot manifest here regardless
			// of selectProb.  Skip; the diffuse-exit trials below cover it.
			++translucentSelected;
			continue;
		}
		++diffuseSelected;

		const Scalar selWeight = spectral ? SelectWeightNM( sel ) : SelectWeightPel( sel );
		if( selWeight <= 0 ) { ++unusable; continue; }
		const Scalar selectProb = selWeight / totalWeight;
		if( selectProb <= 0 ) { ++unusable; continue; }

		const Scalar skyL = spectral ? skyRadianceNM : ColorMath::MaxValue( skyRadiancePel );
		const Scalar measured = spectral ? resultNM : ColorMath::MaxValue( resultPel );
		// measured == initialImportance(1) * scatterThroughput * skyL
		const Scalar measuredThroughput = measured / skyL;

		const Scalar krayMag = spectral ? sel.krayNM : ColorMath::MaxValue( sel.kray );
		// combinedPdf isn't independently observable (it depends on the
		// live OpenPGL guide density at the sampled direction), but with
		// guidingAlpha == 1e-6 the alpha*guidePdf term of
		// GuidingCombinedPdf is negligible relative to (1-alpha)*sel.pdf
		// -- see the file header derivation -- so combinedPdf == sel.pdf
		// to well under the tolerance used below.
		const Scalar combinedPdfApprox = sel.pdf;

		const Scalar correctThroughput = ( selectProb > 0 && combinedPdfApprox > 0 )
			? krayMag * sel.pdf / ( selectProb * combinedPdfApprox ) : 0;
		const Scalar buggyThroughput = ( combinedPdfApprox > 0 )
			? krayMag * sel.pdf / combinedPdfApprox : 0;

		// Tolerance covers the guidingAlpha=1e-6 approximation while
		// staying far tighter than the correct-vs-buggy gap (a full
		// 1/selectProb factor -- 1/(1-scatFactor) here, well over 10%).
		const bool matchCorrect = std::fabs( measuredThroughput - correctThroughput ) < 1e-4 * std::max( 1.0, (double)correctThroughput );
		const bool matchBuggy = std::fabs( measuredThroughput - buggyThroughput ) < 1e-4 * std::max( 1.0, (double)buggyThroughput );

		if( matchCorrect ) ++matchesCorrect;
		else if( matchBuggy ) ++matchesBuggy;
		else {
			++matchesNeither;
			if( matchesNeither <= 5 ) {
				std::cout << "    UNEXPECTED trial=" << trial << " measured=" << measured
					<< " measuredThroughput=" << measuredThroughput
					<< " correctThroughput=" << correctThroughput
					<< " buggyThroughput=" << buggyThroughput
					<< " krayMag=" << krayMag << " sel.pdf=" << sel.pdf
					<< " selectProb=" << selectProb << " totalWeight=" << totalWeight
					<< " lobes=" << obs.lobes.size() << std::endl;
			}
		}
	}

	std::cout << "  trials=" << kTrials << " diffuseSelected=" << diffuseSelected
		<< " translucentSelected=" << translucentSelected << " guidedSubstituted=" << guidedSubstituted
		<< " unusable=" << unusable << " matchesCorrect=" << matchesCorrect
		<< " matchesBuggy=" << matchesBuggy << " matchesNeither=" << matchesNeither << std::endl;

	EXPECT( diffuseSelected > kTrials / 4, "DL-42 a meaningful number of trials selected the guiding-eligible diffuse-exit lobe" );
	EXPECT( translucentSelected > kTrials / 20, "DL-42 sanity: the translucent backscatter lobe is also reachable (unequal lobe weights confirmed live)" );
	EXPECT( matchesNeither == 0, "DL-42 every diffuse-exit trial's measured throughput matches either the correct or the pre-fix-buggy closed form" );
	EXPECT( matchesCorrect == diffuseSelected,
		"DL-42 every diffuse-exit trial's throughput equals kray*pdf/(selectProb*combinedPdf) -- selectProb compensation retained under trained guiding" );

	integrator->release();
	guide->release();
	caster->release();
	shader->release();
	scene->release();
	manager->release();
	skyObject->release();
	vertexObject->release();
	skyMaterial->release();
	nullBase->release();
	skyRadEx->release();
	material->release();
	scatteringP->release();
	exponentP->release();
	extinctionP->release();
	transP->release();
	frontP->release();
}

static void Run()
{
	RunOneMode( false, 3.0, 0.35 );
	RunOneMode( true, 3.0, 0.35 );
	// A second, more lopsided split as an independent check that the fix
	// isn't tuned to one particular selectProb value.
	RunOneMode( false, 2.0, 0.1 );
	RunOneMode( true, 2.0, 0.1 );
}

#else

static void Run()
{
	std::cout << "DL-42 guiding coverage unavailable: build without OpenPGL" << std::endl;
}

#endif

int main()
{
	GlobalLog();
	Run();

	std::cout << std::endl;
	if( failed == 0 ) {
		std::cout << "ALL TESTS PASSED" << std::endl;
		return 0;
	} else {
		std::cout << failed << " CHECK(S) FAILED" << std::endl;
		return 1;
	}
}
