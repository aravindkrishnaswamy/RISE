// DL-03: exercise the production PT guiding substitution with real OpenPGL
// training and real TranslucentSPF. Only the next intersection is controlled:
// it presents the same object again so its input stack and actual entry/exit
// classification can be observed without requiring a rare scene trajectory.
#ifndef TRANSLUCENT_GUIDED_STACK_PROBE_H
#define TRANSLUCENT_GUIDED_STACK_PROBE_H

#include "../src/Library/Materials/TranslucentMaterial.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
#include "../src/Library/Shaders/StandardShader.h"
#include "../src/Library/Rendering/RayCaster.h"
#include "../src/Library/Managers/ObjectManager.h"
#include "../src/Library/Scene.h"
#include "../src/Library/Shaders/BDPTIntegrator.h"
#include "../src/Library/Managers/LightManager.h"
#include "../src/Library/Lights/PointLight.h"
#include "../src/Library/Lights/LightSampler.h"
#include "../src/Library/Utilities/RuntimeContext.h"
#include "../src/Library/Utilities/PathGuidingField.h"

#ifdef RISE_ENABLE_OPENPGL
namespace GuidedStackProbe {
struct Observation {
	unsigned int scatters = 0, pdfQueries = 0;
	bool initialExit = false, poppedSPFStack = false, arrived = false;
	bool containsOnArrival = false, entryLobeOnArrival = false;
	Scalar mediumOnArrival = 0;
	bool hasEntryPrefix = false, forceEntryChoice = false;
	Vector3 spfDirection, tracedDirection, exitNormal;
};

class ObservedSPF : public virtual ISPF, public virtual Reference {
	const ISPF& real;
	Observation& observed;
	void Record( const RayIntersectionGeometric& ri, const IORStack& stack,
		const ScatteredRayContainer& rays ) const
	{
		const unsigned int index = observed.scatters++;
		if( observed.hasEntryPrefix && index == 0 ) {
			observed.forceEntryChoice = true;
			return;
		}
		if( index == (observed.hasEntryPrefix ? 1u : 0u) ) {
			observed.exitNormal = ri.vGeomNormal;
			observed.initialExit = stack.containsCurrent();
			for( unsigned int i = 0; i < rays.Count(); ++i ) {
				if( rays[i].type == ScatteredRay::eRayDiffuse && rays[i].ior_stack ) {
					observed.poppedSPFStack = !rays[i].ior_stack->containsCurrent();
					observed.spfDirection = rays[i].ray.Dir();
				}
			}
		} else {
			observed.arrived = true;
			observed.tracedDirection = ri.ray.Dir();
			observed.containsOnArrival = stack.containsCurrent();
			observed.mediumOnArrival = stack.top();
			for( unsigned int i = 0; i < rays.Count(); ++i ) {
				if( rays[i].type == ScatteredRay::eRayTranslucent && rays[i].ior_stack )
					observed.entryLobeOnArrival = true;
			}
		}
	}
public:
	ObservedSPF( const ISPF& delegate, Observation& state ) : real(delegate), observed(state) {}
	void Scatter( const RayIntersectionGeometric& ri, ISampler& sampler,
		ScatteredRayContainer& rays, const IORStack& stack ) const override {
		real.Scatter(ri, sampler, rays, stack); Record(ri, stack, rays);
	}
	void ScatterNM( const RayIntersectionGeometric& ri, ISampler& sampler, Scalar nm,
		ScatteredRayContainer& rays, const IORStack& stack ) const override {
		real.ScatterNM(ri, sampler, nm, rays, stack); Record(ri, stack, rays);
	}
	Scalar Pdf( const RayIntersectionGeometric& ri, const Vector3& wo,
		const IORStack& stack ) const override {
		++observed.pdfQueries; return real.Pdf(ri, wo, stack);
	}
	Scalar PdfNM( const RayIntersectionGeometric& ri, const Vector3& wo, Scalar nm,
		const IORStack& stack ) const override {
		++observed.pdfQueries; return real.PdfNM(ri, wo, nm, stack);
	}
};

class ObservedMaterial : public TranslucentMaterial {
	ObservedSPF* observer;
public:
	ObservedMaterial( const IPainter& front, const IPainter& trans,
		const IScalarPainter& extinction, const IScalarPainter& exponent,
		const IScalarPainter& scattering, Observation& observation ) :
		TranslucentMaterial(front, trans, extinction, exponent, scattering),
		observer(new ObservedSPF(*pSPF, observation)) {}
	~ObservedMaterial() override { observer->release(); }
	ISPF* GetSPF() const override { return observer; }
};

class NextHitManager : public ObjectManager {
	const IObject& object;
	const IMaterial& material;
public:
	NextHitManager( const IObject& obj, const IMaterial& mat ) :
		ObjectManager(false, false, 4, 8), object(obj), material(mat) {}
	void IntersectRay( RayIntersection& ri, bool, bool, bool ) const override {
		ri.geometric.bHit = true;
		ri.geometric.range = 1;
		ri.geometric.ptIntersection = ri.geometric.ray.PointAtLength(1);
		ri.geometric.vNormal = -ri.geometric.ray.Dir();
		ri.geometric.vGeomNormal = ri.geometric.vNormal;
		ri.geometric.onb.CreateFromW(ri.geometric.vNormal);
		ri.pObject = &object;
		ri.pMaterial = &material;
	}
};

// The BDPT generators accept camera/light roots rather than a pre-seeded
// surface. Start outside, select the real translucent entry, then inspect
// the exit and subsequent hit. This sampler fixes only that entry-lobe
// choice; all guide sampling uses the seeded independent stream.
class EntrySampler : public IndependentSampler {
	Observation& observed;
public:
	EntrySampler(const RandomNumberGenerator& rng, Observation& state) : IndependentSampler(rng), observed(state) {}
	Scalar Get1D() override {
		if(observed.forceEntryChoice) { observed.forceEntryChoice = false; return .999; }
		return IndependentSampler::Get1D();
	}
};
class ThreeHitManager : public ObjectManager {
	const IObject& object;
	const IMaterial& material;
	const Observation& observed;
public:
	ThreeHitManager(const IObject& obj, const IMaterial& mat, const Observation& state) :
		ObjectManager(false,false,4,8),object(obj),material(mat),observed(state) {}
	void IntersectRay(RayIntersection& ri, bool, bool, bool) const override {
		if(observed.scatters >= 3) return;
		ri.geometric.bHit = true;
		ri.geometric.range = 1;
		ri.geometric.ptIntersection = observed.scatters == 1 ? Point3(0,0,0) : ri.geometric.ray.PointAtLength(1);
		ri.geometric.vNormal = observed.scatters == 1 ? ri.geometric.ray.Dir() : -ri.geometric.ray.Dir();
		ri.geometric.vGeomNormal = ri.geometric.vNormal;
		ri.geometric.onb.CreateFromW(ri.geometric.vNormal);
		ri.pObject = &object;
		ri.pMaterial = &material;
	}
};
static void RunBDPT(PathGuidingField& guide, const IPainter& front, const IPainter& trans,
	const IScalarPainter& extinction, const IScalarPainter& exponent,
	const IScalarPainter& scattering, const IObject& object, const IRayCaster& caster)
{
	std::cout << "Sub-test 6: DL-03 trained BDPT real entry/exit/next-hit stack" << std::endl;
	StabilityConfig stability;
	stability.rrMinDepth = 10;
	stability.maxVolumeBounce = 0;
	stability.branchingThreshold = 1;
	BDPTIntegrator* integrator = new BDPTIntegrator(3,3,stability);
	LightManager* lights = new LightManager();
	PointLight* point = new PointLight(1,RISEPel(1,1,1),true);
	lights->AddItem(point,"probe_light");
	point->release();
	for(unsigned int side=0; side<2; ++side) {
		for(unsigned int spectral=0; spectral<2; ++spectral) {
			for(unsigned int mode=0; mode<3; ++mode) {
				unsigned int reached=0,outwardSub=0,inwardSub=0,retained=0,badOut=0,badIn=0,badInitial=0,badMedium=0;
				for(unsigned int trial=0; trial<512; ++trial) {
					Observation observation;
					observation.hasEntryPrefix = true;
					ObservedMaterial* material = new ObservedMaterial(front,trans,extinction,exponent,scattering,observation);
					ThreeHitManager* manager = new ThreeHitManager(object,*material,observation);
					Scene* scene = new Scene();
					scene->SetObjectManager(manager);
					scene->SetLightManager(lights);
					LightSampler* lightSampler = new LightSampler();
					lightSampler->Prepare(*scene,LuminaryManager::LuminariesList());
					integrator->SetLightSampler(lightSampler);
					integrator->SetGuidingField(mode ? &guide : 0,mode ? &guide : 0,.8,1,1,
						mode==2 ? eGuidingRIS : eGuidingOneSampleMIS,2);
					RandomNumberGenerator rng(12601+trial);
					EntrySampler sampler(rng,observation);
					RuntimeContext rc(rng,RuntimeContext::PASS_NORMAL,false);
					std::vector<BDPTVertex> vertices;
					std::vector<uint32_t> starts;
					if(side) {
						if(spectral) integrator->GenerateLightSubpathNM(*scene,caster,sampler,vertices,starts,550,rng,0);
						else integrator->GenerateLightSubpath(*scene,caster,sampler,vertices,starts,rng);
					} else {
						const Ray cameraRay(Point3(0,0,-2),Vector3(0,0,1));
						if(spectral) integrator->GenerateEyeSubpathNM(rc,cameraRay,Point2(.5,.5),*scene,caster,sampler,vertices,starts,550,0);
						else integrator->GenerateEyeSubpath(rc,cameraRay,Point2(.5,.5),*scene,caster,sampler,vertices,starts);
					}
					if(!observation.initialExit || !observation.poppedSPFStack) ++badInitial;
					if(observation.arrived) {
						++reached;
						const bool outward=Vector3Ops::Dot(observation.tracedDirection,observation.exitNormal)>0;
						const bool substituted=Vector3Ops::Magnitude(observation.tracedDirection-observation.spfDirection)>1e-8;
						if(observation.pdfQueries>0) {
							if(!substituted) ++retained;
							else if(outward) ++outwardSub;
							else ++inwardSub;
						}
						if(outward && (observation.containsOnArrival || !observation.entryLobeOnArrival)) ++badOut;
						if(!outward && (!observation.containsOnArrival || observation.entryLobeOnArrival)) ++badIn;
						if(std::fabs(observation.mediumOnArrival-1)>1e-12) ++badMedium;
					}
					integrator->SetLightSampler(0);
					lightSampler->release(); scene->release(); manager->release(); material->release();
				}
				std::cout << "  BDPT " << (side ? "light" : "eye") << " " << (spectral ? "NM" : "RGB")
					<< " mode=" << mode << " reached=" << reached << " substituted_out=" << outwardSub
					<< " substituted_in=" << inwardSub << " retained_spf=" << retained
					<< " bad_initial=" << badInitial << " bad_out=" << badOut << " bad_in=" << badIn
					<< " bad_medium=" << badMedium << std::endl;
				EXPECT(badInitial==0,"DL-03 BDPT real entry leads to real exit with a popped SPF stack");
				EXPECT(reached>0,"DL-03 BDPT continuation reached same-object observer");
				EXPECT(badOut==0,"DL-03 BDPT outward exit carries popped stack and next same-object Scatter enters");
				EXPECT(badIn==0,"DL-03 BDPT inward substitution preserves inside stack");
				EXPECT(badMedium==0,"DL-03 BDPT surrounding air IOR remains unchanged");
				if(mode) EXPECT(outwardSub>0,"DL-03 BDPT actual outward guided exit substitution count is positive");
				if(mode==1) EXPECT(inwardSub>0,"DL-03 BDPT inward guided substitution control is positive");
				if(mode==2) EXPECT(retained>0,"DL-03 BDPT RIS retained SPF candidate control is positive");
			}
		}
	}
	integrator->release(); lights->release();
}

static void Run()
{
	std::cout << "Sub-test 5: DL-03 trained PT guided exit stack / later same-object hit" << std::endl;
	PathGuidingConfig config;
	config.enabled = true;
	PathGuidingField* guide = new PathGuidingField(config, Point3(-2,-2,-2), Point3(2,2,2));
	guide->BeginTrainingIteration();
	// Broad angular support straddling the geometric horizon is deliberate:
	// outward substitutions must pop; inward substitutions must stay inside.
	for( unsigned int i = 0; i < 4096; ++i ) {
		const Scalar z = -1 + 2 * (i + 0.5) / 4096;
		const Scalar phi = i * 2.399963229728653;
		const Scalar r = std::sqrt(1-z*z);
		guide->AddSample(Point3(0,0,0), Vector3(r*std::cos(phi),r*std::sin(phi),z),
			1, 1/(4*PI), 1, false);
	}
	guide->EndTrainingIteration();
	EXPECT(guide->IsTrained(), "DL-03 OpenPGL field is actually trained");

	UniformColorPainter* front = new UniformColorPainter(RISEPel(.5,.5,.5));
	UniformColorPainter* trans = new UniformColorPainter(RISEPel(.5,.5,.5));
	UniformScalarPainter* extinction = new UniformScalarPainter(0);
	UniformScalarPainter* exponent = new UniformScalarPainter(1);
	UniformScalarPainter* scattering = new UniformScalarPainter(0);
	StubObject* water = new StubObject();
	StubObject* object = new StubObject();
	StandardShader* shader = new StandardShader(std::vector<IShaderOp*>());
	RayCaster* caster = new RayCaster(false, 8, *shader, false);
	StabilityConfig stability;
	stability.rrMinDepth = 10;
	PathTracingIntegrator* integrator = new PathTracingIntegrator(ManifoldSolverConfig(), stability);
	integrator->SetMaxPathDepth(2);
	const RasterizerState rast{};

	for( unsigned int spectral = 0; spectral < 2; ++spectral ) {
		for( unsigned int mode = 0; mode < 3; ++mode ) {
			unsigned int intercepted = 0, substitutedOut = 0, substitutedIn = 0, retainedSPF = 0;
			unsigned int badOut = 0, badIn = 0, badMedium = 0, badInitial = 0;
			for( unsigned int trial = 0; trial < 512; ++trial ) {
				Observation observation;
				ObservedMaterial* material = new ObservedMaterial(*front,*trans,*extinction,*exponent,*scattering,observation);
				NextHitManager* manager = new NextHitManager(*object,*material);
				Scene* scene = new Scene();
				scene->SetObjectManager(manager);
				RandomNumberGenerator rng(8191 + trial);
				IndependentSampler sampler(rng);
				RuntimeContext rc(rng, RuntimeContext::PASS_NORMAL, false);
				rc.pGuidingField = mode == 0 ? 0 : guide;
				rc.guidingAlpha = .8;
				rc.guidingLearnedAlpha = false;
				rc.maxGuidingDepth = 0;
				rc.guidingSamplingType = mode == 2 ? eGuidingRIS : eGuidingOneSampleMIS;
				RayIntersection hit(Ray(Point3(0,0,-1),Vector3(0,0,1)),rast);
				hit.geometric.bHit = true;
				hit.geometric.range = 1;
				hit.geometric.ptIntersection = Point3(0,0,0);
				hit.geometric.vNormal = Vector3(0,0,1);
				hit.geometric.vGeomNormal = Vector3(0,0,1);
				hit.geometric.onb.CreateFromW(Vector3(0,0,1));
				hit.pObject = object;
				hit.pMaterial = material;
				IORStack stack = MakeEnteringStack(water,object,kWaterIOR);
				stack.push(stack.top());
				// Seed a diffuse arrival explicitly. Ordinary translucent PT entry
				// arrives specular and therefore cannot prove this guiding branch.
				if( spectral ) {
					integrator->IntegrateFromHitNM(rc,rast,hit,550,*scene,*caster,sampler,0,
						0,stack,0,1,true,1,IRayCaster::RAY_STATE::eRayDiffuse,
						0,0,0,0,0,0,false,false);
				} else {
					integrator->IntegrateFromHit(rc,rast,hit,*scene,*caster,sampler,0,
						0,stack,0,RISEPel(1,1,1),true,1,IRayCaster::RAY_STATE::eRayDiffuse,
						0,0,0,0,0,0,false,false);
				}
				if( !observation.initialExit || !observation.poppedSPFStack ) ++badInitial;
				if( observation.arrived ) {
					++intercepted;
					const bool outward = Vector3Ops::Dot(observation.tracedDirection,observation.exitNormal) > 0;
					const bool substituted = Vector3Ops::Magnitude(observation.tracedDirection-observation.spfDirection) > 1e-8;
					if( !substituted && observation.pdfQueries > 0 ) ++retainedSPF;
					if( substituted && observation.pdfQueries > 0 ) {
						if( outward ) ++substitutedOut; else ++substitutedIn;
					}
					if( outward && (observation.containsOnArrival || !observation.entryLobeOnArrival) ) ++badOut;
					if( !outward && (!observation.containsOnArrival || observation.entryLobeOnArrival) ) ++badIn;
					if( std::fabs(observation.mediumOnArrival-kWaterIOR) > 1e-12 ) ++badMedium;
				}
				scene->release(); manager->release(); material->release();
			}
			std::cout << "  " << (spectral ? "NM" : "RGB") << " mode=" << mode
				<< " intercepted=" << intercepted << " substituted_out=" << substitutedOut
				<< " substituted_in=" << substitutedIn << " retained_spf=" << retainedSPF << " bad_out=" << badOut
				<< " bad_in=" << badIn << " bad_medium=" << badMedium << std::endl;
			EXPECT(badInitial == 0, "DL-03 real SPF produced an exit with a popped stack on every trial");
			EXPECT(intercepted > 0, "DL-03 continuation reached the same-object stack observer");
			EXPECT(badOut == 0, "DL-03 outward exit carries popped stack and next same-object Scatter enters");
			EXPECT(badIn == 0, "DL-03 inward substituted direction preserves inside stack and does not re-enter");
			EXPECT(badMedium == 0, "DL-03 surrounding water IOR remains unchanged");
			if( mode ) EXPECT(substitutedOut > 0, "DL-03 non-vacuous outward guided exit substitution count is positive");
			if( mode == 2 ) EXPECT(retainedSPF > 0, "DL-03 RIS retained SPF candidate control is positive");
			if( mode == 1 ) EXPECT(substitutedIn > 0, "DL-03 one-sample inward substitution control is positive");
		}
	}
	RunBDPT(*guide,*front,*trans,*extinction,*exponent,*scattering,*object,*caster);
	integrator->release(); caster->release(); shader->release();
	object->release(); water->release(); scattering->release(); exponent->release();
	extinction->release(); trans->release(); front->release(); guide->release();
}
}
#else
namespace GuidedStackProbe {
static void Run() { std::cout << "DL-03 guiding coverage unavailable: build without OpenPGL" << std::endl; }
}
#endif
#endif
