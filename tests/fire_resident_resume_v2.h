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
	// r216 additionally admits one pinned historical from-zero transcript; that
	// binary is authenticated but is not claimed to have run a new diagnostic.
	if(!std::filesystem::path(trace.executablePath).is_absolute()||
		!std::filesystem::path(trace.csvPath).is_absolute()||
		!std::filesystem::path(trace.logPath).is_absolute()||
		DigestFile(trace.executablePath)!=trace.executable||
		DigestFile(trace.csvPath)!=trace.csvSHA||DigestFile(trace.logPath)!=trace.logSHA){
		error="resident r78 v2 executed binary or raw evidence mismatch";return false;
	}
	const bool historicalR214=trace.executable==
		"45370ad3bcc7035fb228a1f839efe5059cbcc791a64486bd2a1f0b1c3025dc2c"&&
		trace.build=="0d7537194b877cdffd248de25eccf52262f824b4f1d4cde5fe035ad1eee52175";
	const bool historicalR217=trace.executable==
		"91978041bb53124e45324838a082d6fb64bb0f2a6da28c68ddc7e8c186faaefa"&&
		trace.build=="f2ee8ec4cdf43ed7c527dce95718439888561cb70861f99c22f3f5fd4a311609";
	const bool historical=historicalR214||historicalR217;
	if(historicalR214){
		const auto run=std::filesystem::path(trace.csvPath).parent_path().parent_path();
		const auto root=run.parent_path().parent_path().parent_path().parent_path();
		if(firstStep!=792u||count!=8u||beginningTimeBits!=DoubleBits(1.2894936816301197)||
			checkpointSHA!="65bb74aa8186a95acde57de593d54de664303c4d92ebaea374eea75e1f138c19"||
			checkpointBuild!=trace.build||
			std::filesystem::path(trace.csvPath)!=run/"budgets/maximum_velocity_trajectory.csv"||
			std::filesystem::path(trace.logPath)!=std::filesystem::path(run.string()+".log")||
			trace.csvSHA!="3ec4f50479a59ca90c7610290ad110279e7356f6e0907c470a6fa753a4545b50"||
			trace.logSHA!="b6feee08b9634077e834fbd1de009ddbc1b0f06d5e76849217b8aca95a6768d7"||
			DigestFile(run/"from_zero_identity.v1")!="96558b780535ecadc917160084b497ffe413a77fd767b5ec7cb747b90f7f7998"||
			DigestFile(root/"docs/fire_r214_tier10_onset.v1")!="983cfb4975a614a7bee2593c6d0f4536679925911afd0b22a5b2e897d6b04dc2"){
			error="resident r78 v2 historical r214 authority mismatch";return false;}
	}
	if(historicalR217){
		const auto run=std::filesystem::path(trace.csvPath).parent_path().parent_path();
		const auto root=run.parent_path().parent_path().parent_path().parent_path();
		if(firstStep!=826u||count!=9u||beginningTimeBits!=DoubleBits(1.3440701774088666)||
			checkpointSHA!="f18cac44b71b1fdac106d30d86bcc1efb955ed39ed4edc0a66be912a4130c337"||
			checkpointBuild!=trace.build||
			std::filesystem::path(trace.csvPath)!=run/"budgets/maximum_velocity_trajectory.csv"||
			std::filesystem::path(trace.logPath)!=std::filesystem::path(run.string()+".log")||
			trace.csvSHA!="7c9414de24da4a82dbcc9381364b4b2d2fae2a54a5cc6c7b5005b026ab03fb30"||
			trace.logSHA!="3682feb561f5c201da8120bb76c3dc533e5fffa0bc67a04c069886997a2f5b51"||
			DigestFile(run/"from_zero_identity.v1")!="2334b0bb913b9bc5fa42a34f55c6fb1bf6ba1d43015baec7afcd2403a5292828"||
			DigestFile(root/"docs/fire_r214_tier10_onset.v1")!="983cfb4975a614a7bee2593c6d0f4536679925911afd0b22a5b2e897d6b04dc2"){
			error="resident r78 v2 historical r217 authority mismatch";return false;}
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
		if(historical)continue; // Full original log SHA is the execution witness.
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
	if(!log.eof()||(!historical&&(starts!=1u||ends!=1u||(!legacy&&executions!=1u)))){
		error="resident r78 v2 missing executed identity/terminal records";return false;}
	std::size_t index=0u,rawIndex=0u;
	if(historical)previous=0.0;
	while(std::getline(csv,line)){
		const auto fields=Split(line);
		if((!historical&&index>=count)||fields.size()!=header.size()){
			error="resident r78 v2 CSV count";return false;}
		auto Field=[&](const char* name)->const std::string&{return fields[columns.at(name)];};
		double time=0.0,dt=0.0;char* end=nullptr;
		time=std::strtod(Field("time_s").c_str(),&end);
		if(!end||*end!='\0'||!std::isfinite(time)||time<=previous){
			error="resident r78 v2 non-increasing time";return false;}
		dt=std::strtod(Field("dt_s").c_str(),&end);
		if(!end||*end!='\0'||!std::isfinite(dt)||dt<=0.0||time!=previous+dt||
			Field("accepted_step")!=std::to_string(historical?rawIndex+1u:firstStep+index+1u)||
			Field("payload_digest_format")!="rise-payload-sha256-merkle"||
			Field("payload_digest_version")!="2"){
			error="resident r78 v2 raw timeline mismatch";return false;}
		const bool selected=!historical||(rawIndex>=firstStep&&rawIndex<firstStep+count);
		if(historical&&((rawIndex==firstStep&&DoubleBits(previous)!=beginningTimeBits)||
			Field("qualified_kernel_set_sha256")!=(historicalR217?
			"e938373ec6cd9e0217c04242ad96cd4d46055236e422bd7ad0ea34e164d0a8b2":
			"d580d078ea083ee18032abfb027271ada9d4d1dabab50b35cb6eb7a503258a0e"))){
			error="resident r78 v2 historical checkpoint join or kernel mismatch";return false;}
		if(selected&&(DoubleBits(time)!=trace.times[index]||DoubleBits(dt)!=trace.steps[index]||
			Field("input_payload_root_sha256")!=trace.inputs[index]||
			Field("publication_payload_root_sha256")!=trace.outputs[index]||
			Field("qualified_kernel_set_sha256")!=trace.kernels[index]||
			!ResidentResumeSHA(trace.inputs[index])||!ResidentResumeSHA(trace.outputs[index])||
			!ResidentResumeSHA(trace.kernels[index]))){
			error="resident r78 v2 raw step/root mismatch";return false;}
		previous=time;++rawIndex;if(selected)++index;
	}
	if(index!=count||!csv.eof()||(historical&&rawIndex!=(historicalR217?835u:805u))){
		error="resident r78 v2 truncated CSV";return false;}
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
	certificate.residentAcceptedTimeBits=oldTrace.times;
	certificate.residentInputRoots=oldTrace.inputs;
	certificate.residentPublicationRoots=oldTrace.outputs;
	certificate.residentKernels=oldTrace.kernels;
	return true;
}

