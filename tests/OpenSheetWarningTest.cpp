// DL-382: warning coverage; no claim that a patch has empty interior.
#define main DomainFixtureMain
#include "SMSDomainReplayTest.cpp"
#undef main
#include "../src/Library/Interfaces/ILogPrinter.h"
#include "../src/Library/Utilities/Reference.h"
class CaptureWarnings : public ILogPrinter, public Reference {
public:
 unsigned count=0, negativeCount=0;
 void Print(const LogEvent& event) override {if(std::strstr(event.szMessage,"DL-382"))++count; if(std::strstr(event.szMessage,"negative open signed volume"))++negativeCount;}
 void Flush() override {}
};
int main() {
 auto* capture=new CaptureWarnings();GlobalLogPriv()->AddPrinter(capture);
 const auto path=TestTempPath("dl382_patch.bezier");
 {std::ofstream out(path);out<<"1\n";for(int j=0;j<4;++j)for(int i=0;i<4;++i)out<<i<<' '<<j<<" 0\n";}
 {
  LoadedScene loaded(Materials(false)+"bezierpatch_geometry\n{\n name shape\n file "+path+"\n}\nstandard_object\n{\n name patch\n geometry shape\n material glass\n}\n");
  Check(capture->count==1,"uncertified transmitting Bezier warns once");
  loaded.Scene().GetObjects()->PrepareForRendering();
  Check(capture->count==1,"repeated preparation does not spam warning");
 }
 unsigned before=capture->count;
 {LoadedScene loaded(Materials(false)+"sphere_geometry\n{\n name shape\n radius 1\n}\nstandard_object\n{\n name solid\n geometry shape\n material glass\n}\n");Check(capture->count==before,"certified analytical solid does not warn");}
 for(int inward=0;inward<2;++inward) {
  const std::string fixture="tests/fixtures/dl382/holedglass"+std::to_string(inward)+".RISEscene";
  const auto text=ReadScene(fixture.c_str());
  Check(!text.empty(),"holed glass fixture exists");
  const unsigned previous=capture->negativeCount;
  LoadedScene loaded(text);
  Check(capture->negativeCount==previous+unsigned(inward),inward?"inward holed glass warns":"outward holed glass does not warn");
  loaded.Scene().GetObjects()->PrepareForRendering();
  Check(capture->negativeCount==previous+unsigned(inward),"negative-volume warning does not repeat");
 }
 std::remove(path.c_str());GlobalLogPriv()->RemoveAllPrinters();capture->release();
 std::cout<<passCount<<" passed, "<<failCount<<" failed\n";return failCount?1:0;
}
