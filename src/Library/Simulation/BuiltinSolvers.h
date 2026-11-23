// Simulation M3c: one call that registers the solvers shipped in the library
// with the solver registry, for applications (the GUI bridges call it once
// before attaching a live simulation).  A static library drops
// self-registering objects nothing references, so registration is explicit.
#ifndef RISE_SIMULATION_BUILTIN_SOLVERS_H
#define RISE_SIMULATION_BUILTIN_SOLVERS_H

#include <string>

namespace RISE { namespace Simulation {

//! Registers cloth.shell and, where the library builds it, fire.methane.
//! Idempotent and thread-safe.  False with `error` naming the solvers that
//! could not be registered; the others stay registered.  The registry itself
//! does not depend on any solver or backend.
bool RegisterBuiltinSolvers( std::string& error );

} }
#endif
