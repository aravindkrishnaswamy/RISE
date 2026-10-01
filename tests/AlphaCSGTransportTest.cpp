#define Render OriginalRender
#include "AlphaTransportFixture.h"
#undef Render


inline double Render( const std::string& sceneText, const char* tag )
{
	char path[512];
	std::snprintf( path, sizeof(path), "/tmp/alpha_intersection_%s_%d.RISEscene",
		tag, static_cast<int>( ::getpid() ) );
	{
		std::ofstream ofs( path );
		if( !ofs.is_open() ) return -1.0;
		ofs << sceneText;
	}

	std::srand( g_seedBase + g_renderIndex );
	g_renderIndex++;

	double result = -1.0;
	IJobPriv* pJob = nullptr;
	if( RISE_CreateJobPriv( &pJob ) && pJob )
	{
		if( pJob->LoadAsciiSceneViaCst( path ) )
		{
			if(!pJob->SetObjectInteriorMedium("medobj","med")){pJob->release();return -2;}
            pJob->RemoveRasterizerOutputs();
			CapturingRasterizerOutput* pCap = new CapturingRasterizerOutput();
			GlobalLog()->PrintNew( pCap, __FILE__, __LINE__, "test capture output" );
			pJob->GetRasterizer()->AddRasterizerOutput( pCap );
			if( pJob->Rasterize() ) {
				result = MeanLuminance( *pCap );
			}
			safe_release( pCap );
		}
		safe_release( pJob );
	}
	std::remove( path );
	return result;
}


static void Replace(std::string& s,const std::string& a,const std::string& b){auto at=s.find(a);if(at==std::string::npos)std::exit(2);s.replace(at,a.size(),b);}
int main(){
 for(unsigned mode=0;mode<3;++mode)for(unsigned w=0;w<3;++w){
 auto r=mode==0?RastPT(1024):mode==1?RastBDPT(1024):RastVCM(1024);const std::string family=mode==0?"pathtracing":mode==1?"bdpt":"vcm";
 if(mode){Replace(r,"max_light_depth 8","max_light_depth 1");Replace(r,"max_eye_depth 8","max_eye_depth 2");}
 if(w){Replace(r,family+"_pel_rasterizer",family+"_spectral_rasterizer");Replace(r,"{\n","{\n hwss "+std::string(w==2?"true":"false")+"\n");}
 auto s=ReceiverScene(kOmni,false,0,kTight);Replace(s,"color 0.01 0.01 0.01","color 0 0 0");
 s+="homogeneous_medium\n{\n name med\n absorption 1 1 1\n scattering 0 0 0\n phase hg 0\n}\nsphere_geometry\n{\n name medgeo\n radius 0.5\n}\nlambertian_material\n{\n name boundarymat\n reflectance pnt_recv\n alpha_mode mask\n alpha_coverage 0\n}\nstandard_object\n{\n name operandA\n geometry medgeo\n material boundarymat\n}\nstandard_object\n{\n name operandB\n geometry medgeo\n material boundarymat\n position 100 0 0\n}\ncsg_object\n{\n name medobj\n obja operandA\n objb operandB\n operation union\n allow_transformed_operands TRUE\n position 0 2 0\n casts_shadows FALSE\n}\n";
 auto clear=s;Replace(clear,"absorption 1 1 1","absorption 0 0 0");auto explicitRoot=s;Replace(explicitRoot,"name medobj\n","name medobj\n material boundarymat\n");
 const double a=Render(Assemble(r,clear),"CSG_clear"),b=Render(Assemble(r,s),"CSG_inherited"),c=Render(Assemble(r,explicitRoot),"CSG_root_override");
 std::cout<<"CSG mode="<<mode<<" w="<<w<<" clear="<<a<<" inherited="<<b<<" rootAlpha="<<c<<" inheritedRatio="<<b/a<<" rootRatio="<<c/a<<std::endl;
 Check(std::isfinite(a)&&a>.01,"valid clear control");Check(std::fabs(b/a-1)<.025,"inherited MASK0 rejects all medium attenuation");Check(std::fabs(c/a-1)<.025,"explicit root MASK0 rejects medium attenuation");
 }
 std::cout<<passCount<<" passed / "<<failCount<<" failed"<<std::endl;return failCount?1:0;
}
