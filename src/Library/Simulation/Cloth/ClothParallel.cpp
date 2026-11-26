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

void ParallelPool::Work()
{
	std::uint32_t seen = 0;
	for (;;) {
		// Spin for a short while (jobs come in quick succession), then sleep.
		const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
		for (unsigned k = 0; generation_.load(std::memory_order_acquire) == seen; ++k) {
			if ((k & 255) == 255) {
				if (std::chrono::steady_clock::now() > spinUntil) break;
				std::this_thread::yield();
			}
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
	bool notify;
	{
		std::lock_guard<std::mutex> hold(mutex_);
		fn_ = &fn;
		chunks_ = chunks;
		done_.store(0, std::memory_order_relaxed);
		generation = generation_.load(std::memory_order_relaxed) + 1;
		ticket_.store(static_cast<std::uint64_t>(generation) << 32, std::memory_order_release);
		generation_.store(generation, std::memory_order_release);
		notify = sleepers_ > 0;
	}
	if (notify) wake_.notify_all();
	Run(generation, &fn, chunks);
	for (unsigned k = 0; done_.load(std::memory_order_acquire) < chunks; ++k)
		if ((k & 1023) == 1023) std::this_thread::yield();
}

} } }
