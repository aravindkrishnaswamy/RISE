// r211: resident r78 evidence adapter. Included inside FireSequenceTest's
// implementation namespace, after the canonical-record helpers. No solver math.
bool ResidentResumeSHA(const std::string& value)
{
	return value.size()==64u&&std::all_of(value.begin(),value.end(),[](char c){
		return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}

struct ResidentResumeTraceV2
{
	std::string build,executable,executablePath,csvPath,csvSHA,logPath,logSHA;
	std::vector<std::uint64_t> times,steps;
	std::vector<std::string> inputs,outputs,kernels;
};

bool ResidentResumeTraceV2Read(const RISECBOR64::Value& payload,
	ResidentResumeTraceV2& trace,const std::uint64_t firstStep,
	const std::uint64_t beginningTimeBits,const std::string& checkpointSHA,
	const std::string& checkpointBuild,std::string& error)
{
	std::uint64_t digestVersion=0u;
	if(payload.GetType()!=RISECBOR64::Value::Map||payload.GetMap().size()!=13u||
		!ReadTextMember(payload,"build_id",trace.build)||
		!ReadTextMember(payload,"executable_sha256",trace.executable)||
		!ReadTextMember(payload,"executable_path",trace.executablePath)||
		!ReadTextMember(payload,"csv_path",trace.csvPath)||
		!ReadTextMember(payload,"csv_sha256",trace.csvSHA)||
		!ReadTextMember(payload,"log_path",trace.logPath)||
		!ReadTextMember(payload,"log_sha256",trace.logSHA)||
		!ReadUnsignedMember(payload,"digest_version",digestVersion)||digestVersion!=2u||
		!ReadUnsignedArray(payload,"accepted_time_s_bits",trace.times)||
		!ReadUnsignedArray(payload,"dt_s_bits",trace.steps)||
		!ReadTextArray(payload,"input_payload_roots",trace.inputs)||
		!ReadTextArray(payload,"publication_payload_roots",trace.outputs)||
		!ReadTextArray(payload,"kernel_set_sha256",trace.kernels)){
		error="resident r78 v2 trace schema";return false;
	}
	const std::size_t count=trace.times.size();
	if(count<8u||firstStep>std::numeric_limits<std::uint64_t>::max()-count||
		trace.steps.size()!=count||trace.inputs.size()!=count||trace.outputs.size()!=count||
		trace.kernels.size()!=count||!ResidentResumeSHA(trace.build)||
		!ResidentResumeSHA(trace.executable)||!ResidentResumeSHA(trace.csvSHA)||
		!ResidentResumeSHA(trace.logSHA)){
		error="resident r78 v2 incomplete evidence (N must be >=8)";return false;
	}
	// Files remain evidence dependencies, not strings asserted by a certificate.
	// The issuing runner executes these exact binaries in fresh output directories.
	if(!std::filesystem::path(trace.executablePath).is_absolute()||
		!std::filesystem::path(trace.csvPath).is_absolute()||
		!std::filesystem::path(trace.logPath).is_absolute()||
		DigestFile(trace.executablePath)!=trace.executable||
		DigestFile(trace.csvPath)!=trace.csvSHA||DigestFile(trace.logPath)!=trace.logSHA){
		error="resident r78 v2 executed binary or raw evidence mismatch";return false;
	}
	std::ifstream csv(trace.csvPath);std::string line;
	auto Split=[](const std::string& row){
		std::vector<std::string> fields;std::istringstream stream(row);std::string field;
		while(std::getline(stream,field,','))fields.push_back(field);
		return fields;
	};
	if(!std::getline(csv,line)){error="resident r78 v2 missing CSV";return false;}
	const auto header=Split(line);std::map<std::string,std::size_t> columns;
	for(std::size_t i=0u;i<header.size();++i)if(!columns.emplace(header[i],i).second){
		error="resident r78 v2 duplicate CSV column";return false;}
	for(const char* name:{"accepted_step","time_s","dt_s","payload_digest_format",
		"payload_digest_version","input_payload_root_sha256","publication_payload_root_sha256",
		"qualified_kernel_set_sha256"})if(!columns.count(name)){
		error="resident r78 v2 missing CSV column";return false;}
	double previous=0.0;std::memcpy(&previous,&beginningTimeBits,sizeof(previous));
	if(!std::isfinite(previous)||previous<0.0){error="resident r78 v2 beginning time";return false;}
	const bool legacy=trace.executable==
		"70eca63d46873ab13cee9d97f189d9fbc013e0b1d1831b0b100a3985294b7db9"&&
		trace.build=="a3ef35639751dbc0c3f21aaca7d2b99f0e18ed477ed7c98ab396e2ab8244e1f5";
	const std::string tag=legacy?"OWNER_EOS_DIAGNOSTIC":"OWNER_CERTIFICATE_DIAGNOSTIC";
	std::ostringstream startRecord;startRecord<<std::setprecision(17)<<tag<<
		" checkpoint_sha256="<<checkpointSHA<<" build="<<checkpointBuild<<
		" accepted_steps="<<firstStep<<" beginning_s="<<previous<<" migration_authority=false";
	const std::string endRecord=tag+
		"_END solver_accepted=1 checkpoint_unchanged=1 migration_authority=false error=";
	const std::string executionRecord="RESIDENT_RESUME_EXECUTION build_id="+trace.build+
		" executable_sha256="+trace.executable;
	std::ifstream log(trace.logPath);std::size_t starts=0u,ends=0u,executions=0u;
	while(std::getline(log,line)){
		if(line.compare(0u,tag.size()+1u,tag+" ")==0){
			if(line!=startRecord.str()){error="resident r78 v2 checkpoint execution mismatch";return false;}
			++starts;
		}
		if(line.compare(0u,tag.size()+5u,tag+"_END ")==0){
			if(line!=endRecord){error="resident r78 v2 execution did not succeed";return false;}
			++ends;
		}
		if(line.compare(0u,26u,"RESIDENT_RESUME_EXECUTION ")==0){
			if(line!=executionRecord){error="resident r78 v2 producer identity mismatch";return false;}
			++executions;
		}
	}
	if(!log.eof()||starts!=1u||ends!=1u||(!legacy&&executions!=1u)){
		error="resident r78 v2 missing executed identity/terminal records";return false;}
	std::size_t index=0u;
	while(std::getline(csv,line)){
		const auto fields=Split(line);
		if(index>=count||fields.size()!=header.size()){
			error="resident r78 v2 CSV count";return false;}
		auto Field=[&](const char* name)->const std::string&{return fields[columns.at(name)];};
		double time=0.0,dt=0.0;char* end=nullptr;
		time=std::strtod(Field("time_s").c_str(),&end);
		if(!end||*end!='\0'||!std::isfinite(time)||time<=previous){
			error="resident r78 v2 non-increasing time";return false;}
		dt=std::strtod(Field("dt_s").c_str(),&end);
		if(!end||*end!='\0'||!std::isfinite(dt)||dt<=0.0||time!=previous+dt||
			DoubleBits(time)!=trace.times[index]||DoubleBits(dt)!=trace.steps[index]||
			Field("accepted_step")!=std::to_string(firstStep+index+1u)||
			Field("payload_digest_format")!="rise-payload-sha256-merkle"||
			Field("payload_digest_version")!="2"||
			Field("input_payload_root_sha256")!=trace.inputs[index]||
			Field("publication_payload_root_sha256")!=trace.outputs[index]||
			Field("qualified_kernel_set_sha256")!=trace.kernels[index]||
			!ResidentResumeSHA(trace.inputs[index])||!ResidentResumeSHA(trace.outputs[index])||
			!ResidentResumeSHA(trace.kernels[index])){
			error="resident r78 v2 raw step/root mismatch";return false;}
		previous=time;++index;
	}
	if(index!=count||!csv.eof()){error="resident r78 v2 truncated CSV";return false;}
	return true;
}

bool ParseResidentResumeCertificateV2(const RISECBOR64::Value& payload,
	ResumeEquivalenceCertificate& certificate,std::string& error)
{
	std::string kind;std::uint64_t version=0u,beginningBits=0u;
	if(payload.GetType()!=RISECBOR64::Value::Map||payload.GetMap().size()!=11u||
		!ReadUnsignedMember(payload,"schema_version",version)||version!=2u||
		!ReadTextMember(payload,"record_kind",kind)||kind!="fire-resume-equivalence-certificate-v2"||
		!ReadTextMember(payload,"checkpoint_sha256",certificate.checkpointDigest)||
		!ReadUnsignedMember(payload,"resumed_from_step",certificate.resumedFromStep)||
		!ReadUnsignedMember(payload,"beginning_time_s_bits",beginningBits)||
		!ReadTextMember(payload,"old_build_id",certificate.oldBuildId)||
		!ReadTextMember(payload,"new_build_id",certificate.newBuildId)||
		!ReadTextMember(payload,"old_executable_sha256",certificate.oldExecutableDigest)||
		!ReadTextMember(payload,"new_executable_sha256",certificate.newExecutableDigest)||
		!ResidentResumeSHA(certificate.checkpointDigest)||
		!payload.Find("old_trace")||!payload.Find("new_trace")){
		error="resident r78 v2 certificate schema";return false;}
	ResidentResumeTraceV2 oldTrace,newTrace;
	if(!ResidentResumeTraceV2Read(*payload.Find("old_trace"),oldTrace,
		certificate.resumedFromStep,beginningBits,certificate.checkpointDigest,certificate.oldBuildId,error)||
		!ResidentResumeTraceV2Read(*payload.Find("new_trace"),newTrace,
		certificate.resumedFromStep,beginningBits,certificate.checkpointDigest,certificate.oldBuildId,error))return false;
	if(oldTrace.build!=certificate.oldBuildId||newTrace.build!=certificate.newBuildId||
		oldTrace.executable!=certificate.oldExecutableDigest||
		newTrace.executable!=certificate.newExecutableDigest||oldTrace.build==newTrace.build||
		oldTrace.executable==newTrace.executable||oldTrace.times!=newTrace.times||
		oldTrace.steps!=newTrace.steps||oldTrace.inputs!=newTrace.inputs||
		oldTrace.outputs!=newTrace.outputs||oldTrace.kernels!=newTrace.kernels){
		error="resident r78 v2 executed identities or equal-time roots differ";return false;}
	certificate.schemaVersion=2u;certificate.beginningTimeBits=beginningBits;
	certificate.acceptedStepCount=oldTrace.times.size();
	certificate.timeStepBits=oldTrace.steps;
	return true;
}