// Called only after the native owner diagnostic has returned success into a
// fresh output directory. No log string or caller assertion proves execution.
bool ResidentExecutedOverlapRowsMatch(std::istream& csv,
	const ResumeEquivalenceCertificate& expected,std::string& error)
{
	const auto count=expected.acceptedStepCount;
	if(expected.schemaVersion!=2u||count<8u||
		expected.residentAcceptedTimeBits.size()!=count||expected.timeStepBits.size()!=count||
		expected.residentInputRoots.size()!=count||expected.residentPublicationRoots.size()!=count||
		expected.residentKernels.size()!=count){error="native overlap incomplete expectations";return false;}
	auto split=[](const std::string& row){
		std::vector<std::string> fields;std::istringstream input(row);std::string field;
		while(std::getline(input,field,','))fields.push_back(field);
		return fields;
	};
	std::string line;if(!std::getline(csv,line)){error="native overlap missing CSV";return false;}
	const auto header=split(line);std::map<std::string,std::size_t> columns;
	for(std::size_t i=0;i<header.size();++i)if(!columns.emplace(header[i],i).second){
		error="native overlap duplicate column";return false;}
	for(const char* name:{"accepted_step","time_s","dt_s","payload_digest_format",
		"payload_digest_version","input_payload_root_sha256","publication_payload_root_sha256",
		"qualified_kernel_set_sha256"})if(!columns.count(name)){
		error="native overlap missing column";return false;}
	std::size_t index=0u;
	while(std::getline(csv,line)){
		const auto fields=split(line);
		if(index>=count||fields.size()!=header.size()){error="native overlap row count";return false;}
		auto field=[&](const char* name)->const std::string&{return fields[columns.at(name)];};
		char* end=nullptr;
		const double time=std::strtod(field("time_s").c_str(),&end);
		if(!end||*end||!std::isfinite(time)||DoubleBits(time)!=expected.residentAcceptedTimeBits[index]){
			error="native overlap exact time mismatch";return false;}
		const double dt=std::strtod(field("dt_s").c_str(),&end);
		if(!end||*end||!std::isfinite(dt)||DoubleBits(dt)!=expected.timeStepBits[index]||
			field("accepted_step")!=std::to_string(expected.resumedFromStep+index+1u)||
			field("payload_digest_format")!="rise-payload-sha256-merkle"||field("payload_digest_version")!="2"||
			field("input_payload_root_sha256")!=expected.residentInputRoots[index]||
			field("publication_payload_root_sha256")!=expected.residentPublicationRoots[index]||
			field("qualified_kernel_set_sha256")!=expected.residentKernels[index]){
			error="native overlap step/dt/root/kernel mismatch";return false;}
		++index;
	}
	if(index!=count||!csv.eof()){error="native overlap truncated execution";return false;}
	return true;
}
