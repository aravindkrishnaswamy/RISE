// r271 section 25 (read-only): a retained RISE checkpoint's full state for the FDS transplant (make_fds_xp.py), float64
// in xp_state.py's layout T[z][y][x], U[z][y][x+1], V[z][y+1][x], W[z+1][y][x], then the methane mass fraction Y[z][y][x]
// (constituent CH4 over the gas density; round-off negatives clamped to 0). argv: checkpoint, out file.
#define main FireSequenceSuiteMain
#include R271_HARNESS_SOURCE
#undef main
int main(int argc,char** argv){
 if(argc!=3)return 1;MethaneRunCheckpoint c;std::string error;
 if(!LoadMethaneRunCheckpoint(argv[1],c,error)){std::cerr<<error<<'\n';return 2;}
 std::vector<double> out;for(const auto& s:c.states)out.push_back(s.temperatureK);
 for(int k=0;k<3;++k)out.insert(out.end(),c.velocity.component[k].begin(),c.velocity.component[k].end());
 for(const auto& s:c.states)out.push_back(std::max(0.0,s.constituent[MethaneCH4])/s.GasDensity());
 FILE* o=fopen(argv[2],"wb");if(!o)return 3;fwrite(out.data(),8,out.size(),o);fclose(o);
 std::cerr<<std::setprecision(9)<<"time "<<c.simulationTimeS<<" values "<<out.size()<<'\n';return 0;
}
