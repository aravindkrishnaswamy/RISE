#include "SMSRenderTestSupport.h"
#include <sstream>
static constexpr double kRho=0.5;
static std::string BlackPainterChunk()
{
	return "uniformcolor_painter\n{\n\tname pnt_black\n\tcolor 0 0 0\n\tcolorspace Rec709RGB_Linear\n}\n\n";
}
static std::string EmitterChunks( const char* pts, double scale )
{
	std::ostringstream ss;
	ss << "uniformcolor_painter\n{\n\tname pnt_emit\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_luminaire_material\n{\n\tname mat_emit\n\texitance pnt_emit\n\tscale " << scale << "\n\tmaterial none\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_emit\n" << pts << "\tdoublesided FALSE\n}\n\n"
	      "standard_object\n{\n\tname obj_emit\n\tgeometry geo_emit\n\tmaterial mat_emit\n}\n\n";
	return ss.str();
}
static std::string BlockerChunks( double y, double half = 0.6 )
{
	std::ostringstream ss;
	ss << BlackPainterChunk()
	   << "lambertian_material\n{\n\tname mat_block\n\treflectance pnt_black\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_block\n\tpta " << -half << " " << y << " " << half << "\n\tptb " << half << " " << y << " " << half << "\n"
	      "\tptc " << half << " " << y << " " << -half << "\n\tptd " << -half << " " << y << " " << -half << "\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_block\n\tgeometry geo_block\n\tmaterial mat_block\n}\n\n";
	return ss.str();
}
static std::string FogSlabScene( bool slab, double sigmaS )
{
	std::ostringstream ss;
	ss << "film\n{\n\twidth 16\n\theight 16\n}\n\n"
	      "pinhole_camera\n{\n\tlocation 0 1 1.2\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 10.0\n}\n\n"
	      "uniformcolor_painter\n{\n\tname pnt_recv\n\tcolor " << kRho << " " << kRho << " " << kRho << "\n\tcolorspace Rec709RGB_Linear\n}\n\n"
	      "lambertian_material\n{\n\tname mat_recv\n\treflectance pnt_recv\n}\n\n"
	      "clippedplane_geometry\n{\n\tname geo_recv\n\tpta -1 0 1\n\tptb 1 0 1\n\tptc 1 0 -1\n\tptd -1 0 -1\n\tdoublesided TRUE\n}\n\n"
	      "standard_object\n{\n\tname obj_recv\n\tgeometry geo_recv\n\tmaterial mat_recv\n}\n\n"
	   << EmitterChunks( "\tpta -1 3 1\n\tptb 1 3 1\n\tptc 1 3 -1\n\tptd -1 3 -1\n", 20.0 )
	   << BlockerChunks( 2.95, 1.1 )
	   << "homogeneous_medium\n{\n\tname fog\n\tabsorption 0 0 0\n\tscattering " << sigmaS << " " << sigmaS << " " << sigmaS << "\n\tphase isotropic\n}\n\n"
	      "global_medium\n{\n\tmedium fog\n}\n\n";
	if( slab ) {
		ss << "uniformcolor_painter\n{\n\tname pnt_refr\n\tcolor 1 1 1\n\tcolorspace Rec709RGB_Linear\n}\n\n"
		      "perfectrefractor_material\n{\n\tname mat_refr\n\trefractance pnt_refr\n\tior 1.5\n}\n\n"
		      "box_geometry\n{\n\tname geo_slab\n\twidth 8\n\theight 0.05\n\tdepth 8\n}\n\n"
		      "standard_object\n{\n\tname slab\n\tgeometry geo_slab\n\tposition 0 2 0\n\tmaterial mat_refr\n}\n\n";
	}
	return ss.str();
}

static std::string Rasterizer( bool sms, bool spectral, bool hwss )
{
    const int spp = hwss ? 512 : (spectral ? 768 : 512);
    return std::string("standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n") +
        (spectral ? "pathtracing_spectral_rasterizer" : "pathtracing_pel_rasterizer") +
        "\n{\n samples " + std::to_string(spp) + "\n rr_min_depth 4\n max_volume_bounce 3\n pixel_filter box\n oidn_denoise FALSE\n sms_enabled " + (sms ? "TRUE" : "FALSE") +
        (spectral ? std::string("\n num_wavelengths 4\n spectral_samples 1\n hwss ") + (hwss ? "TRUE" : "FALSE") : "") + "\n}\n";
}
int main( int argc, char** argv )
{
    if( argc > 1 ) g_seedBase = std::strtoul(argv[1], nullptr, 10);
    const int firstMode=argc>2 ? std::atoi(argv[2]) : 0;
    const int endMode=argc>2 ? firstMode+1 : 3;
    if(firstMode<0 || firstMode>2) return 1;
    g_renderIndex=unsigned(firstMode)*8u;
    for( int mode=firstMode; mode<endMode; ++mode ) {
        constexpr int n = 6;
        std::vector<double> ratios;
        for( int t=0; t<n; ++t ) {
            // Pair each (on, off) render with common random inputs across n=6
            // independent salted pairs; medium scatter clears the SMS anchor so
            // the exact no-defect ratio is 1.0.
            const unsigned int pairIndex = g_renderIndex;
            const auto on = Render("RISE ASCII SCENE 7\n"+Rasterizer(true,mode>0,mode==2)+FogSlabScene(true,0.2),"fog_on");
            g_renderIndex = pairIndex;
            const auto off = Render("RISE ASCII SCENE 7\n"+Rasterizer(false,mode>0,mode==2)+FogSlabScene(true,0.2),"fog_off");
            Check(on.ok && off.ok && off.mean>0,"fog render finite and lit");
            ratios.push_back(off.mean>0 ? on.mean/off.mean : -1);
        }
        const Stats result = Summarize(ratios);
        const double threeSe = 3.0 * result.sd / std::sqrt(double(n));
        std::cout << "DL-340 mode=" << mode << " spp=" << (mode==1 ? 768 : 512) << " slab=1 ratio=" << result.mean << " sd=" << result.sd << " 3se=" << threeSe << " n=" << n << std::endl;
        // Salted n=6 CRN pairs at 16x16 @ 512/768/512 spp, nw=4, max_volume_bounce=3.
        // Fixed |ratio - 1| <= 0.035 gate (never widened by SE) rejects the ~5.8%
        // DL-340 defect cleanly; 3*SE <= 0.045 guarantees SE <= 0.015 precision.
        Check(threeSe <= 0.045 && std::fabs(result.mean - 1.0) <= 0.035,
            "medium scatter clears SMS anchor (slab on/off ratio agrees with 1)");
    }
    std::cout << passCount << " passed, " << failCount << " failed" << std::endl;
    return failCount ? 1 : 0;
}
