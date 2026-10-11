// DL-531: a shading-bound painter must join the time-indexed path exactly.
#define main MotionOriginalMain
#include "TimeIndexedMotionBlurTest.cpp"
#undef main
#include "../src/Library/Painters/UniformColorPainter.h"
#include "../src/Library/Interfaces/IPainterManager.h"
int main(int argc,char** argv) {
    if(argc>1 && std::strcmp(argv[1],"--child")==0) return ChildMain(argc,argv);
    if(argc==2 && std::strcmp(argv[1],"--eligibility-only")==0) {
        const std::string scene="RISE ASCII SCENE 7\nfilm\n{\n\twidth 16\n\theight 16\n}\npinhole_camera\n{\n\tlocation 0 0 5\n\tlookat 0 0 0\n\texposure 1\n}\nuniformcolor_painter\n{\n\tname p\n\tcolor 0.5 0.5 0.5\n}\nlambertian_material\n{\n\tname m\n\treflectance p\n}\nsphere_geometry\n{\n\tname g\n\tradius 1\n}\nstandard_object\n{\n\tname o\n\tgeometry g\n\tmaterial m\n}\n"+Timeline("painter","p","risepel","0.2 0.1 0.05","0.8 0.4 0.2")+"standard_shader\n{\n\tname global\n\tshaderop DefaultPathTracing\n}\n"+Rasterizer("pt",1);
        const std::string path=Fmt("tests/painter_eligibility_%d.RISEscene",getpid());
        { std::ofstream f(path);f<<scene; }
        IJobPriv* job=nullptr;
        if(!RISE_CreateJobPriv(&job)||!job||!job->LoadAsciiSceneViaCst(path.c_str())) return 2;
        std::remove(path.c_str());std::string reason;
        const IObjectManager* objects=job->GetScene()->GetObjects();
        objects->BeginMotionSweep();
        objects->RecomposeAnimatedHierarchy();
        objects->AccumulateMotionBounds();
        objects->EndMotionSweep();
        auto* frame=TimeIndexedFrame::TryCreate(*job->GetScene(),reason);
        Check(frame!=nullptr,"shading-only painter is eligible");
        std::cout<<"eligible="<<(frame!=nullptr)<<" reason="<<reason<<"\n";
        delete frame;
        auto* painter=dynamic_cast<UniformColorPainter*>(job->GetPainters()->GetItem("p"));
        if(!painter) return 2;
        const auto token=painter->Attach([]{});
        frame=TimeIndexedFrame::TryCreate(*job->GetScene(),reason);
        Check(!frame,"registered painter observer refuses cloning");
        delete frame;painter->Detach(token);safe_release(job);
        return failCount ? 1:0;
    }
    g_self=std::filesystem::absolute(argv[0]).string();
    for(const char* ras:{"pt","pts","bdpt","bdpts","vcm","vcms"}) for(unsigned i=0;i<4;++i) {
        const auto old=RunChild("shade",ras,1,false,53100+i,8);
        const auto ti=RunChild("shade",ras,1,true,53100+i,8);
        Check(old.ok && ti.ok,"shading painter renders");
        Check(ti.frames>0,"shading painter creates time-indexed frame");
        Check(old.hash==ti.hash,"one-thread painter pixels bit-identical");
    }
    const auto signal=RunChild("shade","pt",1,false,53100,16);
    const auto fixed=RunChild("shade_static","pt",1,false,53100,16);
    Check(signal.ok && fixed.ok && signal.hash!=fixed.hash,"bound painter timeline changes rendered pixels");
    const auto refused=RunChild("shade_unsafe","pt",1,true,53100,8);
    Check(refused.ok && refused.frames==0,"unverified geometry refuses painter cloning");
    std::cout << passCount << " passed, " << failCount << " failed\n";
    return failCount ? 1:0;
}
