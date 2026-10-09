// Legacy-SMS debt rows measured against extended SMS (`sms_extended TRUE`).
// Every comparison is salted (Render() hashes seed+index into the Sobol'
// salt) and gated on combined standard errors, never on a fixed band alone.
// Default run (no arguments) executes the regression gates below; the
// exploratory sections print measurements and gate only where noted.
//   dispersion  DL-438 (RGB per-component index, channel-separated centroids
//               against wavelength-isolated spectral truth) and DL-353
//               (spectral dispersion, hwss FALSE/TRUE)            [gated]
//   slab        DL-421 / DL-420 (glass slab: spot, omni, area vs depth-matched
//               BDPT/VCM; legacy uniform/snell as controls)       [gated]
//   ball        DL-376 / DL-445 (DL-372 ball-lens image vs PT and VCM) [gated]
//   slab-closed DL-455 omni slab direct caustic per chain length vs an
//               unfolded-image closed form                       [gated]
//   nested, nested-hwss, nested-hwss-const
//               DL-391 nested dispersive exterior probes          [measurement]
// Options: --section <name> --lights spot,omni,area --modes a,b --n N --extspp S
//          --seed B (salt base) --ball-y Y (DL-455 focus discriminator)
//          --no-slab (direct-light calibration, mode ptplain)
// Slab modes: extended[:tb], extd[:tb] (extended, direct caustic only:
// max_diffuse_bounce 0), uniform/snell[:tb], bdpt, bdpt8, bdpt16, vcm,
// vcm16, ptplain. SMS_ROWS_VERBOSE=1 prints every salted render.
#include "SMSRenderTestSupport.h"
#include <sstream>
#include <iomanip>
#include <array>
#include <map>

namespace
{
    struct Series {
        std::vector<double> v;
        double mean() const { return Summarize(v).mean; }
        double se() const { return v.size()>1 ? Summarize(v).sd/std::sqrt(double(v.size())) : 0; }
    };
    bool Agree3( const Series& a, const Series& b, double floorAbs=0 )
    {
        return std::fabs(a.mean()-b.mean()) <= 3*std::hypot(a.se(),b.se())+floorAbs;
    }
    void Report( const std::string& label, const Series& a, const Series& b )
    {
        std::cout<<std::setprecision(7)<<label<<": "<<a.mean()<<" +/- "<<a.se()<<" vs "<<b.mean()<<" +/- "<<b.se()
            <<"  diff="<<a.mean()-b.mean()<<" +/- "<<std::hypot(a.se(),b.se())<<"  n="<<a.v.size()<<std::endl;
    }

    // ---------------- dispersion fixture (SMSUniformDispersionTest's) ----
    std::string Sms( bool spectral, bool hwss, double nm, const std::string& mode, unsigned spp )
    {
        std::ostringstream r;
        r<<(spectral ? "pathtracing_spectral_rasterizer" : "pathtracing_pel_rasterizer")
         <<"\n{\n samples "<<spp<<"\n oidn_denoise FALSE\n pixel_filter box\n sms_enabled TRUE\n sms_target_bounces 1\n sms_multi_trials 4\n";
        if( mode=="extended" ) r<<" sms_extended TRUE\n";
        else r<<" sms_seeding "<<mode<<"\n sms_biased TRUE\n";
        if( spectral ) r<<" num_wavelengths 1\n spectral_samples 1\n hwss "<<(hwss?"TRUE":"FALSE")<<"\n nmbegin "<<nm<<"\n nmend "<<nm+0.01<<"\n";
        r<<"}\n";
        return r.str();
    }
    std::string PaneScene( bool dispersive, const std::string& raster )
    {
        std::ostringstream s;
        s<<"RISE ASCII SCENE 7\nfilm\n{\n width 64\n height 16\n}\n"
           "orthographic_camera\n{\n location 0 0.5 0\n lookat 0 0 0\n up 0 0 1\n viewport_scale 3 1.5\n}\n"
           "uniformcolor_painter\n{\n name grey\n color 0.5 0.5 0.5\n colorspace Rec709RGB_Linear\n}\n"
           "uniformcolor_painter\n{\n name white\n color 1 1 1\n colorspace Rec709RGB_Linear\n}\n"
           "lambertian_material\n{\n name floor_mat\n reflectance grey\n}\n";
        if( dispersive ) s<<"scalar_painter\n{\n name sf11\n sellmeier 1.73759695 0.313747346 1.898781010 0.013188707 0.0623068142 155.23629\n}\n";
        s<<"perfectrefractor_material\n{\n name glass\n refractance white\n ior "<<(dispersive?"sf11":"1.78")<<"\n}\n"
           "clippedplane_geometry\n{\n name floor\n pta -5 0 5\n ptb 5 0 5\n ptc 5 0 -5\n ptd -5 0 -5\n doublesided FALSE\n}\n"
           "standard_object\n{\n name receiver\n geometry floor\n material floor_mat\n}\n"
           "clippedplane_geometry\n{\n name caster\n pta -0.5 1 -0.5\n ptb 0.5 1 -0.5\n ptc 0.5 1 0.5\n ptd -0.5 1 0.5\n doublesided TRUE\n}\n"
           "standard_object\n{\n name pane\n geometry caster\n material glass\n}\n"
           "omni_light\n{\n name source\n position 0.5 2 0\n color 1 1 1\n power 40\n}\n"
           "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"<<raster;
        return s.str();
    }
    // Per-channel (0,1,2) intensity-weighted column centroid, plus total.
    std::array<double,5> Centroids( const RenderResult& r )
    {
        std::array<double,3> w{0,0,0}, s{0,0,0};
        for( size_t i=0;i<r.pixels.size();++i ) {
            const auto& c=r.pixels[i];
            const double v[3]={c.base.r*c.a,c.base.g*c.a,c.base.b*c.a};
            for( int k=0;k<3;++k ) { w[k]+=v[k]*(double(i%64)+0.5); s[k]+=v[k]; }
        }
        const double st=s[0]+s[1]+s[2], wt=w[0]+w[1]+w[2];
        return { s[0]>0?w[0]/s[0]:-1, s[1]>0?w[1]/s[1]:-1, s[2]>0?w[2]/s[2]:-1, st/3, st>0?wt/st:-1 };
    }



