// DL-382: wider certificates must not change legacy consumer records.
#include "SMSDomainReplayFixture.h"
#include "../src/Library/Geometry/TriangleMeshGeometryIndexed.h"
#include "../src/Library/Utilities/IORStackSeeding.h"
#include "../src/Library/Geometry/HairGeometry.h"
#include "../src/Library/Objects/Object.h"
int main() {
    const std::string fixture=ReadScene("tests/fixtures/dl382/holedglass0.RISEscene");
    Check(!fixture.empty(),"holed sphere fixture exists");
    const auto begin=fixture.find("indexedmesh_geometry");
    const auto end=fixture.find("standard_object",begin);
    const std::string geometry=fixture.substr(begin,end-begin);
    const std::string materials=Materials(false)+
        "translucent_material\n{\n name trans\n ref white\n tau white\n ext 0\n N 10\n scattering 0\n}\n"
        "composite_material\n{\n name comp\n top glass\n bottom inner\n thickness 0\n extinction 0\n}\n"
        "composite_material\n{\n name top_only\n top glass\n bottom none\n thickness 0\n extinction 0\n}\n"
        "composite_material\n{\n name bottom_only\n top none\n bottom glass\n thickness 0\n extinction 0\n}\n"
        "lambertian_luminaire_material\n{\n name wrapped\n material top_only\n exitance white\n scale 0\n}\n"
        "phong_luminaire_material\n{\n name nested\n material wrapped\n exitance white\n scale 0\n N 10\n}\n"
        "lambertian_luminaire_material\n{\n name wrapped_glass\n material glass\n exitance white\n scale 0\n}\n"
        "phong_luminaire_material\n{\n name nested_glass\n material wrapped_glass\n exitance white\n scale 0\n N 10\n}\n";
    for(const char* material : {"glass","trans","comp","top_only","bottom_only","wrapped","nested","wrapped_glass","nested_glass"}) {
        LoadedScene loaded(materials+geometry+"standard_object\n{\n name caster\n geometry gs\n material "+material+"\n}\n");
        const auto* object=loaded.Object("caster");
        Check(object!=nullptr,"consumer object exists");
        if(!object) continue;
        const auto* mesh=dynamic_cast<const TriangleMeshGeometryIndexed*>(object->GetGeometry());
        Check(mesh && mesh->IsProvablyOpenSheet(),"holed geometry passes face-rule certificate");
        for(int side : {-1,1}) {
            const auto hit=Hit(*object,Point3(0.1,1.1,side*3),Vector3(0,0,-side));
            Check(hit.geometric.bHit,"consumer ray hits cracked solid");
            Check(hit.geometric.bProvablyNoInterior==(std::string(material)=="glass" || std::string(material)=="wrapped_glass" || std::string(material)=="nested_glass"),
                "wider proof admitted only for clear refraction");
        }
    }
    {
        LoadedScene loaded(Materials(false)+
            "sphere_geometry\n{\n name solid\n radius 2\n}\n"
            "standard_object\n{\n name enclosing\n geometry solid\n material glass\n}\n");
        const auto* enclosing=loaded.Object("enclosing");
        std::vector<HairGeometry::StrandDesc> strands(1);
        strands[0].controlPoints={Point3(-1,0,0.5),Point3(1,0,0.5)};
        strands[0].rootWidth=strands[0].tipWidth=0.2;
        strands[0].rootUV=Point2(0,0);
        auto* hair=new HairGeometry(strands);
        auto* object=new Object(hair);
        safe_release(hair);
        object->FinalizeTransformations();
        loaded.job->GetObjects()->AddItem(object,"strand");
        const auto hit=Hit(*object,Point3(0,0,0),Vector3(0,0,1));
        Check(hit.geometric.bHit && !hit.geometric.HasTrueGeomSide(),"probe first encounters a ray-derived hair hit");
        Check(IORStackSeeding::OpenSheetEnclosesCrossing(enclosing,nullptr,&loaded.Scene(),Point3(0,0,0),Vector3(0,0,1)),
            "hair is skipped before the real enclosing exit");
        safe_release(object);
    }
    // DL-616: certified open Bezier/bilinear patches admit bProvablyNoInterior only for clear refraction.
    {
        const auto bezierPath=TestTempPath("dl616_scope.bezier");
        {
            std::ofstream out(bezierPath);
            out<<"1\n";
            for(int j=0;j<4;++j) for(int i=0;i<4;++i) out<<(i-1.5)<<' '<<(j-1.5)<<" "<<(0.1*i*j)<<"\n";
        }
        const auto bilinearPath=TestTempPath("dl616_scope.bilinear");
        {
            std::ofstream out(bilinearPath);
            out<<"1\n-1 -1 0\n1 -1 0\n-1 1 0\n1 1 0.2\n";
        }
        for(const char* material : {"glass","trans","comp","top_only","wrapped_glass"}) {
            const bool admit=(std::string(material)=="glass" || std::string(material)=="wrapped_glass");
            LoadedScene loaded(materials+
                "bezierpatch_geometry\n{\n name bez_g\n file "+bezierPath+"\n}\n"
                "bilinearpatch_geometry\n{\n name bil_g\n file "+bilinearPath+"\n}\n"
                "standard_object\n{\n name bez_o\n geometry bez_g\n material "+material+"\n}\n"
                "standard_object\n{\n name bil_o\n geometry bil_g\n material "+material+"\n}\n");
            const auto* bezObj=loaded.Object("bez_o");
            const auto* bilObj=loaded.Object("bil_o");
            Check(bezObj && bilObj,"DL-616 patch objects exist");
            if(bezObj && bilObj) {
                const auto hitBez=Hit(*bezObj,Point3(0,0,2),Vector3(0,0,-1));
                const auto hitBil=Hit(*bilObj,Point3(0,0,2),Vector3(0,0,-1));
                Check(hitBez.geometric.bHit && hitBez.geometric.bProvablyNoInterior==admit,
                    "DL-616: Bezier open-sheet proof admitted only for clear refraction");
                Check(hitBil.geometric.bHit && hitBil.geometric.bProvablyNoInterior==admit,
                    "DL-616: Bilinear open-sheet proof admitted only for clear refraction");
            }
        }
        std::remove(bezierPath.c_str());
        std::remove(bilinearPath.c_str());
    }
    // DL-617: certified open-sheet CSG (hole subtraction, solid-trimmed intersection, disjoint union)
    // preserves zero-thickness sheet semantics and scopes bProvablyNoInterior to clear refraction.
    for(const char* material : {"glass","trans","top_only","wrapped_glass"}) {
        const bool admit=(std::string(material)=="glass" || std::string(material)=="wrapped_glass");
        LoadedScene loaded(materials+
            "clippedplane_geometry\n{\n name sheet_g\n pta -2 -2 0\n ptb 2 -2 0\n ptc 2 2 0\n ptd -2 2 0\n}\n"
            "sphere_geometry\n{\n name hole_g\n radius 0.5\n}\n"
            "standard_object\n{\n name s1\n geometry sheet_g\n material "+material+"\n}\n"
            "standard_object\n{\n name h1\n geometry hole_g\n material "+material+"\n}\n"
            "csg_object\n{\n name holed_sheet\n obja s1\n objb h1\n operation subtraction\n material "+material+"\n}\n"
            "standard_object\n{\n name s2\n geometry sheet_g\n material "+material+"\n}\n"
            "standard_object\n{\n name h2\n geometry hole_g\n material "+material+"\n}\n"
            "csg_object\n{\n name trimmed_sheet\n obja s2\n objb h2\n operation intersection\n material "+material+"\n}\n");
        const auto* holed=loaded.Object("holed_sheet");
        const auto* trimmed=loaded.Object("trimmed_sheet");
        Check(holed && trimmed,"DL-617 CSG objects exist");
        if(holed && trimmed) {
            RayIntersection missCenter(Ray(Point3(0,0,2),Vector3(0,0,-1)),nullRasterizerState);
            holed->IntersectRay(missCenter,RISE_INFINITY,true,true,false);
            Check(!missCenter.geometric.bHit,"DL-617: ray through subtracted hole in open sheet misses cleanly");
            const auto hitOuter=Hit(*holed,Point3(1.0,0,2),Vector3(0,0,-1));
            Check(hitOuter.geometric.bHit && hitOuter.geometric.range2==RISE_INFINITY &&
                hitOuter.geometric.bProvablyNoInterior==admit,
                "DL-617: ray hitting outer part of subtracted open sheet preserves zero-thickness sheet proof");
            const auto hitCenter=Hit(*trimmed,Point3(0,0,2),Vector3(0,0,-1));
            Check(hitCenter.geometric.bHit && hitCenter.geometric.range2==RISE_INFINITY &&
                hitCenter.geometric.bProvablyNoInterior==admit,
                "DL-617: ray hitting solid-trimmed open sheet preserves zero-thickness sheet proof");
            RayIntersection missOuter(Ray(Point3(1.0,0,2),Vector3(0,0,-1)),nullRasterizerState);
            trimmed->IntersectRay(missOuter,RISE_INFINITY,true,true,false);
            Check(!missOuter.geometric.bHit,"DL-617: ray outside trimming solid misses trimmed open sheet");
        }
    }
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
