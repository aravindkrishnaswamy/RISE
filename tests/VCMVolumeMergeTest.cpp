//////////////////////////////////////////////////////////////////////
//
//  VCMVolumeMergeTest.cpp - DL-469: VCM merges at MEDIUM vertices too.
//
//  Before DL-469 VCM merged on surfaces only while its MIS recurrence
//  counted a merge at every medium vertex (the surface factor), so
//    A  fog behind delta walls lit by a point light and seen through
//       them -- single scattering sandwiched between delta surfaces --
//       had NO strategy at all (fog box: VCM 0.58 of PT), and
//    B  every medium path lost the MIS mass of that phantom merge
//       whenever surface merging was on (global fog, forced merge
//       radius: VCM 0.977 of PT).
//  The fix makes the counted strategy real (a 3-D volume merge with its
//  own MIS factor eta_v; VCMIntegrator.cpp "volume merging").
//
//  Rows (salted, n renders each, ratio of means, VCM / PT):
//    A  an index-1.0 perfect-refractor box carrying isotropic fog, no
//       other surface, omni light outside, camera outside.
//    B  a global absorbing/scattering medium over a Lambertian floor,
//       omni light, VCM with merge_radius 0.05 (surface merging on).
//    D  A with medium light vertices stored with probability 0.2 (the
//       memory thinning): unchanged within the same band.
//    E  A's fog box with a floor inside (surface and medium light vertices
//       in one store), medium vertices stored at q = 0.01 vs q = 1 (both
//       VCM): the firefly clamps must judge a thinned vertex on its
//       physical throughput, so the clamp must not move the mean (old
//       0.632).  q still moves the merge radius (r ~ q^-1/3 until the
//       footprint clip binds), hence the boundary bias of DL-474; the
//       +/-0.2 band guards gross clamp regressions, not that residual.
//    C  the ledger scene (tests/SMSMediumTransmittanceTest.cpp row A: a
//       Lambertian sphere inside A's box) -- printed only: its VCM
//       variance is heavy-tailed (the sphere's light reaches it only by
//       surface merging at the auto radius; per-pair sd up to 0.23 at
//       n = 8).  Measured at n = 48: 1.004 +/- 0.017 (pre-fix 0.80).
//
//  Usage: VCMVolumeMergeTest [--trials n] [--only <label substring>]
//
//////////////////////////////////////////////////////////////////////

#include "VCMVolumeMergeFixture.h"

