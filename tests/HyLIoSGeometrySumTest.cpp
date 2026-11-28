// Deliberately include the implementation: these cancellation oracles target
// private arithmetic without adding public test hooks. Link against the normal
// archive; its geometry object is not pulled because this TU defines that API.
// Compile this test with strict FP, just like the implementation it includes.
#include "../src/Library/Materials/HyLIoS/HyLIoSParticleGeometry.cpp"

#include <array>
#include <iostream>
#include <limits>

namespace {
int passed=0,failed=0;
void Check(bool condition,const char* contract) {
	if (condition) { ++passed; std::cout << "ok: " << contract << '\n'; }
	else { ++failed; std::cerr << "FAIL: " << contract << '\n'; }
}
}
int main() {
	using RISE::HyLIoS::ParticleVector;
	for (double small : {1e-100,-1e-100,1e-300,-1e-300}) for (double large : {1e100,1e300}) {
		const double terms[3]={small,large,-large};
		std::array<int,3> order{0,1,2};
		bool dotCorrect=true,extendedCorrect=true;
		do {
			const ParticleVector values{terms[order[0]],terms[order[1]],terms[order[2]]};
			dotCorrect=dotCorrect && RISE::HyLIoS::Dot(values,{1,1,1})==small;
			const auto a=RISE::HyLIoS::ExtendedProduct({values.x},{});
			const auto b=RISE::HyLIoS::ExtendedProduct({values.y},{});
			const auto c=RISE::HyLIoS::ExtendedProduct({values.z},{});
			extendedCorrect=extendedCorrect && RISE::HyLIoS::Rounded(RISE::HyLIoS::ExtendedDot(a,b,c,{1,1,1}))==small;
		} while (std::next_permutation(order.begin(),order.end()));
		Check(dotCorrect,"compensated dot cancellation preserves tiny term for every input order");
		Check(extendedCorrect,"compensated exponent dot cancellation preserves tiny term before alignment for every input order");
	}
	// Exact (1+x)(1-x)-1 = -x*x: the rounded products alone sum to zero,
	// so this also checks that all FMA product residuals enter the expansion.
	Check(RISE::HyLIoS::Dot({1+0x1p-27,1,0},{1-0x1p-27,-1,0})==-0x1p-54,
		"compensated dot retains exact product residual after rounded products cancel");
	const double largest=std::numeric_limits<double>::max();
	Check(RISE::HyLIoS::Dot({largest,largest,-largest},{1,1,1})==largest,
		"compensated dot cancels opposite signs before an avoidable intermediate overflow");
	const double smallest=std::numeric_limits<double>::denorm_min();
	const auto tiny=RISE::HyLIoS::ExtendedProduct({smallest,smallest},{});
	const auto huge=RISE::HyLIoS::ExtendedProduct({largest,largest},{});
	const auto negative=RISE::HyLIoS::ExtendedProduct({-largest,largest},{});
	const auto result=RISE::HyLIoS::ExtendedDot(tiny,huge,negative,{1,1,1});
	Check(result.fraction==tiny.fraction && result.exponent==tiny.exponent,
		"compensated exponent dot cancellation retains full carried exponent range");
	std::cout << passed << " passed, " << failed << " failed\n";
	return failed ? 1 : 0;
}
