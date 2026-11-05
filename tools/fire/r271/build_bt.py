#!/usr/bin/env python3
"""Build the r271 small-scale response driver (bt_driver.cpp) against the committed production objects.

Only an external copy of the test harness is patched (r260 pattern, as build_tg.py); no production source or object is
rewritten:
  1. stop at the first accepted step reaching start + RISE_R271_WINDOW_S;
  2. on a fresh production start (acceptedSteps 0, not a resume), superpose in ambient air a temperature pattern of
     amplitude A (K) and wavelength n cells on a uniform offset A (the methane table starts at 300 K), times the band
     envelope f(z) (1 on 1.75-2.85 m, sin^2 ramps to 0 at 1.6 and
     3.0 m). Perturbed cells keep ambient composition at the new temperature and the thermodynamic pressure (densities
     scaled by 300/T, sensible energy from the mixture record, then the production binary32 canonicalisation).
       buoy: theta = A (1 + cos(k x') cos(k y') cos(k z')) f(z), x',y' from the domain centre, z' from 2.3 m; velocity zero.
       adv:  theta = A (1 + cos(k x')) f(z); and a uniform u = U f(z) on the x faces (divergence-free), momentum = face rho u.
  3. each loop head (after every accepted step) prints R271_BT t, the projections of the field onto the pattern over the
     core (z 1.9-2.7 m, x and y cells 10-58):
       buoy: Ab = <g(1-300/T), phi>/<phi,phi> at cell centres, Aw = <w, phi_w>/<phi_w,phi_w> on z faces;
       adv:  ac, as = projections of T-300 onto cos(k x') and sin(k x'), and the core mean u.
"""
from pathlib import Path
import subprocess,shlex,hashlib,json,sys
ROOT=Path(__file__).resolve().parents[3];OUT=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_small_scale/bt_build')
OUT.mkdir(parents=True,exist_ok=True);SRC=sys.argv[1] if len(sys.argv)>1 else 'bt_driver'  # or bt_ckpt (checkpoint reader, same harness copy)
h=(ROOT/'tests/FireSequenceTest.cpp').read_text()
old='\t\tconst char* solverPhase="initialization";';assert h.count(old)==1
bt=r'''
		const double r271EndTimeS=simulationTimeS+std::stod(std::getenv("RISE_R271_WINDOW_S"));
		const std::string r271Kind=std::getenv("RISE_R271_BT_KIND")?std::getenv("RISE_R271_BT_KIND"):"";
		const double r271Pi=3.14159265358979323846,r271H=shape.cellWidthM;
		const double r271K=2.0*r271Pi/(std::stod(std::getenv("RISE_R271_BT_N"))*r271H);
		const double r271X0=.5*shape.nx*r271H,r271Y0=.5*shape.ny*r271H,r271Z0=2.3;
		auto r271F=[&](const double z){if(z<=1.6||z>=3.0)return 0.0;if(z<1.75)return std::pow(std::sin(.5*r271Pi*(z-1.6)/.15),2);
			if(z>2.85)return std::pow(std::sin(.5*r271Pi*(3.0-z)/.15),2);return 1.0;};
		auto r271Phi=[&](const double X,const double Y,const double Z){return r271Kind=="buoy"?
			std::cos(r271K*(X-r271X0))*std::cos(r271K*(Y-r271Y0))*std::cos(r271K*(Z-r271Z0)):std::cos(r271K*(X-r271X0));};
		if(r271Kind!="buoy"&&r271Kind!="adv"){values.structuredError="r271_bt_kind";return values;}
		if(persistence.productionMetal&&!persistence.resume&&acceptedSteps==0u){
			const double A=std::stod(std::getenv("RISE_R271_BT_A")),U=r271Kind=="adv"?std::stod(std::getenv("RISE_R271_BT_U")):0.0;
			for(std::size_t z=0u;z<shape.nz;++z){const double Z=(z+.5)*r271H,f=r271F(Z);if(f==0.0)continue;
				for(std::size_t y=0u;y<shape.ny;++y)for(std::size_t x=0u;x<shape.nx;++x){
					MethaneCellState& cell=states[x+shape.nx*(y+shape.ny*z)];
					if(cell.temperatureK!=300.0){values.structuredError="r271_bt_not_ambient";return values;}
					const double T=300.0+A*f*(1.0+r271Phi((x+.5)*r271H,(y+.5)*r271H,Z));
					for(double& c:cell.constituent)c*=300.0/T;cell.rhoTotalZ*=300.0/T;cell.temperatureK=T;
					cell.producerPrecision=FireStateProducerPrecision::Binary64;
					if(!fuel.MixtureSensibleEnergyJPerM3(ThermochemicalDensities(cell),T,cell.sensibleEnergyJPerM3,&error)||
						!canonicalizeProductionBinary32State(cell)){values.structuredError="r271_bt_state:"+error;return values;}}}
			if(U!=0.0){const std::size_t ex=shape.nx+1u,ey=shape.ny;
				if(advanced.velocityMPerS.component[0].size()!=ex*ey*shape.nz||momentum.component[0].size()!=ex*ey*shape.nz){
					values.structuredError="r271_bt_mac_shape";return values;}
				for(std::size_t z=0u;z<shape.nz;++z)for(std::size_t y=0u;y<ey;++y)for(std::size_t x=0u;x<ex;++x){
					const std::size_t face=x+ex*(y+ey*z),below=x>0u?x-1u:0u,above=x<shape.nx?x:shape.nx-1u;
					const double vel=U*r271F((z+.5)*r271H);
					const double rho=.5*(states[below+shape.nx*(y+shape.ny*z)].GasDensity()+states[above+shape.nx*(y+shape.ny*z)].GasDensity());
					advanced.velocityMPerS.component[0][face]=vel;momentum.component[0][face]=rho*vel;}}
			std::fprintf(stderr,"R271_BT_INIT kind=%s A=%g U=%g wavelength_m=%.6f\n",r271Kind.c_str(),A,U,2.0*r271Pi/r271K);
		}
		auto r271Report=[&](){
			double pp=0,pb=0,ww=0,pw=0,c2=0,s2=0,pc=0,ps=0,us=0;std::size_t un=0;const double g=9.80665;
			for(std::size_t z=0u;z<shape.nz;++z){const double Z=(z+.5)*r271H,Zf=z*r271H;
				for(std::size_t y=10u;y<59u;++y)for(std::size_t x=10u;x<59u;++x){const double X=(x+.5)*r271H,Y=(y+.5)*r271H;
					const double T=states[x+shape.nx*(y+shape.ny*z)].temperatureK;
					if(Z>=1.9&&Z<=2.7){const double p=r271Phi(X,Y,Z);pp+=p*p;pb+=p*g*(1.0-300.0/T);
						const double c=std::cos(r271K*(X-r271X0)),s=std::sin(r271K*(X-r271X0));c2+=c*c;s2+=s*s;pc+=c*(T-300.0);ps+=s*(T-300.0);
						us+=.5*(advanced.velocityMPerS.component[0][x+(shape.nx+1u)*(y+shape.ny*z)]+
							advanced.velocityMPerS.component[0][x+1u+(shape.nx+1u)*(y+shape.ny*z)]);++un;}
					if(Zf>=1.9&&Zf<=2.7){const double p=r271Phi(X,Y,Zf);ww+=p*p;pw+=p*advanced.velocityMPerS.component[2][x+shape.nx*(y+shape.ny*z)];}}}
			std::fprintf(stderr,"R271_BT step=%llu t=%.9g Ab=%.9g Aw=%.9g ac=%.9g as=%.9g u=%.9g\n",
				static_cast<unsigned long long>(acceptedSteps),simulationTimeS,pb/pp,pw/ww,pc/c2,ps/s2,us/un);};
		if(persistence.productionMetal)r271Report();'''
