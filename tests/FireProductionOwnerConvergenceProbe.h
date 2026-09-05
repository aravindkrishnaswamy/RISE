#ifndef RISE_TEST_FIRE_PRODUCTION_OWNER_CONVERGENCE_PROBE_H
#define RISE_TEST_FIRE_PRODUCTION_OWNER_CONVERGENCE_PROBE_H

#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// Diagnostic serialization of the already captured owner operands.  This file
// has no production publication API and supplies no convergence tolerance.
// A delta between iterations is evidence, not a truncation-error certificate.
namespace FireProductionOwnerConvergenceProbe
{

struct Geometry
{
	std::size_t nx,ny,nz,columnX,columnY;
};

struct Context
{
	std::string caseSHA256;
	std::string source;
	std::uint64_t ownerIdentity=0u;
	std::array<std::uint32_t,3> acceptedIterations={{0u,0u,0u}};
	bool diagnosticOnly=true;
};

inline bool IsSHA256(const std::string& value)
{
	return value.size()==64u&&value.find_first_not_of("0123456789abcdef")==std::string::npos;
}

// The digest function is supplied by the test's existing SHA-256 implementation.
// The resulting binding authenticates this diagnostic transcript only; it never
// substitutes for a device authority seal or an accepted checkpoint identity.
template<class Trace,class Digest>
bool Serialize(const std::array<const std::vector<Trace>*,3>& stages,
	const Geometry& geometry,const Context& context,const Digest& digest,
	std::string& text,std::string& binding,std::string& error,std::string* csv=nullptr)
{
	text.clear();binding.clear();
	if(csv)csv->clear();
	if(!context.diagnosticOnly){error="convergence probe is diagnostic-only";return false;}
	if(!IsSHA256(context.caseSHA256)||context.source.empty()||
		context.source.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.")!=
		std::string::npos||context.ownerIdentity==0u){
		error="convergence probe requires an identified qualified input and owner";return false;}
	if(geometry.nx==0u||geometry.ny==0u||geometry.nz==0u||
		geometry.columnX>=geometry.nx||geometry.columnY>=geometry.ny||
		geometry.nx>std::numeric_limits<std::size_t>::max()/geometry.ny||
		geometry.nx*geometry.ny>std::numeric_limits<std::size_t>::max()/geometry.nz){
		error="convergence probe column/shape mismatch";return false;}
	const std::size_t cells=geometry.nx*geometry.ny*geometry.nz;
	for(const std::size_t extra:{geometry.ny*geometry.nz,geometry.nx*geometry.nz,
		geometry.nx*geometry.ny})if(cells>std::numeric_limits<std::size_t>::max()-extra){
		error="convergence probe face shape overflow";return false;}
	const std::array<std::size_t,3> faces={{cells+geometry.ny*geometry.nz,
		cells+geometry.nx*geometry.nz,cells+geometry.nx*geometry.ny}};
	std::ostringstream result;result<<std::setprecision(17)
		<<"OWNER_CONVERGENCE_PROBE version=1 scope=diagnostic_only source="<<context.source
		<<" case_record_sha256="<<context.caseSHA256<<" owner_identity="<<context.ownerIdentity
		<<" shape="<<geometry.nx<<','<<geometry.ny<<','<<geometry.nz
		<<" column="<<geometry.columnX<<','<<geometry.columnY
		<<" convergence_enclosure=unavailable truncation_acceptance=unavailable"
		<<" provisional_momentum_role=fixed_stage_projection_input"
		<<" zero_provisional_delta_proves_candidate_convergence=0"
		<<" next_candidate_momentum_capture=unavailable"
		<<" delta_semantics=immediately_preceding_trace_in_same_stage\n";
	std::ostringstream csvRows;csvRows<<std::setprecision(17)
		<<"scope,source,case_record_sha256,owner_identity,stage,phase,raw_iteration_tag,iteration,"
		"trace_index,axis,face,column_z,side,provisional_momentum_kg_m-2_s-1,"
		"projected_velocity_m_s-1,projected_momentum_kg_m-2_s-1,shared_alpha,delta_reference_trace_index,"
		"delta_previous_provisional_momentum_kg_m-2_s-1,delta_previous_projected_velocity_m_s-1,"
		"delta_previous_projected_momentum_kg_m-2_s-1,local_convergence_enclosure\n";
	for(unsigned int stage=0u;stage<3u;++stage){
		if(!stages[stage]||stages[stage]->size()<3u||context.acceptedIterations[stage]==0u){
			error="convergence probe missing complete stage";return false;}
		const auto& traces=*stages[stage];
		std::uint32_t nextIteration=0u;
		for(std::size_t index=0u;index<traces.size();++index){const auto& trace=traces[index];
			const bool bootstrap=trace.iteration==UINT32_MAX;
			const bool terminal=!bootstrap&&(trace.iteration&UINT32_C(0x80000000))!=0u;
			const std::uint32_t iteration=trace.iteration&UINT32_C(0x7fffffff);
			if((index==0u)!=bootstrap||(!bootstrap&&!terminal&&iteration!=nextIteration)||
				(terminal&&(nextIteration==0u||iteration!=nextIteration-1u))){
				error="convergence probe stale/out-of-order iteration";return false;}
			if(!bootstrap&&!terminal)++nextIteration;
			if(index+1u==traces.size()&&(!terminal||nextIteration!=context.acceptedIterations[stage])){
				error="convergence probe terminal/stage count mismatch";return false;}
			if(trace.projectionTargetPerS.size()!=cells){
				error="convergence probe target shape mismatch";return false;}
			for(unsigned int axis=0u;axis<3u;++axis)
				if(trace.provisionalMomentumKGPerM2S[axis].size()!=faces[axis]||
					trace.projectedMomentumKGPerM2S[axis].size()!=faces[axis]||
					trace.projectedVelocityMPerS[axis].size()!=faces[axis]||
					(trace.sharedFaceAlpha[axis].size()!=faces[axis]&&
						!((bootstrap||stage==2u)&&trace.sharedFaceAlpha[axis].empty()))){
					error="convergence probe momentum/velocity/alpha shape mismatch";return false;}
			const char* phase=bootstrap?"bootstrap":(terminal?"terminal_reprojection":"picard");
			const auto emit=[&](const unsigned int axis,const std::size_t face,
				const std::size_t z,const unsigned int side){
				const double momentum=trace.provisionalMomentumKGPerM2S[axis][face];
				const double velocity=trace.projectedVelocityMPerS[axis][face];
				const double projectedMomentum=trace.projectedMomentumKGPerM2S[axis][face];
				const bool alphaAvailable=!trace.sharedFaceAlpha[axis].empty();
				const double alpha=alphaAvailable?static_cast<double>(trace.sharedFaceAlpha[axis][face]):0.0;
				if(!std::isfinite(momentum)||!std::isfinite(velocity)||!std::isfinite(projectedMomentum)||
					!std::isfinite(alpha))return false;
				result<<"OWNER_CONVERGENCE_FACE stage=R"<<stage<<" phase="<<phase
					<<" raw_iteration_tag="<<trace.iteration
					<<" iteration="<<(bootstrap?0u:iteration+1u)<<" trace_index="<<index
					<<" axis="<<axis<<" face="<<face<<" column_z="<<z<<" side="<<side
					<<" provisional_momentum_kg_m-2_s-1="<<momentum
					<<" projected_velocity_m_s-1="<<velocity
					<<" projected_momentum_kg_m-2_s-1="<<projectedMomentum;
				if(alphaAvailable)result<<" shared_alpha="<<alpha;
				else result<<" shared_alpha=unavailable";
				csvRows<<"diagnostic_only,"<<context.source<<','<<context.caseSHA256<<','
					<<context.ownerIdentity<<",R"<<stage<<','<<phase<<','<<trace.iteration<<','
					<<(bootstrap?0u:iteration+1u)<<','<<index<<','<<axis<<','<<face<<','<<z<<','
					<<side<<','<<momentum<<','<<velocity<<','<<projectedMomentum<<',';
				if(alphaAvailable)csvRows<<alpha;else csvRows<<"unavailable";
				if(index>0u){const auto& previous=traces[index-1u];result
					<<" delta_reference_trace_index="<<index-1u
					<<" delta_previous_provisional_momentum_kg_m-2_s-1="
					<<momentum-static_cast<double>(previous.provisionalMomentumKGPerM2S[axis][face])
					<<" delta_previous_projected_velocity_m_s-1="
					<<velocity-static_cast<double>(previous.projectedVelocityMPerS[axis][face])
					<<" delta_previous_projected_momentum_kg_m-2_s-1="
					<<projectedMomentum-static_cast<double>(previous.projectedMomentumKGPerM2S[axis][face]);
					csvRows<<','<<index-1u<<','
						<<momentum-static_cast<double>(previous.provisionalMomentumKGPerM2S[axis][face])<<','
						<<velocity-static_cast<double>(previous.projectedVelocityMPerS[axis][face])<<','
						<<projectedMomentum-static_cast<double>(previous.projectedMomentumKGPerM2S[axis][face]);}
				else{result<<" delta_previous=unavailable";
					csvRows<<",unavailable,unavailable,unavailable,unavailable";}
				csvRows<<",unavailable\n";
				result<<" local_convergence_enclosure=unavailable\n";return true;};
			for(std::size_t z=0u;z<geometry.nz;++z)for(unsigned int side=0u;side<2u;++side){
				const std::size_t xFace=geometry.columnX+side+(geometry.nx+1u)*
					(geometry.columnY+geometry.ny*z);
				const std::size_t yFace=geometry.columnX+geometry.nx*
					(geometry.columnY+side+(geometry.ny+1u)*z);
				if(!emit(0u,xFace,z,side)||!emit(1u,yFace,z,side)){
					error="convergence probe nonfinite column operand";return false;}}
			for(std::size_t z=0u;z<=geometry.nz;++z){const std::size_t face=
				geometry.columnX+geometry.nx*(geometry.columnY+geometry.ny*z);
				if(!emit(2u,face,z,0u)){error="convergence probe nonfinite column operand";return false;}}
		}
	}
	text=result.str();binding=digest(text);
	if(!IsSHA256(binding)){text.clear();binding.clear();error="convergence probe digest failure";return false;}
	if(csv)*csv=csvRows.str();
	return true;
}

template<class Trace,class Digest>
bool Write(std::ostream& output,const std::array<const std::vector<Trace>*,3>& stages,
	const Geometry& geometry,const Context& context,const Digest& digest,
	const std::string& expectedBinding,std::string& error)
{
	std::string text,binding;
	if(!Serialize(stages,geometry,context,digest,text,binding,error))return false;
	if(!IsSHA256(expectedBinding)||binding!=expectedBinding){
		error="convergence probe stale/mismatched diagnostic binding";return false;}
	output<<text<<"OWNER_CONVERGENCE_BINDING sha256="<<binding<<"\n";
	return static_cast<bool>(output);
}

}
#endif
