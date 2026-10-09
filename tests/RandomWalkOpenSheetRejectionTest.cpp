// DL-409: a random walk needs a volume. Reuse the exact F3 card fixture.
#define main ExistingSSSExteriorMain
#include "SSSExteriorIndexInvarianceTest.cpp"
#undef main

static bool LoadBody(const std::string& scene)
{
    const std::string path=WriteScene(scene,"dl409");
    IJobPriv* job=nullptr;
    bool ok=RISE_CreateJobPriv(&job) && job && job->LoadAsciiSceneViaCst(path.c_str());
    safe_release(job);
    std::remove(path.c_str());
    return ok;
}
int main()
{
    const std::string sheet=BuildInsetScene(2,64);
    Check(!LoadBody(sheet),"DL-409 F3 coplanar card is refused");
    std::string lone=sheet;
    const auto start=lone.find("standard_object\n{\n\tname outer\n");
    lone.erase(start,lone.find("}\n\n",start)+3-start);
    Check(!LoadBody(lone),"DL-409 lone open card is refused");
    std::string normal=lone;
    const auto mat=normal.find("material block"); normal.replace(mat,14,"material ordinary");
    const auto declaration=normal.find("standard_object");
    normal.insert(declaration,"lambertian_material\n{\n name ordinary\n reflectance white\n}\n");
    const std::string path=WriteScene(normal,"dl409_retarget");
    IJobPriv* job=nullptr;
    const bool loaded=RISE_CreateJobPriv(&job) && job && job->LoadAsciiSceneViaCst(path.c_str());
    Check(loaded,"DL-409 ordinary sheet material is accepted");
    if(loaded) {
        const IMaterial* old=job->GetScene()->GetObjects()->GetItem("inset")->GetMaterial();
        Check(!job->SetObjectMaterial("inset","block"),"DL-409 runtime binding refuses the open card");
        Check(job->GetScene()->GetObjects()->GetItem("inset")->GetMaterial()==old,
            "DL-409 refused runtime binding retains the old material");
    }
    safe_release(job); std::remove(path.c_str());
    Check(LoadBody(BuildInsetScene(0,64)),"DL-409 closed mesh is accepted");
    Check(LoadBody(BuildInsetScene(1,64)),"DL-409 authored slab is accepted");
    // A geometric slab with any positive thickness has an interior: never
    // replace exact planarity by an epsilon or the watertightness flag.
    std::string thin=BuildInsetScene(1,64);
    const auto h=thin.find("height 0.5"); thin.replace(h,10,"height 0.000001");
    Check(LoadBody(thin),"DL-409 positive thin slab is accepted");
    std::cout << "Passed: " << passCount << "\nFailed: " << failCount << std::endl;
    return failCount ? 1 : 0;
}
