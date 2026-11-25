#ifndef RISE_TEST_FIRE_PRODUCTION_ONSET_EVIDENCE_H
#define RISE_TEST_FIRE_PRODUCTION_ONSET_EVIDENCE_H

#include <algorithm>
#include <cmath>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace FireProductionOnsetEvidence
{
inline const char* TrajectoryHeader()
{
	return "accepted_step,time_s,dt_s,maximum_velocity_m_per_s,axis,face,x,y,z,"
		"manifold_max,manifold_p95,manifold_p50,tail_cells,tail_drained_m3,"
		"device_ms,wall_ms,owner_identity,owner_commits,owner_projections,"
		"owner_r0_iterations,owner_r1_iterations,owner_r2_iterations,"
		"owner_actual_working_set_bytes,owner_certified_working_set_bytes,"
		"owner_projection_device_ms,owner_nonprojection_device_ms,"
		"intermediate_seal_format,intermediate_digest_version,payload_digest_format,payload_digest_version,"
		"qualified_kernel_set_sha256,input_payload_root_sha256,publication_payload_root_sha256,publication_payload_bytes";
}
inline bool Survived(const bool reachedHorizon,const bool crossed)
{
	return reachedHorizon&&!crossed;
}

// The accepted schedule, not file existence or an arbitrary last line, owns
// the trajectory's extent. A continuation starts at its retained step + 1.
inline bool ValidateTrajectory(std::istream& input,const std::vector<double>& schedule,
	const std::size_t firstStep,const double stopVelocity,bool& crossed)
{
	crossed=false;
	if(firstStep==0u||firstStep>schedule.size()||!std::isfinite(stopVelocity)||
		!(stopVelocity>0.0))return false;
	std::string line;
	if(!std::getline(input,line)||line!=TrajectoryHeader())
		return false;
	const auto columns=std::count(line.begin(),line.end(),',');
	double time=0.0;
	for(std::size_t step=1u;step<=schedule.size();++step){
		const double dt=schedule[step-1u];
		if(!std::isfinite(dt)||!(dt>0.0))return false;
		time+=dt;if(!std::isfinite(time))return false;
		if(step<firstStep)continue;
		if(!std::getline(input,line)||std::count(line.begin(),line.end(),',')!=columns)return false;
		std::istringstream row(line);std::size_t observedStep=0u;
		double observedTime=0.0,observedDt=0.0,velocity=0.0;
		char a=0,b=0,c=0,d=0;
		if(!(row>>observedStep>>a>>observedTime>>b>>observedDt>>c>>velocity>>d)||
			a!=','||b!=','||c!=','||d!=','||observedStep!=step||observedTime!=time||
			observedDt!=dt||!std::isfinite(velocity)||velocity<0.0)return false;
		crossed=crossed||velocity>=stopVelocity;
	}
	return !std::getline(input,line)&&input.eof()&&!input.bad();
}
}
#endif
