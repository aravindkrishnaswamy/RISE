// DL-214: import real RGBA glTF alpha into scalar material coverage.
#include <cmath>
#include <iostream>
#include "../src/Library/Job.h"
#include "../src/Library/Importers/GLTFSceneImporter.h"
#include "../src/Library/Interfaces/IMaterial.h"
#include "../src/Library/Interfaces/IMaterialManager.h"
using namespace RISE; using namespace RISE::Implementation;
static int pass=0,fail=0;
static void Check(bool b,const char* s){(b?pass:fail)++;if(!b)std::cout<<"FAIL "<<s<<'\n';}
int main(){
    Job* job=new Job();
    GLTFSceneImporter importer("scenes/Tests/Geometry/assets/AlphaBlendModeTest.glb");
    GLTFImportOptions options; options.namePrefix="alpha";
    Check(importer.IsValid() && importer.ImportScene(*job,options),"real Khronos alpha fixture imports");
    IMaterial* mats[6]={};
    for(int i=0;i<6;++i){mats[i]=job->GetMaterials()->GetItem(("alpha.mat."+std::to_string(i)).c_str());Check(mats[i]!=nullptr,"material registered");}
    if(mats[1]&&mats[2]&&mats[3]&&mats[4]&&mats[5]){
        Check(mats[1]->GetAlphaMode()==eAlphaBlend,"BLEND property installed");
        Check(mats[2]->GetAlphaMode()==eAlphaMask && mats[3]->GetAlphaMode()==eAlphaMask && mats[4]->GetAlphaMode()==eAlphaMask,"MASK properties installed");
        Check(mats[5]->GetAlphaMode()==eAlphaOpaque,"default OPAQUE ignores texture alpha");
        RasterizerState rast={0};RayIntersectionGeometric ri(Ray(Point3(0,0,1),Vector3(0,0,-1)),rast);
        bool correct=true,low=false,high=false,middle=false;
        for(int y=0;y<32;++y)for(int x=0;x<32;++x){
            ri.ptCoord=Point2((x+.5)/32.,(y+.5)/32.);
            const Scalar a=mats[1]->AlphaCoverage(ri);
            low|=a<.25;high|=a>.75;middle|=a>.25&&a<.75;
            correct &= mats[2]->AlphaCoverage(ri)==(a>=.25?1:0);
            correct &= mats[3]->AlphaCoverage(ri)==(a>=.75?1:0);
            correct &= mats[4]->AlphaCoverage(ri)==(a>=.5?1:0);
            correct &= mats[5]->AlphaCoverage(ri)==1;
        }
        Check(low&&middle&&high,"actual texture contains partial and endpoint coverage");
        Check(correct,"RGBA alpha stays linear and MASK uses explicit/default cutoffs");
    }
    job->release();std::cout<<pass<<" passed / "<<fail<<" failed\n";return fail?1:0;
}
