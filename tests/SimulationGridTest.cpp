#include "../src/Library/Simulation/SimulationGrid.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace RISE::Simulation;

namespace {
using Bytes = std::vector<std::uint8_t>;
int failures=0;
constexpr std::size_t Budget=16*1024*1024;

void Check(bool condition,const char* message)
{
	if (!condition) { ++failures; std::fprintf(stderr,"FAIL: %s\n",message); }
}

Bytes Golden()
{
	// Independent fixture, assembled from the wire table using Python struct.
	// Channel A (dimensionless), cells [-2,2) x [3,5) x [0,1).
	const char* hex=
		"5249534547524944010000000100000001004101003101feffffff0300000000"
		"0000000400000002000000010000000000000000000040000000000000f03f00"
		"0000000000000000000000000024400000000000000000000000000000084000"
		"0000000000000000000000000010c00000000000000000000000000000000000"
		"0000000000d03f0000000000001c400000000000000000000000000000000000"
		"00000000000000000000000000f03f0000008002000000feffffff0300000000"
		"00000002000000020000000100000001000000803f0000000003000000000000"
		"00020000000200000001000000000900000080000000000000004000004040";
	auto digit=[](char c){ return c>='a' ? c-'a'+10 : c-'0'; };
	Bytes result;
	for (const char* p=hex; *p; p+=2) result.push_back(static_cast<std::uint8_t>(16*digit(p[0])+digit(p[1])));
	return result;
}

GridFrame Fixture()
{
	GridFrame frame;
	GridChannel c;
	c.name="A";
	c.units="1";
	c.origin={{-2,3,0}};
	c.dimensions={{4,2,1}};
	c.indexToWorld={{2,1,0,10, 0,3,0,-4, 0,0,.25,7, 0,0,0,1}};
	c.background[0]=0x80000000u;
	GridBlock tile;
	tile.origin=c.origin;
	tile.dimensions={{2,2,1}};
	tile.constant=true;
	tile.active={0};
	tile.values={0x3f800000};
	GridBlock dense;
	dense.origin={{0,3,0}};
	dense.dimensions={{2,2,1}};
	dense.active={9};
	dense.values={0x80000000u,0,0x40000000,0x40400000};
	c.blocks={tile,dense};
	frame.channels.push_back(c);
	return frame;
}

void PositiveTests()
{
	const auto frame=Fixture();
	const auto golden=Golden();
	std::string error;
	Bytes bytes;
	Check(EncodeGrid(frame,Budget,bytes,error) && bytes==golden,"native grid canonical golden bytes");
	GridFrame read;
	if (!DecodeGrid(golden,Budget,read,error) || read.channels.size()!=1) {
		Check(false,"decode independently encoded grid");
		return;
	}
	const auto& c=read.channels[0];
	Check(c.name=="A" && c.units=="1" && c.components==1 && c.origin==frame.channels[0].origin &&
		c.dimensions==frame.channels[0].dimensions && c.background[0]==0x80000000u,
		"signed domain and negative-zero background");
	Check(c.indexToWorld==frame.channels[0].indexToWorld &&
		c.indexToWorld[0]*-2+c.indexToWorld[1]*3+c.indexToWorld[3]==9 &&
		c.indexToWorld[5]*3+c.indexToWorld[7]==5,"known index (-2,3,0) maps to (9,5,7)");
	if (c.blocks.size()!=2) { Check(false,"two independent blocks preserved"); return; }
	Check(c.blocks[0].constant && c.blocks[0].active==Bytes{0} &&
		c.blocks[0].values==frame.channels[0].blocks[0].values,"inactive nonbackground tile");
	Check(!c.blocks[1].constant && c.blocks[1].active==Bytes{9} &&
		c.blocks[1].values==frame.channels[0].blocks[1].values,"dense mask and signed-zero active sample");

	auto vectorFrame=frame;
	auto& vector=vectorFrame.channels.emplace_back();
	vector.name="V"; vector.units="m/s"; vector.components=3;
	vector.dimensions={{1000,100,24}};
	vector.background={{0x00000001,0x80000000u,0x3f800000}};
	auto& block=vector.blocks.emplace_back();
	block.constant=true;
	block.origin={{10,70,0}};
	block.dimensions={{3,30,24}};
	block.active={1};
	block.values={0x3f800000,0,0x80000000u};
	constexpr auto limit=64*1024*1024;
	Check(EncodeGrid(vectorFrame,limit,bytes,error) && bytes.size()<1024,"sparse vector tile remains compact");
	GridFrame decoded;
	Check(DecodeGrid(bytes,limit,decoded,error) && decoded.channels.size()==2 &&
		decoded.channels[1].background==vector.background &&
		decoded.channels[1].blocks.size()==1 &&
		decoded.channels[1].blocks.front().values==block.values,"sparse vec3 topology and opaque component bits");
}

void NegativeTests()
{
	std::string error;
	Bytes sentinel{3,1,4};
	const auto expected=sentinel;
	auto rejects=[&](const GridFrame& bad) {
		return !EncodeGrid(bad,Budget,sentinel,error) && !error.empty() && sentinel==expected;
	};
	auto frame=Fixture();
	auto& c=frame.channels.front();
	c.blocks[1].origin[0]=-1;
	Check(rejects(frame),"overlap rejected with encoder output unchanged");
	c.blocks[1].origin[0]=0;
	c.blocks[1].active[0]=0x80;
	Check(rejects(frame),"unused mask bits refused");
	c.blocks[1].active[0]=1;
	c.blocks[1].values.pop_back();
	Check(rejects(frame),"wrong dense value count refused");
	frame=Fixture(); frame.channels[0].origin[0]=(std::numeric_limits<std::int32_t>::max)();
	Check(rejects(frame),"overflow of last cell coordinate refused");
	frame=Fixture(); frame.channels[0].indexToWorld[0]=0;
	Check(rejects(frame),"singular affine transform refused");
	frame=Fixture(); frame.channels[0].indexToWorld={{4,1,1,0, 1,1,1,0, 5,2,2,0, 0,0,0,1}};
	Check(rejects(frame),"linearly dependent nonzero columns refused exactly");
	frame=Fixture(); frame.channels.push_back(frame.channels.front());
	Check(rejects(frame),"duplicate channel refused");
	frame=Fixture(); frame.channels[0].units="e\xcc\x81";
	Check(rejects(frame),"non-NFC text refused");

	const auto golden=Golden();
	GridFrame retained=Fixture();
	retained.channels.front().name="previous";
	auto refuses=[&](const Bytes& input,std::size_t limit) {
		return !DecodeGrid(input,limit,retained,error) && !error.empty() &&
			retained.channels.size()==1 && retained.channels[0].name=="previous";
	};
	for (std::size_t n=0;n<golden.size();++n) {
		if (!refuses(Bytes(golden.begin(),golden.begin()+n),Budget)) {
			Check(false,"every truncated wire prefix rejected transactionally");
			break;
		}
	}
	Bytes corrupt=golden;
	corrupt.push_back(0);
	Check(refuses(corrupt,Budget),"trailing byte rejected");
	corrupt=golden; corrupt[8]=2;
	Check(refuses(corrupt,Budget),"unknown format version rejected");
	corrupt=golden;
	std::fill(corrupt.begin()+12,corrupt.begin()+16,0xff);
	Check(refuses(corrupt,Budget),"unbounded channel count refused before allocation");
	corrupt=golden;
	// Origin of second block starts at byte 213 in the fixed golden.
	std::fill(corrupt.begin()+213,corrupt.begin()+217,0xff);
	Check(refuses(corrupt,Budget),"encoded intersecting block refused");
	Check(refuses(golden,golden.size()),"aggregate decode budget enforced");
	corrupt=golden;
	std::fill(corrupt.begin()+35,corrupt.begin()+39,0x7f);
	Check(refuses(corrupt,Budget),"sparse logical expansion refused before allocation");
}

void TextBudgetTest()
{
	GridFrame frame;
	for (char suffix='A';suffix<='Z';++suffix) {
		auto& channel=frame.channels.emplace_back();
		channel.name=std::string(31,'N')+suffix;
		channel.units=std::string(32,'u');
		channel.dimensions={{1,1,1}};
	}
	Bytes bytes;
	std::string error;
	GridFrame decoded;
	if (!EncodeGrid(frame,Budget,bytes,error) || !DecodeGrid(bytes,Budget,decoded,error)) {
		Check(false,"32-byte descriptor strings encode and decode");
		return;
	}
	auto retained=bytes.capacity()+decoded.channels.capacity()*sizeof(GridChannel);
	for (const auto& channel:decoded.channels)
		retained+=channel.name.capacity()+1+channel.units.capacity()+1;
	const auto logical=5*decoded.channels.size(); // one binary32 and one mask byte per channel
	GridFrame destination;
	Check(!DecodeGrid(bytes,retained+logical-1,destination,error) && destination.channels.empty(),
		"decoder accounts for all retained string capacities and logical cells");
}
}

int main()
{
	PositiveTests();
	NegativeTests();
	TextBudgetTest();
	if (failures==0) std::puts("SimulationGridTest passed");
	return failures ? 1 : 0;
}
