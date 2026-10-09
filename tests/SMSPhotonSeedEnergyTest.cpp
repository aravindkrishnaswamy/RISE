// DL-331: independent SMS-off PT energy reference, four salted replicas.
#define main GradedIndexFixtureMain
#include "GradedIndexInteriorFactorTest.cpp"
#undef main
int main() {
	auto build = [&]( int mode, bool sms ) {
		std::string s = ReadFile( kSeededScene );
		if( mode == 0 ) s = ReplaceSpan( s, "MEDIUM", UniformBox( 1.4 ) );
		if( mode == 2 ) s = ReplaceSpan( s, "MEDIUM", "" );
		s = ReplaceOnce( s, "\tpta -28 -28 1.0\n\tptb -28 28 1.0\n\tptc 28 28 1.0\n\tptd 28 -28 1.0\n",
			"\tpta 0.5 -0.1 1.0\n\tptb 0.5 0.1 1.0\n\tptc 0.7 0.1 1.0\n\tptd 0.7 -0.1 1.0\n" );
		s = ReplaceOnce( s, "\tscale 1.0\n", "\tscale 40.0\n" );
		s = ReplaceOnce( s, kEmitterObject, kEmitterObject +
			"\ndielectric_material\n{\n\tname mat_ball\n\tior 1.5\n\ttau 1.0\n\tscattering 1000000\n}\n\n"
			"sphere_geometry\n{\n\tname geo_ball\n\tradius 0.15\n}\n\n"
			"standard_object\n{\n\tname ball\n\tgeometry geo_ball\n\tmaterial mat_ball\n\tposition 0.3 0 0.55\n}\n" );
		return ReplaceSpan( s, "RASTERIZER", PTRasterizerOpts( 128, sms ?
			"\tsms_enabled TRUE\n\tsms_max_iterations 30\n\tsms_threshold 1e-4\n\tsms_max_chain_depth 10\n\tsms_biased TRUE\n\tsms_multi_trials 4\n\tsms_photon_count 200000\n" : "" ) );
	};
 g_saltRenders=true;
 for(int mode: {0,2}) {
  std::vector<double> a,b;
  for(int i=0;i<4;++i) {
   Stat pt=RenderStat(build(mode,false),"331_pt");
   Stat sms=RenderStat(build(mode,true),"331_photon");
   if(!pt.ok || !sms.ok) return 1;
   a.push_back(pt.mean); b.push_back(sms.mean);
  }
  auto stats=[](const std::vector<double>& v) { double m=0,ss=0; for(double x:v)m+=x; m/=v.size(); for(double x:v)ss+=(x-m)*(x-m); return std::make_pair(m,std::sqrt(ss/(v.size()-1)/v.size())); };
  auto x=stats(a),y=stats(b);
  double band=3*std::hypot(x.second,y.second);
  std::printf("DL331 mode=%d PT %.9g SE %.9g photon %.9g SE %.9g ratio %.7g combined3sigma %.9g within=%d\n",mode,x.first,x.second,y.first,y.second,y.first/x.first,band,std::abs(x.first-y.first)<=band);
 }
 return 0;
}