    std::string CubeMesh( bool reverse );
    double Sf11( double nm );
    // ---------------- nested dispersive exterior (DL-391) ------------------
    std::string NestedScene( const std::string& raster )
    {
        std::ostringstream s;
        s<<"RISE ASCII SCENE 7\nfilm\n{\n width 64\n height 16\n}\n"
           "orthographic_camera\n{\n location 0 0.5 0\n lookat 0 0 0\n up 0 0 1\n viewport_scale 3 1.5\n}\n"
           "uniformcolor_painter\n{\n name grey\n color 0.5 0.5 0.5\n colorspace Rec709RGB_Linear\n}\n"
           "uniformcolor_painter\n{\n name white\n color 1 1 1\n colorspace Rec709RGB_Linear\n}\n"
           "lambertian_material\n{\n name floor_mat\n reflectance grey\n}\n"
           "scalar_painter\n{\n name sf11\n sellmeier 1.73759695 0.313747346 1.898781010 0.013188707 0.0623068142 155.23629\n}\n"
           "perfectrefractor_material\n{\n name outer_glass\n refractance white\n ior sf11\n}\n"
           "perfectrefractor_material\n{\n name inner_glass\n refractance white\n ior 1.3\n}\n"
           "clippedplane_geometry\n{\n name floor\n pta -5 0 5\n ptb 5 0 5\n ptc 5 0 -5\n ptd -5 0 -5\n doublesided FALSE\n}\n"
           "standard_object\n{\n name receiver\n geometry floor\n material floor_mat\n}\n"
        <<CubeMesh(false)
        <<"standard_object\n{\n name outer\n geometry shape\n material outer_glass\n position 0 1.1 0\n scale 3 0.3 3\n}\n"
           "sphere_geometry\n{\n name ball_geom\n radius 0.2\n}\n"
           "standard_object\n{\n name ball\n geometry ball_geom\n material inner_glass\n position 0.3 1.1 0\n}\n"
           "omni_light\n{\n name source\n position 0.5 3 0\n color 1 1 1\n power 40\n}\n"
           "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"<<raster;
        return s.str();
    }
    std::string SpecRaster( const std::string& mode, bool hwss, double nmb, double nme, unsigned nw, unsigned spp )
    {
        std::ostringstream r;
        const bool b=mode=="bdpt";
        r<<(b?"bdpt_spectral_rasterizer":"pathtracing_spectral_rasterizer")<<"\n{\n samples "<<spp<<"\n oidn_denoise FALSE\n pixel_filter box\n";
        if( b ) r<<" max_eye_depth 3\n max_light_depth 6\n";
        else {
            r<<" sms_enabled TRUE\n sms_multi_trials 2\n";
            if( mode=="extended" ) r<<" sms_extended TRUE\n"; else r<<" sms_seeding "<<mode<<"\n sms_biased TRUE\n";
        }
        r<<" num_wavelengths "<<nw<<"\n spectral_samples 1\n hwss "<<(hwss?"TRUE":"FALSE")<<"\n nmbegin "<<nmb<<"\n nmend "<<nme<<"\n}\n";
        return r.str();
    }
    // Total intensity in the ball's column band, and over the whole frame.
    std::array<double,2> BandTotals( const RenderResult& r )
    {
        double band=0, all=0;
        for( size_t i=0;i<r.pixels.size();++i ) {
            const auto& c=r.pixels[i]; const double v=(c.base.r+c.base.g+c.base.b)*c.a/3;
            all+=v; const unsigned x=i%64; if( x>=26 && x<=52 ) band+=v;
        }
        return { band/(27.0*16), all/r.pixels.size() };
    }
    void Nested( bool hwssOnly=false, bool constantOuter=false )
    {
        // Invariant: a monochromatic render of the dispersive outer medium at
        // wavelength L equals the same scene with the outer index replaced by
        // the constant n(L).  Legacy chains keep the SEEDED exterior index, so
        // an unrefreshed nested exterior breaks it; extended replays the stack.
        struct Case { const char* label; double nb, ne; unsigned nw; bool hwss; };
        for( const Case& c : { Case{"450nm",450,450.01,1,false}, Case{"650nm",650,650.01,1,false},
                               Case{"hwss-vs-nohwss 450-650 nw4 (UNMATCHED quadrature, DL-456 contrast)",450,650,4,true},
                               Case{"hwss-vs-nohwss 450-650 nw160 (matched quadrature)",450,650,160,true} } ) {
            if( hwssOnly && !c.hwss ) continue;
            for( const char* mode : {"snell","uniform","extended"} ) {
                const unsigned spp = std::string(mode)=="extended" ? 512 : 256;
                Series disp, flat;
                // The constant control index: n at the lane wavelength (hwss
                // spans a band, so its control is the 450 and 650 constants
                // mixed 1:1 by the invariant only when mono; skipped there).
                const bool mono = !c.hwss;
                for( unsigned t=0;t<6;++t ) {
                    g_renderIndex=t;
                    std::string dscene=NestedScene(SpecRaster(mode,c.hwss,c.nb,c.ne,c.nw,spp));
                    if( constantOuter ) dscene.replace(dscene.find("ior sf11"),8,"ior 1.8");
                    const auto r=Render(dscene,"nested");
                    Check(r.ok&&r.mean>0,std::string("DL-391 ")+mode+" lit");
                    disp.v.push_back(BandTotals(r)[0]);
                    g_renderIndex=t;
                    std::string scene=NestedScene(SpecRaster(mode,false,c.nb,c.ne,c.nw,spp));
                    if( constantOuter ) scene.replace(scene.find("ior sf11"),8,"ior 1.8");
                    else if( mono ) scene.replace(scene.find("ior sf11"),8,"ior "+std::to_string(Sf11(c.nb)));
                    const auto q=Render(scene,"nested_ref");
                    Check(q.ok&&q.mean>0,std::string("DL-391 reference ")+mode+" lit");
                    flat.v.push_back(BandTotals(q)[0]);
                }
                Report(std::string("DL-391 ")+c.label+" "+mode+(mono?" dispersive vs constant-n(L) band":" hwss TRUE vs hwss FALSE band"),disp,flat);
                if( mono && std::string(mode)=="extended" ) Check(Agree3(disp,flat),std::string("DL-391 ")+c.label+": extended dispersive outer = constant n(L) outer");
            }
        }
    }

