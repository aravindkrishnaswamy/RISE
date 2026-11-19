#!/usr/bin/env python3
"""External component experiments. No production source/object is overwritten."""
from pathlib import Path
import subprocess,shlex,difflib,hashlib,json
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r260_components';OUT.mkdir(exist_ok=True)
# Same diagnostic observer, physical-time stop in its external harness.
driver=(ROOT/'tools/fire/r258_ab_driver.cpp').read_text().replace('p.stopAfterAdditionalAcceptedSteps=baseline?2u:stop-step;','p.stopAfterAdditionalAcceptedSteps=baseline?2u:8192u;')
a=driver.index(' double time=start.simulationTimeS;size_t stop=step;');b=driver.index(' RunPersistenceOptions p;',a);driver=driver[:a]+driver[b:]
(ROOT/'tools/fire/r260_component_driver.cpp').write_text(driver)
h=(ROOT/'tests/FireSequenceTest.cpp').read_text();assert h.count('persistence.stopAfterAdditionalAcceptedSteps>=8u')==2
h=h.replace('persistence.stopAfterAdditionalAcceptedSteps>=8u','persistence.stopAfterAdditionalAcceptedSteps>=2u').replace('persistence.stopAfterAdditionalAcceptedSteps<=64u&&','persistence.stopAfterAdditionalAcceptedSteps<=8192u&&')
old='\t\tconst char* solverPhase="initialization";';assert h.count(old)==1
h=h.replace(old,old+'\n\t\tconst double r260EndTimeS=simulationTimeS+4.0;')
old='(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {';assert h.count(old)==1
h=h.replace(old,'(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&\n\t\t\t(persistence.stopAfterAdditionalAcceptedSteps<=2u||simulationTimeS<r260EndTimeS)) {')
old='maximumReducedGravity,maximumActiveDiffusivity,previousStepS);';assert h.count(old)==1
# The selector includes1.1*previous growth. Scale its previous argument so
# halving the admissible step does not recursively halve the growth cap.
h=h.replace(old,'maximumReducedGravity,maximumActiveDiffusivity,std::getenv("RISE_R260_HALF_STEP")?2.0*previousStepS:previousStepS);')
old='eventStep=std::min(eventStep,persistence.maximumProductionSourceStepS);';assert h.count(old)==1
h=h.replace(old,old+'\n\t\t\tif(std::getenv("RISE_R260_HALF_STEP"))eventStep*=0.5;')
(OUT/'Harness.cpp').write_text(h)
original=(ROOT/'src/Library/Utilities/FireProductionAdvectionMac.mm').read_text();s=original
kernel=r'''
// Diagnostic only: centered interior advective gas mass carrier plus unchanged
// physical gas diffusion. Boundary total carriers remain the accepted ones.
kernel void r260_centered_carrier(device const float* low [[buffer(0)]],
 device const float* delta [[buffer(1)]],device const float* alpha [[buffer(2)]],
 device const float* q [[buffer(3)]],device const float* ux [[buffer(4)]],
 device const float* uy [[buffer(5)]],device const float* uz [[buffer(6)]],
 device const float* physicalGas [[buffer(7)]],device float* central [[buffer(8)]],
 device float* zero [[buffer(9)]],constant FCTParams& p [[buffer(10)]],
 uint gid [[thread_position_in_grid]]){
 uint all=fct_all_faces(p);if(gid>=all)return;uint axis,x,y,z;fct_decode_face(p,gid,axis,x,y,z);
 for(uint component=0u;component<9u;++component){central[component*all+gid]=0.0f;zero[component*all+gid]=0.0f;}
 uint coordinate=fct_coordinate(axis,x,y,z),extent=fct_extent(p,axis);float gas=0.0f;
 if(coordinate==0u||coordinate==extent)gas=fct_accepted_gas(low,delta,alpha,p,axis,x,y,z);
 else{uint lx=x,ly=y,lz=z;fct_set_coordinate(axis,coordinate-1u,lx,ly,lz);
  float rhoL=0.0f,rhoR=0.0f;for(uint k=1u;k<=6u;++k){rhoL+=q[k*p.cells+fct_cell(p,lx,ly,lz)];rhoR+=q[k*p.cells+fct_cell(p,x,y,z)];}
  device const float* v=axis==0u?ux:(axis==1u?uy:uz);
  gas=0.5f*(rhoL+rhoR)*v[gid-fct_face_offset(p,axis)]+physicalGas[gid];
 }
 central[all+gid]=gas;
}
'''
needle='inline float fct_velocity(device const float* ux';assert s.count(needle)==1;s=s.replace(needle,kernel+'\n'+needle)
s=s.replace('id<MTLComputePipelineState> compatibleStageRate;','id<MTLComputePipelineState> compatibleStageRate;\n\t\t\tid<MTLComputePipelineState> centeredCarrier;')
s=s.replace('compatibleStageRate(nil),applyMomentumRate(nil),','compatibleStageRate(nil),centeredCarrier(nil),applyMomentumRate(nil),')
s=s.replace('compatibleStageRate=makePipeline("fct_compatible_stage_rate");','compatibleStageRate=makePipeline("fct_compatible_stage_rate");\n\t\t\t\t\tcenteredCarrier=makePipeline("r260_centered_carrier");')
s=s.replace('!compatibleStageRate||!applyMomentumRate','!compatibleStageRate||!centeredCarrier||!applyMomentumRate')
needle='\t\t\t\tif(!EvaluateFireProductionNonpressureMomentumRHSMetalResident(rhs,output.nonpressure,';assert s.count(needle)==1
prep='''\t\t\t\tconst std::size_t carrierBytes=9u*allFaces_*sizeof(float);
                id<MTLBuffer> central=Private(carrierBytes),zero=Private(carrierBytes);
                id<MTLBuffer> centralR0=stage==1u?Private(carrierBytes):nil;
                id<MTLBuffer> zeroR0=stage==1u?Private(carrierBytes):nil;
                if(!central||!zero||!Encode(command,fct_.centeredCarrier,
                    {output.physical->lowComposite,output.physical->advectiveDelta,
                     output.candidate->faceAlpha,state,output.projection.velocityMPerS[0],
                     output.projection.velocityMPerS[1],output.projection.velocityMPerS[2],
                     output.physical->physicalGas,central,zero,fctParameters_[stage]},allFaces_))return false;
                if(stage==1u&&r0&&(!centralR0||!zeroR0||!Encode(command,fct_.centeredCarrier,
                    {r0->physical->lowComposite,r0->physical->advectiveDelta,
                     output.candidate->faceAlpha,q0_,r0->projection.velocityMPerS[0],
                     r0->projection.velocityMPerS[1],r0->projection.velocityMPerS[2],
                     r0->physical->physicalGas,centralR0,zeroR0,fctParameters_[0]},allFaces_)))return false;
'''
s=s.replace(needle,prep+needle)
old='{output.physical->lowComposite,output.physical->advectiveDelta,\n\t\t\t\t\t output.candidate->faceAlpha,output.projection.velocityMPerS[0],';assert s.count(old)==1;s=s.replace(old,'{central,zero,\n\t\t\t\t\t output.candidate->faceAlpha,output.projection.velocityMPerS[0],')
old='{r0->physical->lowComposite,r0->physical->advectiveDelta,\n\t\t\t\t\t  output.candidate->faceAlpha,r0->projection.velocityMPerS[0],';assert s.count(old)==1;s=s.replace(old,'{centralR0,zeroR0,\n\t\t\t\t\t  output.candidate->faceAlpha,r0->projection.velocityMPerS[0],')
s=s.replace('correction[component]=-\n','correction[component] = -\n') # spelling only; silence Metal's ambiguous unary-token warning
source=OUT/'CenteredAdvection.mm';source.write_text(s)
(ROOT/'docs/data/fire_r260/centered_momentum.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='production',tofile='external_centered_momentum')))
# Compile one shared observer; baseline andhalfstep use original solver objects.
lines=(BASE/'r256_limiter_diagnostic/build_limiter.sh').read_text().splitlines()
c=next(x for x in lines if x.startswith('c++ ') and ' -c ' in x)
c=c.replace(str(ROOT/'tools/fire/r256_limiter_driver.cpp'),str(ROOT/'tools/fire/r260_component_driver.cpp')).replace(str(BASE/'r256_limiter_driver.o'),str(OUT/'observer.o')).replace(str(BASE/'r256_limiter_diagnostic/FireSequenceObserverHarness.cpp'),str(OUT/'Harness.cpp'))
link=next(x for x in lines if x.startswith('c++ ') and ' -c ' not in x).replace(str(BASE/'r256_limiter_driver.o'),str(OUT/'observer.o'))
mc=link.replace(' -o '+str(BASE/'r256_limiter_driver')+' ',' -o '+str(OUT/'mc_driver')+' ')
center=link.replace(' -o '+str(BASE/'r256_limiter_driver')+' ',' -o '+str(OUT/'centered_driver')+' ').replace('../../../src/Library/Utilities/FireProductionAdvectionMac.o',str(OUT/'CenteredAdvection.o'))
cmd=subprocess.check_output(['make','-C',str(ROOT/'build/make/rise'),'-n','-B','../../../src/Library/Utilities/FireProductionAdvectionMac.o'],text=True)
mm=next(x for x in cmd.splitlines() if x.startswith('c++ ')).replace('../../../src/Library/Utilities/FireProductionAdvectionMac.mm',str(source)).replace('../../../src/Library/Utilities/FireProductionAdvectionMac.o',str(OUT/'CenteredAdvection.o')).replace(' -c ',f' -I{ROOT}/src/Library/Utilities -c ')
script='set -e\ncd '+shlex.quote(str(ROOT/'build/make/rise'))+'\n'+c+'\n'+mm+'\n'+mc+'\n'+center+'\n';(OUT/'build.sh').write_text(script)
with (OUT/'build.log').open('w') as log:subprocess.run(['sh',str(OUT/'build.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
files=[ROOT/'src/Library/Utilities/FireProductionAdvectionMac.mm',OUT/'mc_driver',OUT/'centered_driver',OUT/'Harness.cpp',source]
(ROOT/'docs/data/fire_r260/build_manifest.json').write_text(json.dumps({'production_changed':False,'half_step':'half ordinary CFL/source-cap admissible step before eventalignment','end_condition':'first ordinary accepted step reaching4s, no extra endpoint shortening','centered':'interior centered rho*u plus unchanged physical gas flux; boundary accepted carriers unchanged; scalar MC/FCT untouched; not full FDS rotational formulation','sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}},indent=2)+'\n')