h=h.replace(old,old+bt)
old='(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {';assert h.count(old)==1
h=h.replace(old,'(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&\n\t\t\tsimulationTimeS<r271EndTimeS) {\n\t\t\tif(persistence.productionMetal&&acceptedSteps>0u)r271Report();')
old='\t\tif(!BuildProductionTemporalWindowPlan(beginning,frameCadenceS,r246TopDouble,window))return 93;';assert h.count(old)==1
h=h.replace(old,'\t\tif(!beginning.succeeded)std::fprintf(stderr,"R271_BT_PROBE_ERROR %s\\n",beginning.structuredError.c_str());\n\t\tstd::fprintf(stderr,"R271_BT_PROBE ok=%d dims=%zu,%zu,%zu ft=%a mft=%a ss=%a\\n",beginning.succeeded?1:0,beginning.dimensions[0],beginning.dimensions[1],beginning.dimensions[2],beginning.flowThroughTimeS,beginning.matchedScheduleFlowThroughTimeS,beginning.statisticsStartS);\n'+old)
old='\t\tif(!launchCase.succeeded||launchCase.flowThroughTimeS!=beginning.flowThroughTimeS||';assert h.count(old)==1
h=h.replace(old,'\t\tstd::fprintf(stderr,"R271_BT_LAUNCH ok=%d err=%s\\n",launchCase.succeeded?1:0,launchCase.structuredError.c_str());\n'+old)
harness=OUT/'Harness.cpp';harness.write_text(h)
make=ROOT/'build/make/rise';cmds=subprocess.check_output(['make','-C',str(make),'-n','-B','../../../bin/tests/FireSequenceTest'],text=True).splitlines()
compile_=next(x for x in cmds if x.startswith('c++ ')and' -c 'in x and'tests/FireSequenceTest.cpp'in x)
link=next(x for x in cmds if x.startswith('c++ ')and' -o ../../../bin/tests/FireSequenceTest 'in x)
obj=OUT/(SRC+'.o');drv=OUT/SRC
compile_=compile_.replace('../../../tests/FireSequenceTest.cpp',str(ROOT/'tools/fire/r271'/(SRC+'.cpp'))).replace('-o ../../../tests/FireSequenceTest.o','-o '+str(obj))
compile_=compile_.replace(' -c ',f' -I{ROOT}/tests -DR271_HARNESS_SOURCE=\\"{harness}\\" -c ')
assert str(obj) in compile_
link=link.replace(' -o ../../../bin/tests/FireSequenceTest ',' -o '+str(drv)+' ').replace(' ../../../tests/FireSequenceTest.o ',' '+str(obj)+' ')
assert str(obj) in link
(OUT/f'build_{SRC}.sh').write_text('set -e\ncd '+shlex.quote(str(make))+'\n'+compile_+'\n'+link+'\n')
with (OUT/f'build_{SRC}.log').open('w') as log:subprocess.run(['sh',str(OUT/f'build_{SRC}.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(OUT/f'build_manifest_{SRC}.json').write_text(json.dumps({'production_sources_rewritten':False,'git_head':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
 'git_dirty':subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain','--','src','tests'],text=True).strip()!='',
 'harness_changes':['stop at start + RISE_R271_WINDOW_S','temperature pattern (and uniform u for adv) at t=0','per-step pattern projections'],
 'sha256':{str(p):sha(p) for p in [ROOT/'tools/fire/r271'/(SRC+'.cpp'),harness,drv]}},indent=2)+'\n')
print(drv)
