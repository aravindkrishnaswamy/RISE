//////////////////////////////////////////////////////////////////////
//
//  ClothParallel.cpp - Fork-join pool for the cloth solver
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "ClothParallel.h"

#include <chrono>

namespace RISE { namespace Simulation { namespace Cloth {

ParallelPool::ParallelPool(unsigned threads)
{
	for (unsigned k = 1; k < threads; ++k) workers_.emplace_back([this] { Work(); });
}

ParallelPool::~ParallelPool()
{
	{
		std::lock_guard<std::mutex> hold(mutex_);
		quit_ = true;
		generation_.fetch_add(1, std::memory_order_release);
	}
	wake_.notify_all();
	for (auto& w : workers_) w.join();
}

// Claims chunks of job `generation` until none are left or a newer job began.
void ParallelPool::Run(std::uint32_t generation, const std::function<void(std::size_t)>* fn, std::size_t chunks)
{
	for (;;) {
		std::uint64_t t = ticket_.load(std::memory_order_acquire);
		if (static_cast<std::uint32_t>(t >> 32) != generation) return;
		const std::size_t chunk = static_cast<std::size_t>(t & 0xffffffffu);
		if (chunk >= chunks) return;
		if (!ticket_.compare_exchange_weak(t, t + 1, std::memory_order_acq_rel)) continue;
		(*fn)(chunk);
		done_.fetch_add(1, std::memory_order_release);
	}
}

namespace {
// A cheap hint to the core that this thread is busy-waiting (no system call).
inline void SpinHint()
{
#if defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
	__asm__ __volatile__("pause" ::: "memory");
#endif
}
}

void ParallelPool::Work()
{
	std::uint32_t seen = 0;
	for (;;) {
		// Spin for a short while (jobs come in quick succession) without any
		// system call, then sleep.
		const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
		for (unsigned k = 0; generation_.load(std::memory_order_acquire) == seen; ++k) {
			SpinHint();
			if ((k & 127) == 127 && std::chrono::steady_clock::now() > spinUntil) break;
		}
		std::uint32_t generation;
		const std::function<void(std::size_t)>* fn;
		std::size_t chunks;
		{
			std::unique_lock<std::mutex> hold(mutex_);
			if (generation_.load(std::memory_order_acquire) == seen && !quit_) {
				++sleepers_;
				wake_.wait(hold, [&] { return quit_ || generation_.load(std::memory_order_acquire) != seen; });
				--sleepers_;
			}
			if (quit_) return;
			generation = generation_.load(std::memory_order_acquire);
			fn = fn_;
			chunks = chunks_;
		}
		seen = generation;
		Run(generation, fn, chunks);
	}
}

void ParallelPool::For(std::size_t chunks, const std::function<void(std::size_t)>& fn)
{
	if (chunks == 0) return;
	std::uint32_t generation;
	unsigned wake;
	{
		std::lock_guard<std::mutex> hold(mutex_);
		fn_ = &fn;
		chunks_ = chunks;
		done_.store(0, std::memory_order_relaxed);
		generation = generation_.load(std::memory_order_relaxed) + 1;
		ticket_.store(static_cast<std::uint64_t>(generation) << 32, std::memory_order_release);
		generation_.store(generation, std::memory_order_release);
		// The caller takes chunks too, so a job of c chunks needs c - 1 helpers at most.
		wake = static_cast<unsigned>(std::min<std::size_t>(sleepers_, chunks - 1));
	}
	for (unsigned k = 0; k < wake; ++k) wake_.notify_one();
	Run(generation, &fn, chunks);
	// Wait for the chunks other threads still hold: spin, and give the core up
	// only if that takes long (the holder may not be scheduled).
	const auto yieldAfter = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
	for (unsigned k = 0; done_.load(std::memory_order_acquire) < chunks; ++k) {
		SpinHint();
		if ((k & 255) == 255 && std::chrono::steady_clock::now() > yieldAfter) std::this_thread::yield();
	}
}

} } }
