// DL-331: independent SMS-off PT energy reference and Snell coverage control.
// Args: replicas (>=4), optional "air", optional photon count (default 200000), optional PT spp (default 128), optional seed budget, optional SMS spp, optional solver threshold.
#define main GradedIndexFixtureMain
#include "GradedIndexInteriorFactorTest.cpp"
#undef main
#include "../src/Library/Rendering/PathTracingPelRasterizer.h"
#include "../src/Library/Shaders/PathTracingIntegrator.h"
// Legal protected-member access on the actual base object, as in DL-466.
struct PhotonDiagnosticsAccess : PathTracingPelRasterizer {
 static auto IntegratorMember() { return &PhotonDiagnosticsAccess::pIntegrator; }
};
static unsigned seedBudget=0;
static Scalar thresholdOverride=0;
static bool thresholdAttached=false;
static bool seedBudgetAttached=false;
static void AttachSeedBudget(IJobPriv* job) {
 if(auto* raster=dynamic_cast<PathTracingPelRasterizer*>(job->GetRasterizer())) {
  auto* integrator=raster->*PhotonDiagnosticsAccess::IntegratorMember();
  if(integrator && integrator->GetSolver()) {
   // The solver owns a nonconst config; mutate only before workers start.
   auto& config=const_cast<ManifoldSolverConfig&>(integrator->GetSolver()->GetConfig());
   if(config.photonCount>0 && seedBudget) {config.maxPhotonSeedsPerShadingPoint=seedBudget;seedBudgetAttached=true;}
   if(thresholdOverride>0) {config.solverThreshold=thresholdOverride;thresholdAttached=true;}
  }
 }
}
int main(int argc,char** argv) {
 const unsigned repeats=argc>1?std::max(4, std::atoi(argv[1])):8;
 const unsigned photonCount=argc>3?std::max(1,std::atoi(argv[3])):200000;
 const unsigned ptSPP=argc>4?std::max(128,std::atoi(argv[4])):128;
 seedBudget=argc>5?std::max(1,std::atoi(argv[5])):0;
 const unsigned smsSPP=argc>6?std::max(1,std::atoi(argv[6])):128;
 thresholdOverride=argc>7?std::atof(argv[7]):0;
 if(seedBudget || thresholdOverride>0) g_beforeGradedRender=&AttachSeedBudget;
	auto build = [&]( int mode, int estimator ) {
		std::string s = ReadFile( kSeededScene );
		if( mode == 0 ) s = ReplaceSpan( s, "MEDIUM", UniformBox( 1.4 ) );
		if( mode == 2 ) s = ReplaceSpan( s, "MEDIUM", "" );
		s = ReplaceOnce( s, "\tpta -28 -28 1.0\n\tptb -28 28 1.0\n\tptc 28 28 1.0\n\tptd 28 -28 1.0\n",
			"\tpta 0.5 -0.1 1.0\n\tptb 0.5 0.1 1.0\n\tptc 0.7 0.1 1.0\n\tptd 0.7 -0.1 1.0\n" );
		s = ReplaceOnce( s, "\tscale 1.0\n", "\tscale 40.0\n" );
		s = ReplaceOnce( s, kEmitterObject, kEmitterObject +
			"\ndielectric_material\n{\n\tname mat_ball\n\tior 1.5\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			"sphere_geometry\n{\n\tname geo_ball\n\tradius 0.15\n}\n\n"
			"standard_object\n{\n\tname ball\n\tgeometry geo_ball\n\tmaterial mat_ball\n\tposition 0.3 0 0.55\n}\n" );
        const std::string options=estimator==0 ? "" :
            "\tsms_enabled TRUE\n\tsms_max_iterations 30\n\tsms_threshold 1e-4\n\tsms_max_chain_depth 10\n\tsms_biased TRUE\n\tsms_multi_trials 4\n" +
            (estimator==2 ? "\tsms_photon_count "+std::to_string(photonCount)+"\n" : "\tsms_seeding snell\n");
        return ReplaceSpan(s,"RASTERIZER",PTRasterizerOpts(estimator==0?ptSPP:smsSPP,options));
	};
 g_saltRenders=true;
 for(int mode: {0,2}) {
  if(mode==2 && (argc<3 || std::string(argv[2])!="air")) continue;
  std::vector<double> a,b,c;
  for(unsigned i=0;i<repeats;++i) {
   g_renderIndex=3*i;
   Stat pt=RenderStat(build(mode,0),"331_pt");
   thresholdAttached=false;
   Stat snell=RenderStat(build(mode,1),"331_snell");
   if(thresholdOverride>0 && !thresholdAttached) {std::cerr<<"FAIL: DL331 Snell threshold not installed\n";return 1;}
   seedBudgetAttached=false;thresholdAttached=false;
   Stat photon=RenderStat(build(mode,2),"331_photon");
   if(thresholdOverride>0 && !thresholdAttached) {std::cerr<<"FAIL: DL331 photon threshold not installed\n";return 1;}
   if(seedBudget && !seedBudgetAttached) {std::cerr<<"FAIL: DL331 seed budget was not installed\n";return 1;}
   if(!pt.ok || !snell.ok || !photon.ok) return 1;
   a.push_back(pt.mean); b.push_back(snell.mean); c.push_back(photon.mean);
   std::printf("DL331 salt=%u mode=%d photons=%u seedBudget=%u ptSPP=%u smsSPP=%u PT %.9g snell %.9g photon %.9g\n",i,mode,photonCount,seedBudget?seedBudget:16,ptSPP,smsSPP,pt.mean,snell.mean,photon.mean); std::fflush(stdout);
  }
  auto stats=[](const std::vector<double>& v) { double m=0,ss=0; for(double x:v)m+=x; m/=v.size(); for(double x:v)ss+=(x-m)*(x-m); return std::make_pair(m,std::sqrt(ss/(v.size()-1)/v.size())); };
  auto x=stats(a),y=stats(b),z=stats(c);
  for(const auto& comparison: {std::make_pair("snell",y), std::make_pair("photon",z)}) {
   const auto q=comparison.second;
   const double band=3*std::hypot(x.second,q.second);
   std::printf("DL331 mode=%d n=%u PT %.9g SE %.9g %s %.9g SE %.9g ratio %.7g combined3sigma %.9g within=%d\n",mode,repeats,x.first,x.second,comparison.first,q.first,q.second,q.first/x.first,band,std::abs(x.first-q.first)<=band);
  }
 }
 return 0;
}
