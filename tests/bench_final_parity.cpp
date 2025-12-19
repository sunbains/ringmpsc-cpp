// C++23 benchmark mirroring Zig bench_final.zig semantics:
// - One consumer per ring, spawned before producers
// - Producers use reserve (no backoff unless full)
// - Per-consumer atomic counters
// - Explicit ring close after producers join
// - Optional CPU pinning to mimic Zig affinity behavior

#include <ringmpsc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

using namespace ringmpsc;

namespace {

std::uint64_t parse_msgs(int argc, char** argv) {
    if (argc >= 2) {
        const auto v = std::strtoull(argv[1], nullptr, 10);
        if (v > 0) return v;
    }
    if (const char* env = std::getenv("BENCH_MSG")) {
        const auto v = std::strtoull(env, nullptr, 10);
        if (v > 0) return v;
    }
    return 1'000'000;
}

void pin_thread(std::size_t cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu % CPU_SETSIZE), &set);
    ::sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpu;
#endif
}

struct Result {
    double rate_billion_per_s = 0.0;
};

Result run_bench(std::size_t num_producers, std::uint64_t msgs_per_producer) {
    constexpr std::size_t BATCH = 32768;
    constexpr std::size_t RING_BITS = 16;
    constexpr std::size_t MAX_PRODUCERS = 8;
    constexpr Config cfg{.ring_bits = RING_BITS, .max_producers = MAX_PRODUCERS};

    using ChannelT = Channel<std::uint32_t, cfg>;
    ChannelT channel;

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    producers.reserve(num_producers);
    consumers.reserve(num_producers);

    std::vector<std::atomic<std::uint64_t>> consumed(num_producers);
    for (auto& c : consumed) c.store(0, std::memory_order_relaxed);

    // Register producers
    std::vector<ChannelT::Producer> regs;
    regs.reserve(num_producers);
    for (std::size_t i = 0; i < num_producers; ++i) {
        auto p = channel.register_producer();
        if (!p) {
            throw std::runtime_error("producer registration failed");
        }
        regs.push_back(*p);
    }

    // Consumers first (pin after producers in Zig; we pin by index here)
    for (std::size_t i = 0; i < num_producers; ++i) {
        auto ring = regs[i].ring;
        consumers.emplace_back([ring, i, &consumed] {
            pin_thread(i);
            struct Handler {
                std::atomic<std::uint64_t>* counter;
                inline void process(const std::uint32_t*) { counter->fetch_add(1, std::memory_order_relaxed); }
            } handler{.counter = &consumed[i]};

            while (true) {
                auto n = ring->consume_batch(handler);
                if (n == 0) {
                    if (ring->is_closed() && ring->is_empty()) break;
                    std::this_thread::yield();
                }
            }
        });
    }

    // Producers
    for (std::size_t i = 0; i < num_producers; ++i) {
        producers.emplace_back([prod = regs[i], i, msgs_per_producer] () mutable {
            pin_thread(i);
            constexpr std::size_t BATCH_LOCAL = BATCH;
            std::uint64_t sent = 0;
            while (sent < msgs_per_producer) {
                const auto want = std::min<std::uint64_t>(BATCH_LOCAL, msgs_per_producer - sent);
                if (auto r = prod.reserve(static_cast<std::size_t>(want))) {
                    for (std::size_t j = 0; j < r->slice.size(); ++j) {
                        r->slice[j] = static_cast<std::uint32_t>(sent + j);
                    }
                    prod.commit(r->slice.size());
                    sent += r->slice.size();
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    const auto start = std::chrono::steady_clock::now();

    for (auto& t : producers) t.join();
    // Close rings (Zig closes per-ring after producer join)
    for (std::size_t i = 0; i < num_producers; ++i) {
        regs[i].ring->close();
    }
    for (auto& t : consumers) t.join();

    const auto end = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::uint64_t total = 0;
    for (auto& c : consumed) total += c.load(std::memory_order_relaxed);

    const double rate = static_cast<double>(total) / static_cast<double>(ns); // msgs per ns
    return {.rate_billion_per_s = rate * 1e9 / 1e9}; // B msg/s
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t msgs_per_producer = parse_msgs(argc, argv);
    const std::size_t producer_counts[] = {1, 2, 4, 6, 8};

    std::cout << "C++ bench (parity): msgs/producer=" << msgs_per_producer << "\n";
    std::cout << "Producers | Throughput (B msg/s)\n";
    std::cout << "-------------------------------\n";
    for (auto p : producer_counts) {
        auto r = run_bench(p, msgs_per_producer);
        std::cout << p << "         | " << r.rate_billion_per_s << "\n";
    }
    return 0;
}
