#!/usr/bin/env python3
"""Build the r271 Taylor-Green operator-damping driver (tg_driver.cpp) against the committed production objects.

Only an external copy of the test harness is patched (r260 pattern); no production source or object is rewritten:
  1. stop at the first accepted step reaching start + RISE_R271_WINDOW_S (as build_ab.py);
  2. on a fresh production start (not the capstone's geometry probe), superpose a 2D Taylor-Green array u=U f(z) sin(kx) cos(ky), v=-U f(z) cos(kx) sin(ky), w=0,
     k=m pi/L (zero normal flow on the side boundaries; exactly MAC divergence-free), f(z)=1 on 1.75-2.85 m with
     sin^2 ramps to 0 at 1.6 and 3.0 m; momentum = face gas density x velocity;
  3. optional Vreman coefficient override RISE_R271_CV in the projected-Heun owner request.
"""
from pathlib import Path
import subprocess,shlex,hashlib,json,sys
ROOT=Path(__file__).resolve().parents[3];OUT=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_compact_stencil')/(sys.argv[1] if len(sys.argv)>1 else 'tg_build')
OUT.mkdir(parents=True,exist_ok=True)
SRC=sys.argv[2] if len(sys.argv)>2 else 'tg_driver'  # or tg_mode (checkpoint reader, same harness copy)
h=(ROOT/'tests/FireSequenceTest.cpp').read_text()
old='\t\tconst char* solverPhase="initialization";';assert h.count(old)==1
tg=r'''
		const double r271EndTimeS=simulationTimeS+std::stod(std::getenv("RISE_R271_WINDOW_S"));
		if(const char* r271Modes=std::getenv("RISE_R271_TG_MODES");r271Modes&&persistence.productionMetal&&!persistence.resume&&acceptedSteps==0u){
			const double U=std::stod(std::getenv("RISE_R271_TG_U")),m=std::stod(r271Modes),h=shape.cellWidthM,Pi=3.14159265358979323846;
			const double kx=m*Pi/(shape.nx*h),ky=m*Pi/(shape.ny*h);
			auto f=[&](const double z){if(z<=1.6||z>=3.0)return 0.0;if(z<1.75)return std::pow(std::sin(.5*Pi*(z-1.6)/.15),2);
				if(z>2.85)return std::pow(std::sin(.5*Pi*(3.0-z)/.15),2);return 1.0;};
			for(unsigned int axis=0u;axis<2u;++axis){const std::size_t ex=shape.nx+(axis==0u),ey=shape.ny+(axis==1u);
				if(advanced.velocityMPerS.component[axis].size()!=ex*ey*shape.nz||momentum.component[axis].size()!=ex*ey*shape.nz){
					values.structuredError="r271_tg_mac_shape";return values;}
				for(std::size_t z=0u;z<shape.nz;++z)for(std::size_t y=0u;y<ey;++y)for(std::size_t x=0u;x<ex;++x){
					const std::size_t face=x+ex*(y+ey*z);const double X=(x+(axis==0u?0.:.5))*h,Y=(y+(axis==1u?0.:.5))*h,Z=(z+.5)*h;
					const double vel=axis==0u?U*f(Z)*std::sin(kx*X)*std::cos(ky*Y):-U*f(Z)*std::cos(kx*X)*std::sin(ky*Y);
					const std::size_t n=axis==0u?x:y,e=axis==0u?shape.nx:shape.ny,below=n>0u?n-1u:0u,above=n<e?n:e-1u;
					auto cell=[&](const std::size_t i){return axis==0u?i+shape.nx*(y+shape.ny*z):x+shape.nx*(i+shape.ny*z);};
					const double rho=.5*(states[cell(below)].GasDensity()+states[cell(above)].GasDensity());
					advanced.velocityMPerS.component[axis][face]=vel;momentum.component[axis][face]=rho*vel;}}
			std::fprintf(stderr,"R271_TG_INIT modes=%g U=%g wavelength_m=%.6f\n",m,U,2.0*Pi/kx);
		}'''
h=h.replace(old,old+tg)
old='(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {';assert h.count(old)==1
h=h.replace(old,'(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&\n\t\t\tsimulationTimeS<r271EndTimeS) {')
old='\t\towner.vremanCoefficient=ordinary.force.vremanCoefficient;';assert h.count(old)==1
h=h.replace(old,'\t\towner.vremanCoefficient=std::getenv("RISE_R271_CV")?std::stof(std::getenv("RISE_R271_CV")):ordinary.force.vremanCoefficient;')
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
# Homebrew moved openvdb to 14.0 on 2026-11-02; the production objects were built against 13.1, so pin it
# (headers ahead of the /opt/homebrew/include system dir, which now links openvdb 14; library by path).
VDB='/opt/homebrew/Cellar/openvdb/13.1.0';assert Path(VDB).is_dir(),'openvdb 13.1 no longer installed'
compile_=compile_.replace('/opt/homebrew/opt/openvdb',VDB).replace(' -isystem /opt/homebrew/include ',f' -isystem {VDB}/include -isystem /opt/homebrew/include ',1);assert compile_.count(VDB+'/include')>=2;link=link.replace('/opt/homebrew/opt/openvdb',VDB).replace(' -lopenvdb ',f' {VDB}/lib/libopenvdb.13.1.dylib ')
(OUT/f'build_{SRC}.sh').write_text('set -e\ncd '+shlex.quote(str(make))+'\n'+compile_+'\n'+link+'\n')
with (OUT/f'build_{SRC}.log').open('w') as log:subprocess.run(['sh',str(OUT/f'build_{SRC}.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
# The 13.1 dylib's install name points at the moved opt/ link; reference the Cellar copy (caffeinate drops DYLD_*).
subprocess.run(['install_name_tool','-change','/opt/homebrew/opt/openvdb/lib/libopenvdb.13.1.dylib',VDB+'/lib/libopenvdb.13.1.dylib',str(drv)],check=True,capture_output=True)
subprocess.run(['codesign','-f','-s','-',str(drv)],check=True,capture_output=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(OUT/f'build_manifest_{SRC}.json').write_text(json.dumps({'production_sources_rewritten':False,'git_head':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
 'git_dirty':subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain','--','src','tests'],text=True).strip()!='',
 'harness_changes':['stop at start + RISE_R271_WINDOW_S','Taylor-Green array at t=0 (RISE_R271_TG_MODES, RISE_R271_TG_U)','Vreman override RISE_R271_CV'],
 'sha256':{str(p):sha(p) for p in [ROOT/'tools/fire/r271'/(SRC+'.cpp'),harness,drv]}},indent=2)+'\n')
print(drv)