int main( int argc, char** argv )
{
	bool boundaryOnly = false, thinningPTOnly = false, cornerOnly=false;
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--dl474-corner-pin") cornerOnly=true;
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--dl474-boundary-only") boundaryOnly=true;
    for(int i=1;i<argc;++i) if(std::string(argv[i])=="--dl474-thinning-pt-only") thinningPTOnly=true;
    std::string boundaryOptions;
    if(boundaryOnly || thinningPTOnly || cornerOnly) {
        boundaryOptions=WriteScene(std::string("force_number_of_threads 1\nrender_thread_reserve_count 0\n") +
            ((boundaryOnly || cornerOnly) ? "vcm_volume_merge_neighbours 1\nvcm_volume_merge_footprints 64\n" : ""), "opts");
        #ifdef _WIN32
        _putenv_s("RISE_OPTIONS_FILE",boundaryOptions.c_str());
#else
        setenv("RISE_OPTIONS_FILE",boundaryOptions.c_str(),1);
#endif
        if(((boundaryOnly || cornerOnly) && (GlobalOptions().ReadDouble("vcm_volume_merge_neighbours",0.01)!=1 ||
           GlobalOptions().ReadDouble("vcm_volume_merge_footprints",2)!=64)) ||
           GlobalOptions().ReadInt("force_number_of_threads",0)!=1) return 2;
    }
    unsigned int trials = 8;
	for( int i = 1; i < argc; ++i ) {
		const std::string a( argv[i] );
		if( a == "--trials" && i + 1 < argc ) trials = static_cast<unsigned int>( std::atoi( argv[++i] ) );
	}
	if( trials < 2 ) trials = 2;
	std::cout << "=== DL-469 VCM volume merging, n=" << trials << " ===" << std::endl;
	unsigned int seed = 46900;
	std::string only;
	for( int i = 1; i < argc; ++i ) {
		if( std::string( argv[i] ) == "--only" && i + 1 < argc ) only = argv[++i];
	}

    if(cornerOnly) {
        auto corner=[](bool vcm) {
            std::string scene=FogBoxScene(vcm,512,false);
            const std::string camera="location 0 0 5\n\tlookat 0 0 0";
            scene.replace(scene.find(camera),camera.size(),"location 1.4 1.4 5\n\tlookat 1.4 1.4 0");
            return scene;
        };
        RatioRow("G: own-box corner VCM / PT",corner(true),corner(false),1.0,-1.0,trials,seed,"");
        std::remove(boundaryOptions.c_str());
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1:0;
    }
    if(boundaryOnly) {
        std::string blackVCM=FogBoxScene(true,512,true), blackPT=FogBoxScene(false,512,true);
        const std::string white="\treflectance white";
        for(auto* scene : {&blackVCM,&blackPT}) {
            scene->insert(scene->find("lambertian_material"), "uniformcolor_painter\n{\n\tname black\n\tcolor 0 0 0\n\tcolorspace Rec709RGB_Linear\n}\n\n");
            scene->replace(scene->find(white),white.size(),"\treflectance black");
        }
        RatioRow("F: black-sphere fog box K1/64-footprint VCM / PT",blackVCM,blackPT,1.0,-1.0,trials,seed,"");
        std::remove(boundaryOptions.c_str());
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
    if(thinningPTOnly) {
        VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store(.01);
        RatioRow("E-PT: fog floor box q.01 VCM / PT",FogFloorBoxScene(true,512),
                 FogFloorBoxScene(false,512),1.0,-1.0,std::max(16u,trials),seed,"");
        VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store(0);
        std::remove(boundaryOptions.c_str());
        std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
        return failCount ? 1 : 0;
    }
	// A: pre-fix 0.58 (the single-scatter share had no strategy).
	RatioRow( "A: fog box, VCM / PT",
		FogBoxScene( true, 512, false ), FogBoxScene( false, 64, false ), 1.0, 0.06, trials, seed, only );
	// B: pre-fix 0.977 (phantom medium merge in the MIS denominators).
	RatioRow( "B: global fog, forced surface merging, VCM / PT",
		GlobalFogScene( true, 64 ), GlobalFogScene( false, 512 ), 1.0, 0.015, trials, seed, only );
	// D: A's scene with medium light vertices stored with probability
	// q = 0.2 (the memory thinning; DL-469 review P1).  Throughput / q and
	// the MIS factor q eta_v keep the estimator and the partition exact,
	// so the ratio must not move.
	VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store( 0.2 );
	RatioRow( "D: fog box, store probability 0.2, VCM / PT",
		FogBoxScene( true, 512, false ), FogBoxScene( false, 64, false ), 1.0, 0.06, trials, seed, only );
	VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store( 0.0 );

	// E: a store holding surface AND medium light vertices (A's fog box
	// with a Lambertian floor inside), medium vertices stored at q = 0.01
	// against q = 1, both VCM.  The store's two firefly clamps derive
	// their threshold from the surface vertices; a thinned medium vertex
	// carries throughput / q, so comparing that stored value capped its
	// PHYSICAL throughput at threshold * q -- a bias growing as q falls
	// (DL-469 review round 2).  They now judge q * stored.  Measured
	// (salted, 1024 spp): old clamp 0.632, n = 8, per-pair sd 0.040;
	// fixed 0.936, n = 32, per-pair sd 0.177 (q = 0.01 keeps 1 % of the
	// medium vertices, so the test render is noisy and heavy-tailed; the
	// residual is within 2 SE and includes the larger volume radius a
	// thinner store gets, DL-474).  At q = 0.05 the old clamp read
	// 1.037 +/- 0.038 (n = 8): too little to see, hence q = 0.01.
	{
		const unsigned int trialsE = trials < 16 ? 16 : trials;
		RatioRow( "E: fog box with a floor, medium store probability 0.01 / 1, VCM",
			FogFloorBoxScene( true, 1024 ), FogFloorBoxScene( true, 1024 ), 1.0, 0.2, trialsE, seed, only, 0.01, 1.0 );
	}
	VCMRasterizerBase::TestVolumeStoreProbabilityOverride().store( 0.0 );

	// C: the ledger row; pre-fix 0.80 (printed: heavy-tailed, see header).
	RatioRow( "C: ledger fog box with sphere, VCM / PT (printed)",
		FogBoxScene( true, 1024, true ), FogBoxScene( false, 64, true ), 1.0, 0.0, trials, seed, only );

	std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
	return failCount ? 1 : 0;
}
