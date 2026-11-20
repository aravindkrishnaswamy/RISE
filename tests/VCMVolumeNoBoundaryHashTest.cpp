// DL-628: single-threaded heterogeneous-medium tile-seed determinism.
#include "VCMVolumeMergeFixture.h"
#include <cstdint>
int main() {
    const std::string opts=WriteScene("force_number_of_threads 1\nrender_thread_reserve_count 0\n", "hashopts");
#ifdef _WIN32
    _putenv_s("RISE_OPTIONS_FILE",opts.c_str());
#else
    setenv("RISE_OPTIONS_FILE",opts.c_str(),1);
#endif
    // DL-628: under single-threaded execution (`force_number_of_threads 1`), both
    // `PixelBasedRasterizerHelper` (`pathtracing_pel_rasterizer`) and `BDPTRasterizerBase`
    // (`bdpt_pel_rasterizer`) must seed `ScopedMediumTileSeed` from `LockedLibcRand()`
    // per pass so identical `std::srand(s)` reproduces bit-identically while distinct
    // `std::srand(s1)` vs `std::srand(s2)` advance `HeterogeneousMedium` ratio-tracking
    // transmittance even when `SobolSampler` is unsalted (`ValueSalt == 0`).
    auto renderHetHash = []( const char* rastBlock, unsigned int libcSeed ) -> uint64_t {
        std::string hetScene =
            "RISE ASCII SCENE 7\n"
            "film\n{\n\twidth 8\n\theight 8\n}\n"
            "pinhole_camera\n{\n\tlocation 0 0 2.5\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 40\n}\n"
            "uniformcolor_painter\n{\n\tname p_hi\n\tcolor 1 1 1\n}\n"
            "uniformcolor_painter\n{\n\tname p_lo\n\tcolor 0 0 0\n}\n"
            "uniformcolor_painter\n{\n\tname p_alb\n\tcolor 0.8 0.8 0.8\n}\n"
            "perlin3d_painter\n{\n\tname p_dens\n\tpersistence 0.65\n\toctaves 3\n\tcolora p_lo\n\tcolorb p_hi\n\tscale 3 3 3\n\tshift 0 0 0\n}\n"
            "painter_heterogeneous_medium\n{\n\tname g_het\n\tabsorption 1.5 1.5 1.5\n\tscattering 0.5 0.5 0.5\n\tphase isotropic\n\tdensity_painter p_dens\n\tbbox_min -4 -4 -4\n\tbbox_max 4 4 4\n\tresolution 16\n}\n"
            "global_medium\n{\n\tmedium g_het\n}\n"
            "lambertian_material\n{\n\tname m_wall\n\treflectance p_alb\n}\n"
            "clippedplane_geometry\n{\n\tname g_wall\n\tpta -3 -3 0\n\tptb 3 -3 0\n\tptc 3 3 0\n\tptd -3 3 0\n}\n"
            "standard_object\n{\n\tname wall\n\tgeometry g_wall\n\tmaterial m_wall\n}\n"
            "omni_light\n{\n\tname l0\n\tpower 40\n\tcolor 1 1 1\n\tposition 0.5 0.5 2.0\n}\n"
            "standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n"
            + std::string( rastBlock );
        const std::string p = WriteScene( hetScene, "het_sp_seed" );
        IJobPriv* j = nullptr;
        if( !RISE_CreateJobPriv( &j ) || !j || !j->LoadAsciiSceneViaCst( p.c_str() ) ) {
            std::remove( p.c_str() );
            safe_release( j );
            return 0;
        }
        std::remove( p.c_str() );
        j->RemoveRasterizerOutputs();
        auto* c = new CapturingRasterizerOutput();
        j->GetRasterizer()->AddRasterizerOutput( c );
        GlobalRNG().Reseed( 999u );
        std::srand( libcSeed );
        const bool ok = j->Rasterize() && !c->pixels.empty();
        uint64_t h = 14695981039346656037ull;
        if( ok ) {
            for( const auto& px : c->pixels ) {
                const Scalar ch[] = { px.base.r, px.base.g, px.base.b, px.a };
                for( const Scalar v : ch ) {
                    const auto* b = reinterpret_cast<const unsigned char*>( &v );
                    for( size_t k = 0; k < sizeof( v ); ++k ) { h ^= b[k]; h *= 1099511628211ull; }
                }
            }
        } else {
            h = 0;
        }
        safe_release( c );
        safe_release( j );
        return h;
    };
    const char* kPtBlock =
        "pathtracing_pel_rasterizer\n{\n\tsamples 1\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n";
    const char* kBdptBlock =
        "bdpt_pel_rasterizer\n{\n\tsamples 1\n\tmax_eye_depth 2\n\tmax_light_depth 1\n\tpixel_filter box\n\toidn_denoise FALSE\n}\n";
    const uint64_t ptA1 = renderHetHash( kPtBlock, 101u );
    const uint64_t ptA2 = renderHetHash( kPtBlock, 101u );
    const uint64_t ptB  = renderHetHash( kPtBlock, 202u );
    const uint64_t bdA1 = renderHetHash( kBdptBlock, 303u );
    const uint64_t bdA2 = renderHetHash( kBdptBlock, 303u );
    const uint64_t bdB  = renderHetHash( kBdptBlock, 404u );
    std::cout << "SP het hashes PT=(" << ptA1 << "," << ptA2 << "," << ptB
              << ") BDPT=(" << bdA1 << "," << bdA2 << "," << bdB << ")\n";
    Check( ptA1 != 0 && ptA1 == ptA2 && ptA1 != ptB, "DL-628 PT single-thread het-medium seed determinism" );
    Check( bdA1 != 0 && bdA1 == bdA2 && bdA1 != bdB, "DL-628 BDPT single-thread het-medium seed determinism" );
    std::remove( opts.c_str() );
    std::cout << passCount << " passed, " << failCount << " failed\n";
    return failCount ? 1 : 0;
}
