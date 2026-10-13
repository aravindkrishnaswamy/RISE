// DL-382: wider certificates must not change legacy consumer records.
#define main DomainFixtureMain
#include "SMSDomainReplayTest.cpp"
#undef main
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
    std::cout<<passCount<<" passed, "<<failCount<<" failed\n";
    return failCount?1:0;
}
