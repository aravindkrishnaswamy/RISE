// DiskFileWriteBufferTrustedRootsTest: the staging-ancestor check of DiskFileWriteBuffer honours the opt-in
// trusted cache roots (RISE_SIMULATION_TRUSTED_ROOTS, AddTrustedCacheRoot) through the one shared policy
// (Simulation::FindTrustedCacheRoot), and is unchanged when none is configured. POSIX only.

#include "../src/Library/Utilities/DiskFileWriteBuffer.h"
#include "../src/Library/Simulation/SimulationCache.h"
#include "TestTempDir.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using RISE::Implementation::DiskFileWriteBuffer;
using RISE::Implementation::ForTest_CheckStagingParents;

namespace {
int failures = 0;
void Check(bool condition, const char* what)
{
	if (!condition) { ++failures; std::cout << "  FAIL: " << what << "\n"; }
}

#ifndef _WIN32
void Mode(const fs::path& path, fs::perms perms) { fs::permissions(path, perms, fs::perm_options::replace); }
const fs::perms ownerOnly = fs::perms::owner_all;
const fs::perms groupWritable = fs::perms::owner_all | fs::perms::group_all | fs::perms::others_read | fs::perms::others_exec;

std::string Read(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool Stage(const fs::path& target, const char* text)
{
	return DiskFileWriteBuffer::WriteAtomically(target.string(), [text](DiskFileWriteBuffer& buffer) {
		return buffer.setBytes(text, static_cast<unsigned int>(std::char_traits<char>::length(text)));
	}, DiskFileWriteBuffer::StagingPolicy::Required);
}

bool Accepts(const fs::path& directory)
{
	std::error_code error;
	return ForTest_CheckStagingParents(directory, error);
}

// Nothing but the destination is left behind by a refused or finished write.
std::size_t Entries(const fs::path& directory)
{
	return static_cast<std::size_t>(std::distance(fs::directory_iterator(directory), fs::directory_iterator()));
}

void Run(const fs::path& scratch)
{
	::unsetenv(RISE::Simulation::TrustedCacheRootsEnvironment);
	RISE::Simulation::ClearTrustedCacheRoots();
	std::string error;

	// A trusted root below a group-writable parent (a 0775 external drive), and the root itself group-writable.
	const fs::path drive = scratch / "drive";
	fs::create_directories(drive / "root" / "run" / "legacy");
	Mode(drive, groupWritable);
	Mode(drive / "root", groupWritable);
	Mode(drive / "root" / "run", ownerOnly);
	Mode(drive / "root" / "run" / "legacy", ownerOnly);
	const fs::path legacy = drive / "root" / "run" / "legacy";

	// Default: nothing trusted, a group-writable ancestor refuses staging and writes nothing.
	Check(!Accepts(legacy), "default: a group-writable ancestor is refused");
	Check(!Stage(legacy / "a.bin", "one") && !fs::exists(legacy / "a.bin") && Entries(legacy) == 0,
		"default: a staged write under a group-writable ancestor is refused and leaves nothing");

	// Trusted through the API: the same location is accepted and a file is staged and published.
	Check(RISE::Simulation::AddTrustedCacheRoot(drive / "root", error), "trust a root through the API");
	Check(Accepts(legacy), "trusted root: owner-only chain below it is accepted");
	Check(Stage(legacy / "a.bin", "one") && Read(legacy / "a.bin") == "one" && Entries(legacy) == 1,
		"trusted root: a file is staged and published, no staging directory left");
	Check(Stage(legacy / "a.bin", "two") && Read(legacy / "a.bin") == "two",
		"trusted root: republishing replaces the file");
	Check(DiskFileWriteBuffer::WriteNewFile((legacy / "new.bin").string(), [](DiskFileWriteBuffer& buffer) {
		return buffer.setBytes("n", 1);
	}) && Read(legacy / "new.bin") == "n", "trusted root: WriteNewFile is accepted too");
	// A trusted root does not trust a sibling location, or a path that merely shares its prefix.
	fs::create_directories(drive / "root2" / "x");
	Mode(drive / "root2", ownerOnly);
	Mode(drive / "root2" / "x", ownerOnly);
	Check(!Accepts(drive / "root2" / "x"), "trusted root: a sibling with a shared name prefix is not under it");

	// Below the root everything must still be owner-only: group-writable child refused, root group-writable.
	fs::create_directories(drive / "root" / "loose" / "legacy");
	Mode(drive / "root" / "loose", groupWritable);
	Mode(drive / "root" / "loose" / "legacy", ownerOnly);
	Check(!Accepts(drive / "root" / "loose" / "legacy") &&
		!Stage(drive / "root" / "loose" / "legacy" / "b.bin", "x") &&
		!fs::exists(drive / "root" / "loose" / "legacy" / "b.bin"),
		"trusted root (group-writable itself): a group-writable directory below it is refused");
	Check(!Accepts(drive / "root"), "trusted root: as the directory itself, a group-writable root is refused");
	// A sticky world-writable directory below the root is fine, as in the default policy.
	fs::create_directories(drive / "root" / "sticky" / "legacy");
	Mode(drive / "root" / "sticky", fs::perms::all | fs::perms::sticky_bit);
	Mode(drive / "root" / "sticky" / "legacy", ownerOnly);
	Check(Accepts(drive / "root" / "sticky" / "legacy"), "trusted root: sticky directories are accepted below it as before");

	// Symlinks: one in the chain below the root is refused by the descriptor walk (O_NOFOLLOW) ...
	fs::create_directories(drive / "root" / "real" / "legacy");
	Mode(drive / "root" / "real", ownerOnly);
	Mode(drive / "root" / "real" / "legacy", ownerOnly);
	fs::create_directory_symlink(drive / "root" / "real", drive / "root" / "link");
	Check(!Accepts(drive / "root" / "link" / "legacy"), "trusted root: a symlink in the chain below the root is refused");
	// ... and a symlink to an untrusted group-writable place resolves outside the root and is refused.
	fs::create_directories(drive / "elsewhere" / "legacy");
	Mode(drive / "elsewhere", groupWritable);
	Mode(drive / "elsewhere" / "legacy", ownerOnly);
	fs::create_directory_symlink(drive / "elsewhere", drive / "root" / "out");
	Check(!Stage(drive / "root" / "out" / "legacy" / "c.bin", "x") && !fs::exists(drive / "elsewhere" / "legacy" / "c.bin"),
		"trusted root: a symlink out of the root is checked at its target and refused");
	// Paths that are not canonical absolute spellings never use a root.
	Check(!Accepts(drive / "root" / "run" / ".." / "run" / "legacy"), "trusted root: a path with .. is not matched to a root");

	// Clearing the API roots restores the default refusal; the environment form behaves alike.
	RISE::Simulation::ClearTrustedCacheRoots();
	Check(!Accepts(legacy) && !Stage(legacy / "d.bin", "x"), "clearing the roots restores the refusal");
	::setenv(RISE::Simulation::TrustedCacheRootsEnvironment, ("relative/ignored:" + (drive / "root").string()).c_str(), 1);
	Check(Accepts(legacy) && Stage(legacy / "d.bin", "x") && Read(legacy / "d.bin") == "x",
		"the environment root is honoured");
	::unsetenv(RISE::Simulation::TrustedCacheRootsEnvironment);
	Check(!Accepts(legacy), "unsetting the environment restores the refusal");
}
#endif
}

int main()
{
#ifdef _WIN32
	std::cout << "DiskFileWriteBufferTrustedRootsTest: POSIX only\n";
	return 0;
#else
	const fs::path scratch = RISE::TestSupport::ScratchRunDir("rise_dfwb_trust_", "suite");
	try { fs::create_directories(scratch); Run(fs::canonical(scratch)); }
	catch (const std::exception& e) { ++failures; std::cout << "  FAIL: exception " << e.what() << "\n"; }
	std::cout << (failures ? "DiskFileWriteBufferTrustedRootsTest FAILED\n" : "DiskFileWriteBufferTrustedRootsTest passed\n");
	return failures ? 1 : 0;
#endif
}
