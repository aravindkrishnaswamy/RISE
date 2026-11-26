// Opt-in fast mode for the fire production solver (RISE_FIRE_FAST_MODE=1).
//
// Same physical model and discretisation as the certified path, without the
// machinery that exists only to certify correctly-rounded results and per-step
// provenance: the resident EOS inversion and divergence-target terms use plain
// compensated binary32 arithmetic instead of interval-certified extended
// precision, and per-step identity hashing/seal re-checks are skipped. Physical
// guards (positivity, temperature range, pressure-ratio validity, finiteness)
// still fail the step, and an exact per-step conservation ledger is checked (see FireSequenceTest).
// Unset (or "0") leaves every code path and every output byte unchanged.
#ifndef RISE_FIRE_FAST_MODE_H
#define RISE_FIRE_FAST_MODE_H

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace RISE
{
	namespace FireSim
	{
		// Read once per process; the first call prints one line when fast mode is on.
		inline bool FireFastMode()
		{
			static const bool enabled=[](){
				const char* value=std::getenv("RISE_FIRE_FAST_MODE");
				if(!value||!*value||std::strcmp(value,"0")==0)return false;
				if(std::strcmp(value,"1")!=0){
					std::fprintf(stderr,"RISE_FIRE_FAST_MODE=%s is not 0 or 1; fast mode stays OFF\n",value);
					return false;}
				std::fprintf(stderr,"RISE_FIRE_FAST_MODE=1: fire solver fast mode is ON; results are NOT certified "
					"(uncertified binary32 EOS/target arithmetic, no per-step seals or digests, not bit-reproducible)\n");
				return true;}();
			return enabled;
		}

		// r271 opt-in (RISE_FIRE_LIMITED_MOMENTUM_CARRIER=1, read once per process): carry momentum
		// with the scalar transport's accepted (limited) gas mass flux instead of the r264 centred
		// carrier, the production default. Shared by the Metal owner and the M8 CPU backend.
		inline bool FireLimitedMomentumCarrier()
		{
			static const bool limited=[](){const char* value=std::getenv("RISE_FIRE_LIMITED_MOMENTUM_CARRIER");
				return value&&std::strcmp(value,"1")==0;}();
			return limited;
		}

		// Fast mode stops the pressure projection's fixed V-cycle schedule once the unchanged validation
		// band is met. RISE_FIRE_FAST_FIXED_VCYCLES=1 keeps the certified fixed count instead, so fast mode
		// then differs from the certified path only by binary32 rounding (slower).
		inline bool FireFastAdaptiveVCycles()
		{
			static const bool adaptive=[](){
				if(!FireFastMode())return false;
				const char* value=std::getenv("RISE_FIRE_FAST_FIXED_VCYCLES");
				if(value&&std::strcmp(value,"1")==0){
					std::fprintf(stderr,"RISE_FIRE_FAST_FIXED_VCYCLES=1: fast mode keeps the fixed V-cycle count\n");
					return false;}
				return true;}();
			return adaptive;
		}
	}
}

#endif