    // ---------------- DL-372 ball lens (DL-376 / DL-445) -------------------
    std::string g_ballY = "1.5";
    std::string BallScene( const std::string& raster )
    {
        std::ostringstream ss;
        ss << "RISE ASCII SCENE 7\nstandard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n\n"
              "film\n{\n\twidth 24\n\theight 24\n}\n\n"
              "pinhole_camera\n{\n\tlocation 0 4 8\n\tlookat 0 0 0\n\tup 0 1 0\n\tfov 6.0\n}\n\n"
              "uniformcolor_painter\n{\n\tname pnt_floor\n\tcolor 0.7 0.7 0.7\n}\n\n"
              "uniformcolor_painter\n{\n\tname pnt_light\n\tcolor 1.0 1.0 1.0\n}\n\n"
              "uniformcolor_painter\n{\n\tname pnt_glass_tau\n\tcolor 0.999 0.999 0.999\n}\n\n"
              "lambertian_material\n{\n\tname floor_mat\n\treflectance pnt_floor\n}\n\n"
              "lambertian_luminaire_material\n{\n\tname light_mat\n\texitance pnt_light\n\tscale 500.0\n\tmaterial none\n}\n\n"
              "perfectrefractor_material\n{\n\tname glass_mat\n\trefractance pnt_glass_tau\n\tior 1.5\n}\n\n"
              "sphere_geometry\n{\n\tname sphere_geom\n\tradius 1.0\n}\n\n"
              "clippedplane_geometry\n{\n\tname floor_geom\n\tpta -5.0 0.0 -5.0\n\tptb -5.0 0.0 5.0\n\tptc 5.0 0.0 5.0\n\tptd 5.0 0.0 -5.0\n}\n\n"
              "clippedplane_geometry\n{\n\tname light_geom\n\tpta -1.5 0.0 -1.5\n\tptb 1.5 0.0 -1.5\n\tptc 1.5 0.0 1.5\n\tptd -1.5 0.0 1.5\n}\n\n"
              "standard_object\n{\n\tname floor\n\tgeometry floor_geom\n\tmaterial floor_mat\n}\n\n"
              "standard_object\n{\n\tname glass_sphere\n\tgeometry sphere_geom\n\tposition 0 "<<g_ballY<<" 0\n\tmaterial glass_mat\n}\n\n"
              "standard_object\n{\n\tname area_light\n\tgeometry light_geom\n\tposition 0 5.0 0\n\tmaterial light_mat\n}\n\n";
        return ss.str()+raster;
    }
    std::string g_ballThreshold = "1e-4"; // DL-445's measured configuration (SMSExtendedPartitionTest ExtendedImage)
    std::string BallRaster( const std::string& mode, unsigned trials, unsigned tb, unsigned spp )
    {
        std::ostringstream r;
        if( mode=="pt" ) return "pathtracing_pel_rasterizer\n{\n samples "+std::to_string(spp)+"\n rr_min_depth 8\n pixel_filter box\n oidn_denoise FALSE\n sms_enabled FALSE\n}\n";
        if( mode=="vcm" ) return "vcm_pel_rasterizer\n{\n samples "+std::to_string(spp)+"\n max_eye_depth 8\n max_light_depth 8\n merge_radius 0.0\n vc_enabled true\n vm_enabled true\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        r<<"pathtracing_pel_rasterizer\n{\n samples "<<spp<<"\n rr_min_depth 8\n pixel_filter box\n oidn_denoise FALSE\n sms_enabled TRUE\n sms_threshold "<<g_ballThreshold<<"\n sms_target_bounces "<<tb<<"\n sms_multi_trials "<<trials<<"\n";
        if( mode=="extended" ) r<<" sms_extended TRUE\n"; else r<<" sms_seeding "<<mode<<"\n sms_biased TRUE\n";
        r<<"}\n";
        return r.str();
    }
    void Ball( std::string only, unsigned n, unsigned spp )
    {
        if( only.empty() ) only="PT (SMS off),VCM,extended tb0 trials1,extended tb0 trials2,legacy snell tb0 trials2,legacy uniform tb2 trials1";
        struct Mode { const char* name; const char* label; unsigned trials; unsigned tb; };
        const Mode modes[]={ {"pt","PT (SMS off)",1,0}, {"vcm","VCM",1,0},
            {"extended","extended tb0 trials1",1,0}, {"extended","extended tb0 trials2",2,0},
            {"extended","extended tb2 trials1",1,2}, {"extended","extended tb2 trials2",2,2},
            {"snell","legacy snell tb0 trials1",1,0}, {"snell","legacy snell tb0 trials2",2,0}, {"snell","legacy snell tb2 trials2",2,2},
            {"uniform","legacy uniform tb2 trials1",1,2}, {"uniform","legacy uniform tb2 trials16",16,2},
            {"uniform","legacy uniform tb0 trials1",1,0} };
        std::map<std::string,Series> m;
        for( const Mode& md : modes ) {
            if( !only.empty() && ("," + only + ",").find(std::string(",")+md.label+",")==std::string::npos ) continue;
            for( unsigned t=0;t<n;++t ) {
                g_renderIndex=t;
                const auto r=Render(BallScene(BallRaster(md.name,md.trials,md.tb,spp)),"ball");
                Check(r.ok&&r.mean>0,std::string("ball ")+md.label+" lit");
                m[md.label].v.push_back(r.mean);
                if( std::getenv("SMS_ROWS_VERBOSE") ) std::cout<<"BALLRUN "<<md.label<<" "<<t<<" "<<std::setprecision(9)<<r.mean<<std::endl;
            }
            std::cout<<"BALL "<<md.label<<" mean="<<m[md.label].mean()<<" +/- "<<m[md.label].se()<<std::endl;
        }
        for( const Mode& md : modes ) {
            if( m.find(md.label)==m.end() ) continue;
            for( const char* ref : {"PT (SMS off)","VCM"} ) {
                if( m.find(ref)==m.end() ) continue;
                const double r1=m[md.label].mean()/m[ref].mean();
                std::cout<<"BALL RESULT "<<md.label<<"/"<<ref<<" = "<<r1<<" +/- "
                    <<r1*std::hypot(m[md.label].se()/m[md.label].mean(),m[ref].se()/m[ref].mean())<<std::endl;
            }
        }
        for( const char* l : {"extended tb0 trials1","extended tb0 trials2","extended tb2 trials1","extended tb2 trials2"} )
            if( m.find(l)!=m.end() ) {
                // DL-455: the -0.7..-1.6 % read at n 8 did not survive more
                // salts (skew ~2): n 192 tb0 trials1 0.9998 +/- 0.0018, n 96
                // tb0 trials2 / tb2 trials1 1.0034 +/- 0.0029 / 0.0028. 1 %
                // floor, against legacy uniform's +14 % / -70 %.
                const Series &e=m[l], &p=m["PT (SMS off)"];
                Check(std::fabs(e.mean()/p.mean()-1) <= 3*std::hypot(e.se()/e.mean(),p.se()/p.mean())+0.01,
                      std::string("ball: ")+l+" = PT within 3 combined se + 1 %");
            }
    }

    // ---------------- glass slab (SMSExtendedReferenceTest's SlabScene) ----
    std::string CubeMesh( bool reverse )
    {
        std::ostringstream s;
        s<<"indexedmesh_geometry\n{\n name shape\n double_sided TRUE\n face_normals TRUE\n";
        const double p[8][3]={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};
        for( auto& v : p ) s<<" vertex "<<v[0]<<' '<<v[1]<<' '<<v[2]<<'\n';
        for( int i=0;i<8;++i ) s<<" uv "<<(i%2)<<' '<<((i/2)%2)<<'\n';
        const int t[][3]={{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},{3,7,6},{3,6,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5}};
        for( int i=0;i<12;++i ) s<<" triangle "<<t[i][0]<<' '<<t[i][reverse?2:1]<<' '<<t[i][reverse?1:2]<<'\n';
        return s.str()+"}\n";
    }
    bool g_noSlab = false;
    std::string SlabScene( const std::string& light, const std::string& raster )
    {
        std::string text="RISE ASCII SCENE 7\nuniformcolor_painter\n{\n name white\n color 1 1 1\n}\n"
            "perfectrefractor_material\n{\n name glass\n refractance white\n ior 1.5\n}\n"
            "standard_shader\n{\n name global\n shaderop DefaultPathTracing\n}\n"
            "film\n{\n width 16\n height 16\n}\n"
            "pinhole_camera\n{\n location 0 0 -1.9\n lookat 0 0 -2\n up 0 1 0\n fov 174.275189547777\n}\n"
            "lambertian_material\n{\n name diffuse\n reflectance white\n}\n";
        text+=CubeMesh(false)+(g_noSlab?std::string():std::string("standard_object\n{\n name slab\n geometry shape\n material glass\n scale 3 3 0.25\n}\n"))+
            "clippedplane_geometry\n{\n name floor\n pta -4 -2 -2\n ptb -4 2 -2\n ptc 4 2 -2\n ptd 4 -2 -2\n doublesided TRUE\n}\n"
            "standard_object\n{\n name receiver\n geometry floor\n material diffuse\n}\n";
        return text+light+raster;
    }
    const char* kSpot="spot_light\n{\n name source\n position 0.5 0 2\n target 0.5 0 -2\n color 1 1 1\n power 40\n inner 30\n outer 45\n}\n";
    const char* kOmni="omni_light\n{\n name source\n position 0.5 0 2\n color 1 1 1\n power 40\n}\n";
    const char* kArea="uniformcolor_painter\n{\n name lightc\n color 1 1 1\n}\n"
        "lambertian_luminaire_material\n{\n name lum\n exitance lightc\n scale 40\n material none\n}\n"
        "clippedplane_geometry\n{\n name emitter_geo\n pta 0 -0.5 3\n ptb 1 -0.5 3\n ptc 1 0.5 3\n ptd 0 0.5 3\n doublesided TRUE\n}\n"
        "standard_object\n{\n name emitter\n geometry emitter_geo\n material lum\n}\n";
    // `mode` is "<base>" or "<base>:<sms_target_bounces>".
    std::string SlabRaster( const std::string& modeArg, unsigned spp )
    {
        std::string mode=modeArg, tb="2";
        const auto colon=modeArg.find(':');
        if( colon!=std::string::npos ) { mode=modeArg.substr(0,colon); tb=modeArg.substr(colon+1); }
        std::ostringstream r;
        if( mode=="bdpt" ) r<<"bdpt_pel_rasterizer\n{\n samples "<<spp<<"\n max_eye_depth 2\n max_light_depth 4\n rr_min_depth 20\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        else if( mode=="bdpt8" ) r<<"bdpt_pel_rasterizer\n{\n samples "<<spp<<"\n max_eye_depth 8\n max_light_depth 8\n rr_min_depth 20\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        else if( mode=="bdpt16" ) r<<"bdpt_pel_rasterizer\n{\n samples "<<spp<<"\n max_eye_depth 16\n max_light_depth 16\n rr_min_depth 20\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        else if( mode=="vcm" ) r<<"vcm_pel_rasterizer\n{\n samples "<<spp<<"\n max_eye_depth 8\n max_light_depth 8\n merge_radius 0.0\n vc_enabled true\n vm_enabled true\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        else if( mode=="ptplain" ) r<<"pathtracing_pel_rasterizer\n{\n samples "<<spp<<"\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        else if( mode=="vcm16" ) r<<"vcm_pel_rasterizer\n{\n samples "<<spp<<"\n max_eye_depth 16\n max_light_depth 16\n merge_radius 0.0\n vc_enabled true\n vm_enabled true\n pixel_filter box\n oidn_denoise FALSE\n}\n";
        else {
            r<<"pathtracing_pel_rasterizer\n{\n samples "<<spp<<"\n rr_min_depth 20\n pixel_filter box\n oidn_denoise FALSE\n sms_enabled TRUE\n sms_target_bounces "<<tb<<"\n";
            if( mode=="extended" ) r<<" sms_extended TRUE\n sms_multi_trials 1\n";
            else if( mode=="extd" ) r<<" sms_extended TRUE\n sms_multi_trials 1\n max_diffuse_bounce 0\n";
            else r<<" sms_seeding "<<mode<<"\n sms_biased TRUE\n sms_multi_trials 1\n";
            r<<"}\n";
        }
        return r.str();
    }
    void Slab( const std::string& lights, const std::string& modesFilter, unsigned n, unsigned extSpp )
    {
        struct Light { const char* name; const char* text; std::vector<const char*> modes; const char* ext; const char* uni; double tol; };
        // tol: documented relative floor on |extended/reference - 1| (spot and
        // area read within noise). Omni: the +1.0..1.2 % DL-455 excess was
        // estimator A's rediscovery identity (fixed; slab-closed gates it);
        // it now reads -0.5 % +/- 0.14 % against depth-8 VCM (n 288), the
        // heavy-tailed 4+-vertex corner/TRRT chains reading about half
        // their closed form (DL-465).
        const Light cases[]={
            {"spot",kSpot,{"uniform","snell","extended","bdpt8","vcm"},"extended","uniform",0.005},
            {"omni",kOmni,{"uniform:0","extended:0","bdpt16","vcm"},"extended:0","uniform:0",0.01},
            {"area",kArea,{"uniform","extended","bdpt8","vcm"},"extended","uniform",0.01} };
        for( const Light& L : cases ) {
            if( lights.find(L.name)==std::string::npos ) continue;
            std::map<std::string,Series> m;
            std::vector<std::string> modeList;
            if( modesFilter.empty() ) for( const char* md : L.modes ) modeList.push_back(md);
            else { std::stringstream ms(modesFilter); std::string tok; while( std::getline(ms,tok,',') ) modeList.push_back(tok); }
            for( const std::string& mode : modeList ) {
                const unsigned spp = (mode.rfind("extended",0)==0 || mode.rfind("extd",0)==0) ? extSpp : 1024;
                for( unsigned t=0;t<n;++t ) {
                    g_renderIndex=t;
                    const auto r=Render(SlabScene(L.text,SlabRaster(mode,spp)),"slab");
                    Check(r.ok&&r.mean>0,"slab render lit");
                    m[mode].v.push_back(r.mean);
                    if( std::getenv("SMS_ROWS_VERBOSE") ) std::cout<<"SLABRUN "<<L.name<<" "<<mode<<" "<<t<<" "<<std::setprecision(9)<<r.mean<<std::endl;
                }
                std::cout<<"SLAB "<<L.name<<" mode="<<mode<<" mean="<<m[mode].mean()<<" +/- "<<m[mode].se()<<std::endl;
            }
            if( m.find("vcm")==m.end() ) continue;
            for( const auto& kv : m ) {
                const Series& a=kv.second, &b=m["vcm"];
                const double r1=a.mean()/b.mean();
                std::cout<<"SLAB RESULT "<<L.name<<" "<<kv.first<<"/VCM = "<<r1<<" +/- "<<r1*std::hypot(a.se()/a.mean(),b.se()/b.mean())<<std::endl;
            }
            const Series& e=m[L.ext]; const Series& v=m["vcm"];
            if( e.v.empty() ) continue;
            Check(std::fabs(e.mean()/v.mean()-1) <= 3*std::hypot(e.se()/e.mean(),v.se()/v.mean())+L.tol,
                std::string("slab ")+L.name+": extended = VCM within 3 combined se + "+std::to_string(L.tol));
            if( m.find(L.uni)!=m.end() ) Check(m[L.uni].mean()/v.mean()<0.97,
                std::string("slab ")+L.name+": legacy uniform seeding reads >3 % low (control: the DL-421 defect legacy mode retains)");
        }
    }

    // DL-455 closed form: the omni slab's DIRECT caustic per chain length
    // (extended, max_diffuse_bounce 0, sms_target_bounces k) against an
    // independent unfolded-image quadrature of the same box -- each side
    // reflection a mirror image of the floor point, unpolarized dielectric
    // Fresnel per interface (TIR = 1), entry/exit inside the faces, image
    // mean of (I/pi) E over the visible floor square [-2,2]^2 (1600^2
    // midpoint grid, 4e5-node angle table; resolution-converged to 3e-4
    // relative). Normalization pinned by the no-slab direct render,
    // 40/pi * mean(D/(r^2+D^2)^1.5) = 0.631818 (PT reads 0.6318182).
    //   k = 2 (T-T)          0.6300666
    //   k = 3 (T-R-T, sides) 0.0779853
    //   k = 4 (corner, TRRT) 0.0042208 -- not gated: heavy-tailed (skew
    //         4.3 at 64 spp), reads ~half; a residual recorded on DL-455.
    // Measured (n 48-512 salted renders, 64 spp): k = 2 1.0012 +/- 0.0009,
    // k = 3 1.0011 +/- 0.0071. Pre-fix the k = 3 row read +7 % (estimator A's resolution-limited
    // rediscovery identity rejected ~0.4 % of genuine rediscoveries).
    void SlabClosedForm( unsigned n, unsigned spp )
    {
        // The k = 3 row is ~2x noisier per render than k = 2 relative to its
        // mean, so it runs 3n renders at 2x spp (its pre-fix +7 % then sits
        // outside 3 se + 1 %).
        const struct { const char* mode; double truth; double floor; unsigned nScale, sppScale; } rows[]={
            {"extd:2",0.6300666,0.003,1,1},{"extd:3",0.0779853,0.01,3,2} };
        for( const auto& row : rows ) {
            Series s;
            for( unsigned t=0;t<n*row.nScale;++t ) {
                g_renderIndex=t;
                const auto r=Render(SlabScene(kOmni,SlabRaster(row.mode,spp*row.sppScale)),"slab_closed");
                Check(r.ok&&r.mean>0,"slab closed-form render lit");
                s.v.push_back(r.mean);
            }
            const double ratio=s.mean()/row.truth, se=s.se()/row.truth;
            std::cout<<"SLAB CLOSED "<<row.mode<<" mean="<<s.mean()<<" +/- "<<s.se()<<" closed form="<<row.truth
                <<" ratio="<<ratio<<" +/- "<<se<<std::endl;
            Check(std::fabs(ratio-1)<=3*se+row.floor,std::string("DL-455 omni slab direct ")+row.mode+" = closed form within 3 se + floor");
        }
    }

    double Sf11( double nm )
    {
        const double l2=(nm/1000)*(nm/1000);
        const double B[3]={1.73759695,0.313747346,1.898781010}, C[3]={0.013188707,0.0623068142,155.23629};
        double n2=1; for( int i=0;i<3;++i ) n2+=B[i]*l2/(l2-C[i]);
        return std::sqrt(n2);
    }
    Series SpectralCentroid( const char* mode, bool hwss, double nm, unsigned n, bool constantIndex=false, unsigned spp=128 )
    {
        Series c;
        for( unsigned t=0;t<n;++t ) {
            g_renderIndex=t;
            std::string scene=PaneScene(true,Sms(true,hwss,nm,mode,spp));
            if( constantIndex ) scene.replace(scene.find("ior sf11"),8,"ior "+std::to_string(Sf11(nm)));
            const auto r=Render(scene,"dispersion");
            Check(r.ok&&Centroids(r)[3]>0,std::string("spectral ")+mode+" render lit");
            c.v.push_back(Centroids(r)[4]);
        }
        return c;
    }
    Series Diff( const Series& a, const Series& b )
    {
        Series d; for( size_t i=0;i<a.v.size();++i ) d.v.push_back(a.v[i]-b.v[i]); return d;
    }

    void Dispersion()
    {
        // DL-438: RGB, SF11 sampled at {611,549,465} nm per channel.  Truth is
        // the wavelength-isolated spectral snell centroid at each channel's
        // wavelength (DL-353's material-side fix makes that path exact);
        // BDPT is not a usable reference on this single-interface fixture
        // (its centroid sits far from every SMS mode and shows no dispersion).
        const unsigned n=6;
        const Series t611=SpectralCentroid("snell",false,611,n,false,256), t549=SpectralCentroid("snell",false,549,n,false,256),
                     t465=SpectralCentroid("snell",false,465,n,false,256);
        Series truthBR=Diff(t465,t611), truthGR=Diff(t549,t611);
        std::cout<<"DL-438 truth (spectral snell) blue-red="<<truthBR.mean()<<" +/- "<<truthBR.se()
            <<" green-red="<<truthGR.mean()<<" +/- "<<truthGR.se()<<std::endl;
        for( const char* mode : {"snell","uniform","extended"} ) {
            Series br, gr;
            for( unsigned t=0;t<n;++t ) {
                g_renderIndex=t;
                const auto r=Render(PaneScene(true,Sms(false,false,0,mode,std::string(mode)=="extended"?512:256)),"dl438");
                const auto c=Centroids(r);
                Check(r.ok&&c[3]>0,std::string("DL-438 ")+mode+" render lit");
                br.v.push_back(c[2]-c[0]); gr.v.push_back(c[1]-c[0]);
            }
            std::cout<<"DL-438 RGB mode="<<mode<<" blue-red="<<br.mean()<<" +/- "<<br.se()<<" green-red="<<gr.mean()<<" +/- "<<gr.se()<<std::endl;
            Report(std::string("DL-438 ")+mode+" blue-red vs truth",br,truthBR);
            Report(std::string("DL-438 ")+mode+" green-red vs truth",gr,truthGR);
            if( std::string(mode)=="extended" ) {
                Check(Agree3(br,truthBR,2e-3),"DL-438 extended RGB blue-red centroid = wavelength-isolated truth");
                Check(Agree3(gr,truthGR,2e-3),"DL-438 extended RGB green-red centroid = wavelength-isolated truth");
            } else Check(!Agree3(br,truthBR,2e-3),std::string("DL-438 legacy ")+mode+" collapses channels onto one index (control)");
        }
        // DL-353: spectral uniform dispersion, blue(450)-red(650) centroid.
        for( bool hwss : {false,true} ) {
            std::map<std::string,Series> shift;
            for( const char* mode : {"snell","uniform","extended"} ) {
                shift[mode]=Diff(SpectralCentroid(mode,hwss,450,4),SpectralCentroid(mode,hwss,650,4));
                std::cout<<"DL-353 spectral hwss="<<hwss<<" mode="<<mode<<" blue-red="<<shift[mode].mean()<<" +/- "<<shift[mode].se()<<std::endl;
            }
            Report("DL-353 extended vs snell reference",shift["extended"],shift["snell"]);
            Report("DL-353 legacy uniform vs snell reference",shift["uniform"],shift["snell"]);
            Check(Agree3(shift["extended"],shift["snell"],2e-3),"DL-353 extended spectral dispersion = snell reference");
            Check(std::fabs(shift["extended"].mean())>0.02,"DL-353 extended N-SF11 caustic moves with wavelength");
        }
    }
}

int main( int argc, char** argv )
{
    ConfigureTestWorker();
    std::string section="all", lights="spot,omni,area", modes;
    unsigned n=0, extSpp=0;
    for( int i=1;i<argc;++i ) {
        const std::string a=argv[i];
        if( a=="--section" && i+1<argc ) section=argv[++i];
        else if( a=="--lights" && i+1<argc ) lights=argv[++i];
        else if( a=="--modes" && i+1<argc ) modes=argv[++i];
        else if( a=="--n" && i+1<argc ) n=unsigned(std::atoi(argv[++i]));
        else if( a=="--no-slab" ) g_noSlab=true;
        else if( a=="--ball-y" && i+1<argc ) g_ballY=argv[++i];
        else if( a=="--seed" && i+1<argc ) g_seedBase=unsigned(std::atoi(argv[++i]));
        else if( a=="--extspp" && i+1<argc ) extSpp=unsigned(std::atoi(argv[++i]));
    }
    const bool all=section=="all";
    if( all || section=="dispersion" ) Dispersion();
    if( all || section=="slab" ) Slab(lights,modes,n?n:4,extSpp?extSpp:512);
    if( all || section=="ball" ) Ball(modes,n?n:8,extSpp?extSpp:256);
    if( all || section=="slab-closed" ) SlabClosedForm(n?n:8,extSpp?extSpp:256);
    if( section=="nested" ) Nested();
    if( section=="nested-hwss" ) Nested(true);
    if( section=="nested-hwss-const" ) Nested(true,true);
    std::cout<<passCount<<" passed, "<<failCount<<" failed"<<std::endl;
    return failCount?1:0;
}
