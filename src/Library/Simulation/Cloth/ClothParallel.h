//////////////////////////////////////////////////////////////////////
//
//  ClothParallel.h - A small fork-join pool for the cloth solver
//  (simulation M3a)
//
//  For(chunks, fn) runs fn(c) for every chunk c in [0, chunks) on the pool's
//  threads (the caller included) and returns when all are done.  The cloth
//  code only hands it work whose chunks write disjoint outputs, with every
//  sum inside a chunk in a fixed order and every reduction over chunks done
//  afterwards on the caller in chunk order, and the chunking never depends on
//  the thread count.  Results are therefore bitwise identical for any number
//  of threads, including none (a null pool runs the chunks in order).
//
//  Workers spin briefly between jobs (the jobs are short and frequent) and
//  then sleep on a condition variable.
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#ifndef RISE_SIMULATION_CLOTH_PARALLEL_H
#define RISE_SIMULATION_CLOTH_PARALLEL_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace RISE { namespace Simulation { namespace Cloth {

class ParallelPool {
public:
	//! `threads` counts the caller; 0 or 1 creates no workers.
	explicit ParallelPool(unsigned threads);
	~ParallelPool();
	ParallelPool(const ParallelPool&) = delete;
	ParallelPool& operator=(const ParallelPool&) = delete;
	unsigned Threads() const { return static_cast<unsigned>(workers_.size()) + 1; }
	void For(std::size_t chunks, const std::function<void(std::size_t)>& fn);
private:
	void Work();
	void Run(std::uint32_t generation, const std::function<void(std::size_t)>* fn, std::size_t chunks);
	std::vector<std::thread> workers_;
	std::mutex mutex_;                         // guards the job description and sleepers_
	std::condition_variable wake_;
	std::atomic<std::uint32_t> generation_{0}; // job number
	std::atomic<std::uint64_t> ticket_{0};     // generation << 32 | next chunk
	std::atomic<std::size_t> done_{0};         // chunks finished in the current job
	std::size_t chunks_ = 0;
	const std::function<void(std::size_t)>* fn_ = nullptr;
	unsigned sleepers_ = 0;
	bool quit_ = false;
};

//! Runs f(c) for c in [0, chunks): on the pool when there is one and more
//! than one chunk, else in order on the caller.
template <class F>
void ParallelFor(ParallelPool* pool, std::size_t chunks, F&& f)
{
	if (pool && chunks > 1 && pool->Threads() > 1) {
		const std::function<void(std::size_t)> fn = std::ref(f);
		pool->For(chunks, fn);
	} else for (std::size_t c = 0; c < chunks; ++c) f(c);
}

//! Number of chunks of `size` items covering n.
inline std::size_t Chunks(std::size_t n, std::size_t size) { return (n + size - 1)/size; }

} } }

#endif
